// SPDX-License-Identifier: GPL-2.0-or-later
#include "LanDatagram.h"
#include <algorithm>
#include <bitset>
#include <chrono>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <boost/asio/ip/address_v4.hpp>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace WiiU3DSLink {
namespace {
using Bytes=std::vector<std::uint8_t>;
using Salt=std::array<std::uint8_t,32>;
using Clock=std::chrono::steady_clock;
constexpr std::array<std::uint8_t,8> Magic{'C','W','3','L','A','N','0','1'};
constexpr std::array<std::uint8_t,8> DirectMagic{'C','W','3','D','I','R','0','1'};
constexpr std::size_t Header=60,Tag=16,Chunk=1024,Maximum=2092;
void Put64(std::span<std::uint8_t> b,std::size_t p,std::uint64_t n){for(int i=7;i>=0;--i){b[p+static_cast<std::size_t>(i)]=static_cast<std::uint8_t>(n);n>>=8;}}
std::uint64_t Get64(std::span<const std::uint8_t> b,std::size_t p){std::uint64_t n{};for(unsigned i=0;i<8;++i)n=(n<<8)|b[p+i];return n;}
int Hex(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
#ifdef _WIN32
void Check(NTSTATUS status){if(status<0)throw std::runtime_error("Windows authenticated LAN crypto failed");}
class Algorithms {
public:
    BCRYPT_ALG_HANDLE aes{},hmac{};
    Algorithms(){
        Check(BCryptOpenAlgorithmProvider(&aes,BCRYPT_AES_ALGORITHM,nullptr,0));
        try {
            Check(BCryptSetProperty(aes,BCRYPT_CHAINING_MODE,reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),sizeof(BCRYPT_CHAIN_MODE_GCM),0));
            Check(BCryptOpenAlgorithmProvider(&hmac,BCRYPT_SHA256_ALGORITHM,nullptr,BCRYPT_ALG_HANDLE_HMAC_FLAG));
        }catch(...){BCryptCloseAlgorithmProvider(aes,0);throw;}
    }
    ~Algorithms(){if(hmac)BCryptCloseAlgorithmProvider(hmac,0);if(aes)BCryptCloseAlgorithmProvider(aes,0);}
};
Salt Derive(BCRYPT_ALG_HANDLE algorithm,const Salt& secret,const Salt& salt,bool passphrase=false){
    Salt result{};BCRYPT_HASH_HANDLE hash{};
    if(passphrase){
        Check(BCryptDeriveKeyPBKDF2(algorithm,const_cast<PUCHAR>(secret.data()),static_cast<ULONG>(secret.size()),
            const_cast<PUCHAR>(salt.data()),static_cast<ULONG>(salt.size()),600000,result.data(),static_cast<ULONG>(result.size()),0));
        return result;
    }
    Check(BCryptCreateHash(algorithm,&hash,nullptr,0,const_cast<PUCHAR>(secret.data()),static_cast<ULONG>(secret.size()),0));
    try {
        constexpr char domain[]="Cemu-AzaharX-LAN-v1 sender key";
        Check(BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(domain)),sizeof(domain)-1,0));
        Check(BCryptHashData(hash,const_cast<PUCHAR>(salt.data()),static_cast<ULONG>(salt.size()),0));
        Check(BCryptFinishHash(hash,result.data(),static_cast<ULONG>(result.size()),0));
    }catch(...){BCryptDestroyHash(hash);throw;}
    BCryptDestroyHash(hash);return result;
}
class AesKey {
    BCRYPT_KEY_HANDLE key{};
public:
    AesKey(BCRYPT_ALG_HANDLE algorithm,Salt bytes){
        const auto status=BCryptGenerateSymmetricKey(algorithm,&key,nullptr,0,bytes.data(),static_cast<ULONG>(bytes.size()),0);
        SecureZeroMemory(bytes.data(),bytes.size());Check(status);
    }
    ~AesKey(){if(key)BCryptDestroyKey(key);}
    AesKey(const AesKey&)=delete;
    bool Crypt(bool encrypt,std::span<const std::uint8_t> input,std::span<const std::uint8_t> header,std::span<std::uint8_t> tag,Bytes& output){
        std::array<std::uint8_t,12> nonce{};Put64(nonce,4,Get64(header,40));
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce=nonce.data();info.cbNonce=static_cast<ULONG>(nonce.size());
        info.pbAuthData=const_cast<PUCHAR>(header.data());info.cbAuthData=static_cast<ULONG>(header.size());
        info.pbTag=tag.data();info.cbTag=static_cast<ULONG>(tag.size());
        output.resize(input.size());ULONG count{};
        const auto operation=encrypt?BCryptEncrypt:BCryptDecrypt;
        const auto status=operation(key,const_cast<PUCHAR>(input.data()),static_cast<ULONG>(input.size()),&info,nullptr,0,output.data(),static_cast<ULONG>(output.size()),&count,0);
        if(status<0 || count!=input.size()){if(!output.empty())SecureZeroMemory(output.data(),output.size());output.clear();return false;}
        return true;
    }
};
#endif
}

std::optional<LanOptions> ParseLanInvite(const std::string& invite){
    constexpr std::string_view prefix="CW3LAN1:";
    if(invite.size()>128 || !invite.starts_with(prefix))return {};
    const auto colon=invite.find(':',prefix.size()),last=invite.find(':',colon==std::string::npos?invite.size():colon+1);
    if(colon==std::string::npos || last==std::string::npos || invite.size()-last-1!=64)return {};
    LanOptions options;options.address=invite.substr(prefix.size(),colon-prefix.size());
    boost::system::error_code error;const auto address=boost::asio::ip::make_address_v4(options.address,error);
    if(error)return {};
    const auto b=address.to_bytes();
    if(!(b[0]==10 || (b[0]==172&&b[1]>=16&&b[1]<=31) || (b[0]==192&&b[1]==168) || (b[0]==169&&b[1]==254)))return {};
    unsigned port{};
    if(last==colon+1 || last-colon>6)return {};
    for(std::size_t i=colon+1;i<last;++i){if(invite[i]<'0'||invite[i]>'9')return {};port=port*10+static_cast<unsigned>(invite[i]-'0');}
    if(port<1024 || port>65535)return {};options.port=static_cast<std::uint16_t>(port);
    for(std::size_t i=0;i<options.key.size();++i){const auto a=Hex(invite[last+1+i*2]),c=Hex(invite[last+2+i*2]);if(a<0||c<0)return {};options.key[i]=static_cast<std::uint8_t>((a<<4)|c);}
    if(std::all_of(options.key.begin(),options.key.end(),[](auto b){return b==0;}))return {};
    return options;
}
std::optional<LanOptions> LanOptionsFromEnvironment(){
#ifdef _WIN32
    char value[160]{};const auto length=GetEnvironmentVariableA("CEMU_3DS_LAN_INVITE",value,sizeof(value));
    if(!length)return {};
    if(length>=sizeof(value))throw std::runtime_error("LAN connection code is invalid");
    auto options=ParseLanInvite(value);SecureZeroMemory(value,sizeof(value));
    if(!options)throw std::runtime_error("LAN connection code must name a private IPv4 address, port and256-bit invitation key");
    return options;
#else
    return {};
#endif
}

LanOptions DirectOptions(bool host,const std::string& address,std::uint16_t port,std::string passphrase){
#ifdef _WIN32
    struct Erase {std::string& value;~Erase(){if(!value.empty())SecureZeroMemory(value.data(),value.size());}} erase{passphrase};
    if(port<1024)throw std::invalid_argument("Use a UDP port between 1024 and 65535.");
    boost::system::error_code error;const auto ip=boost::asio::ip::make_address_v4(address,error);
    if(error)throw std::invalid_argument("Enter a numeric IPv4 address, not a hostname.");
    const auto bytes=ip.to_bytes();
    if((!host&&ip.is_unspecified()) || (!ip.is_unspecified()&&bytes[0]==0) || bytes[0]>=224)
        throw std::invalid_argument("Use a unicast IPv4 address (0.0.0.0 is allowed only for hosting).");
    if(passphrase.size()<16 || passphrase.size()>256 ||
        std::any_of(passphrase.begin(),passphrase.end(),[](unsigned char c){return c<32 || c==127;}) ||
        std::all_of(passphrase.begin(),passphrase.end(),[](unsigned char c){return c==' ';}))
        throw std::invalid_argument("Use a strong passphrase of 16 to 256 UTF-8 bytes, without control characters.");
    LanOptions options{address,port,{},true};
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    Check(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0));
    try{
        Check(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0));
        constexpr char domain[]="Cemu-AzaharX-Direct-v1 passphrase seed";
        Check(BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(domain)),sizeof(domain)-1,0));
        Check(BCryptHashData(hash,reinterpret_cast<PUCHAR>(passphrase.data()),static_cast<ULONG>(passphrase.size()),0));
        Check(BCryptFinishHash(hash,options.key.data(),static_cast<ULONG>(options.key.size()),0));
    }catch(...){if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);throw;}
    BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
    return options;
#else
    (void)host;(void)address;(void)port;(void)passphrase;
    throw std::runtime_error("This private direct-link candidate requires Windows.");
#endif
}
std::string GenerateDirectPassphrase(){
#ifdef _WIN32
    std::array<std::uint8_t,16> bytes{};Check(BCryptGenRandom(nullptr,bytes.data(),static_cast<ULONG>(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    constexpr char hex[]="0123456789abcdef";std::string result;result.reserve(bytes.size()*2);
    for(auto b:bytes){result+=hex[b>>4];result+=hex[b&15];}
    SecureZeroMemory(bytes.data(),bytes.size());return result;
#else
    throw std::runtime_error("This private direct-link candidate requires Windows.");
#endif
}

#ifdef _WIN32
struct LanDatagram::Impl {
    struct Receiver {
        std::unique_ptr<AesKey> key;
        std::uint64_t highest{};std::bitset<256> seen;
        bool Replayed(std::uint64_t counter)const{return !counter || (counter<=highest && (highest-counter>=seen.size() || seen.test(static_cast<std::size_t>(highest-counter))));}
        void Mark(std::uint64_t counter){if(counter>highest){const auto shift=counter-highest;if(shift>=seen.size())seen.reset();else seen<<=static_cast<std::size_t>(shift);highest=counter;}seen.set(static_cast<std::size_t>(highest-counter));}
    };
    struct Assembly {Clock::time_point start;std::uint16_t size{};unsigned mask{};Bytes bytes;};
    Algorithms algorithms;Salt secret{},salt{};
    std::unique_ptr<AesKey> sender;
    std::map<Salt,Receiver> receivers;
    std::map<std::pair<Salt,std::uint64_t>,Assembly> assemblies;
    std::uint64_t counter{},message{};
    bool passphrase{};
    Clock::time_point attempt_window{};unsigned attempts{};
    explicit Impl(const Salt& key,bool password):secret(key),passphrase(password){
        Check(BCryptGenRandom(nullptr,salt.data(),static_cast<ULONG>(salt.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG));
        sender=std::make_unique<AesKey>(algorithms.aes,Derive(algorithms.hmac,secret,salt,passphrase));
    }
    ~Impl(){SecureZeroMemory(secret.data(),secret.size());}
};
LanDatagram::LanDatagram(const Salt& key,bool passphrase):impl(std::make_unique<Impl>(key,passphrase)){}
LanDatagram::~LanDatagram()=default;
std::vector<Bytes> LanDatagram::Seal(std::span<const std::uint8_t> bytes){
    if(bytes.empty() || bytes.size()>Maximum)throw std::runtime_error("LAN frame length outside native bounds");
    if(impl->counter>std::numeric_limits<std::uint64_t>::max()-3 || impl->message==std::numeric_limits<std::uint64_t>::max())throw std::runtime_error("LAN session exhausted; reconnect");
    std::vector<Bytes> packets;const auto message=++impl->message;
    const auto parts=(bytes.size()+Chunk-1)/Chunk;
    for(std::size_t part=0;part<parts;++part){
        const auto input=bytes.subspan(part*Chunk,std::min(Chunk,bytes.size()-part*Chunk));
        Bytes packet(Header+input.size()+Tag),cipher;
        const auto& magic=impl->passphrase?DirectMagic:Magic;
        std::copy(magic.begin(),magic.end(),packet.begin());std::copy(impl->salt.begin(),impl->salt.end(),packet.begin()+8);
        Put64(packet,40,++impl->counter);Put64(packet,48,message);packet[56]=static_cast<std::uint8_t>(part);packet[57]=static_cast<std::uint8_t>(parts);
        packet[58]=static_cast<std::uint8_t>(bytes.size()>>8);packet[59]=static_cast<std::uint8_t>(bytes.size());
        if(!impl->sender->Crypt(true,input,std::span(packet).first(Header),std::span(packet).last(Tag),cipher))throw std::runtime_error("LAN frame encryption failed");
        std::copy(cipher.begin(),cipher.end(),packet.begin()+Header);packets.push_back(std::move(packet));
    }
    return packets;
}
LanResult LanDatagram::Open(std::span<const std::uint8_t> packet){
    const auto& magic=impl->passphrase?DirectMagic:Magic;
    if(packet.size()<Header+1+Tag || packet.size()>Header+Chunk+Tag || !std::equal(magic.begin(),magic.end(),packet.begin()))return {};
    const auto size=(std::size_t{packet[58]}<<8)|packet[59],part=std::size_t{packet[56]},parts=std::size_t{packet[57]};
    if(!size || size>Maximum || parts!=(size+Chunk-1)/Chunk || part>=parts || packet.size()!=Header+std::min(Chunk,size-part*Chunk)+Tag)return {};
    const auto counter=Get64(packet,40),message=Get64(packet,48);if(!counter||!message)return {};
    Salt salt{};std::copy_n(packet.begin()+8,salt.size(),salt.begin());
    auto receiver=impl->receivers.find(salt);
    if(receiver!=impl->receivers.end() && receiver->second.Replayed(counter))return {};
    std::unique_ptr<AesKey> candidate;
    if(receiver==impl->receivers.end()){
        if(impl->receivers.size()>=64)return {};
        if(impl->passphrase){
            const auto now=Clock::now();
            if(now-impl->attempt_window>=std::chrono::seconds(1)){impl->attempt_window=now;impl->attempts=0;}
            if(impl->attempts>=2)return {};
            ++impl->attempts;
        }
        candidate=std::make_unique<AesKey>(impl->algorithms.aes,Derive(impl->algorithms.hmac,impl->secret,salt,impl->passphrase));
    }
    Bytes plain;std::array<std::uint8_t,Tag> tag{};std::copy_n(packet.end()-Tag,Tag,tag.begin());
    auto& key=receiver==impl->receivers.end()?*candidate:*receiver->second.key;
    if(!key.Crypt(false,packet.subspan(Header,packet.size()-Header-Tag),packet.first(Header),tag,plain))return {};
    if(receiver==impl->receivers.end())receiver=impl->receivers.emplace(salt,Impl::Receiver{std::move(candidate),0,{}}).first;
    receiver->second.Mark(counter);
    const auto now=Clock::now();
    std::erase_if(impl->assemblies,[&](const auto& a){return now-a.second.start>std::chrono::milliseconds(250);});
    if(parts==1)return {LanRead::Complete,std::move(plain)};
    const auto id=std::pair{salt,message};auto found=impl->assemblies.find(id);
    if(found==impl->assemblies.end()){
        if(impl->assemblies.size()>=16)return {};
        found=impl->assemblies.emplace(id,Impl::Assembly{now,static_cast<std::uint16_t>(size),0,Bytes(size)}).first;
    }
    auto& assembly=found->second;
    if(assembly.size!=size || (assembly.mask&(1U<<part)))return {};
    std::copy(plain.begin(),plain.end(),assembly.bytes.begin()+part*Chunk);assembly.mask|=1U<<part;
    if(assembly.mask!=(1U<<parts)-1)return {LanRead::Pending,{}};
    auto bytes=std::move(assembly.bytes);impl->assemblies.erase(found);return {LanRead::Complete,std::move(bytes)};
}
#else
struct LanDatagram::Impl{};
LanDatagram::LanDatagram(const std::array<std::uint8_t,32>&,bool){throw std::runtime_error("This private LAN candidate requires Windows");}
LanDatagram::~LanDatagram()=default;
std::vector<std::vector<std::uint8_t>> LanDatagram::Seal(std::span<const std::uint8_t>){return {};}
LanResult LanDatagram::Open(std::span<const std::uint8_t>){return {};}
#endif
}
