// SPDX-License-Identifier: GPL-2.0-or-later
// Wire layouts are interoperable with the GPL Citra/Azahar UDS protocol.
#include "UdsHost.h"
#include "NativeFrames.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace WiiU3DSLink {
namespace {
using Bytes = std::vector<std::uint8_t>;
void Put16(Bytes& b, std::size_t p, std::uint16_t v) { b[p]=static_cast<std::uint8_t>(v>>8); b[p+1]=static_cast<std::uint8_t>(v); }
void Put32(Bytes& b, std::size_t p, std::uint32_t v, bool little = false) {
    for (unsigned i=0;i<4;++i) b[p+i]=static_cast<std::uint8_t>(v >> ((little ? i : 3-i)*8));
}
void Put64(Bytes& b, std::size_t p, std::uint64_t v) {
    for (unsigned i=0;i<8;++i) b[p+i]=static_cast<std::uint8_t>(v >> ((7-i)*8));
}
std::uint64_t Get64(std::span<const std::uint8_t> b, std::size_t p) {
    std::uint64_t v=0; for(unsigned i=0;i<8;++i) v=(v<<8)|b[p+i]; return v;
}
void LLC(Bytes& b, std::uint16_t protocol) { b[0]=b[1]=0xAA; b[2]=3; Put16(b,6,protocol); }
void Tag(Bytes& b, std::uint8_t tag, const Bytes& data) {
    if (data.size()>255) throw std::runtime_error("UDS tag overflow");
    b.push_back(tag); b.push_back(static_cast<std::uint8_t>(data.size()));
    b.insert(b.end(),data.begin(),data.end());
}
constexpr bool IsNode(std::uint16_t node) { return node>=1 && node<=16; }
}

UdsHost::UdsHost(UdsCrypto operations) : crypto(std::move(operations)) {}
UdsResult UdsHost::Initialize(const UdsNode& identity, std::size_t size) {
    if (initialized) return UdsResult::Busy;
    if (size<=1536 || (size&63)) return UdsResult::Argument;
    Finalize(); initialized=true; memory_size=size; nodes[0]=identity; nodes[0].id=1;
    status.state=3; return UdsResult::Ok;
}
void UdsHost::Finalize() {
    initialized=allowed=false; status={}; nodes={}; endpoints={}; authenticating={};
    outgoing.clear(); app_data.clear(); state_change=false; last_beacon=0;
    received=transmitted=rejected=0;
}
void UdsHost::Changed(std::uint16_t mask) { status.changed|=mask; state_change=true; }
UdsStatus UdsHost::TakeStatus() { auto result=status; status.changed=0; state_change=false; return result; }
UdsResult UdsHost::CreateNetwork(std::uint8_t sub_id, std::uint8_t maximum, std::uint32_t comm,
                                std::uint8_t physical_channel, std::uint32_t network,
                                std::span<const std::uint8_t> data) {
    if (!initialized) return UdsResult::NotInitialized;
    if (status.state!=3) return UdsResult::State;
    if (sub_id==255 || maximum<2 || maximum>9 || data.size()>200 || network==0 ||
        (physical_channel!=0 && physical_channel!=1 && physical_channel!=6 && physical_channel!=11))
        return UdsResult::Argument;
    id=sub_id; channel=physical_channel ? physical_channel : 6;
    communication_id=comm; network_id=network; app_data.assign(data.begin(),data.end());
    status={6,1,1,0,1,1,maximum}; allowed=true; sequence=0; last_beacon=0;
    Changed(1); return UdsResult::Ok;
}
UdsResult UdsHost::DestroyNetwork(std::uint32_t reason) {
    if (!initialized) return UdsResult::NotInitialized;
    if (status.state!=6) return UdsResult::State;
    for(std::size_t i=1;i<nodes.size();++i) if(nodes[i].id) {
        outgoing.push_back(MakeFrame(FrameType::Deauthentication,nodes[i].mac,{3,0}));
        nodes[i]={};
    }
    for(auto& ep:endpoints) { ep.packets.clear(); ep.used=0; }
    authenticating={}; allowed=false;
    const auto previous=status.mask; status={3,reason,0,0,0,0,0}; Changed(previous);
    return UdsResult::Ok;
}
UdsResult UdsHost::Allow(bool allow) {
    if (!initialized) return UdsResult::NotInitialized;
    if (status.state!=6) return UdsResult::State;
    allowed=allow; return UdsResult::Ok;
}
UdsResult UdsHost::Eject(std::uint16_t node) {
    if(!initialized) return UdsResult::NotInitialized;
    if(status.state!=6) return UdsResult::State;
    if(node==1 || (!IsNode(node) && node!=0xFFFF)) return UdsResult::Argument;
    for(std::size_t i=1;i<nodes.size();++i) if(nodes[i].id && (node==0xFFFF || nodes[i].id==node)) {
        outgoing.push_back(MakeFrame(FrameType::Deauthentication,nodes[i].mac,{3,0}));
        Remove(i);
    }
    return UdsResult::Ok;
}
UdsResult UdsHost::SetApplicationData(std::span<const std::uint8_t> data) {
    if(!initialized) return UdsResult::NotInitialized;
    if(status.state!=6) return UdsResult::State;
    if(data.size()>200) return UdsResult::TooLarge;
    app_data.assign(data.begin(),data.end()); return UdsResult::Ok;
}
int UdsHost::EndpointIndex(std::uint32_t descriptor) const {
    for(std::size_t i=0;i<endpoints.size();++i)
        if(descriptor && endpoints[i].descriptor==descriptor) return static_cast<int>(i);
    return -1;
}
bool UdsHost::HasPacket(std::size_t index) const { return index<endpoints.size() && !endpoints[index].packets.empty(); }
UdsResult UdsHost::CreateEndpoint(std::uint32_t& descriptor) {
    if(!initialized) return UdsResult::NotInitialized;
    for(auto& ep:endpoints) if(!ep.descriptor) {
        if (++next_descriptor==0) ++next_descriptor;
        ep.descriptor=next_descriptor; descriptor=ep.descriptor; return UdsResult::Ok;
    }
    return UdsResult::Resource;
}
UdsResult UdsHost::Attach(std::uint32_t descriptor, std::uint16_t peer, std::uint8_t data_channel,
                         std::size_t capacity) {
    if(!initialized) return UdsResult::NotInitialized;
    const auto index=EndpointIndex(descriptor);
    if(index<0 || !data_channel || (!IsNode(peer) && peer!=0xFFFF) || capacity<1536)
        return UdsResult::Argument;
    std::size_t used=0;
    for(const auto& ep:endpoints) if(ep.descriptor!=descriptor) used+=ep.capacity;
    if(capacity>memory_size || used>memory_size-capacity) return UdsResult::Resource;
    auto& ep=endpoints[index]; ep.peer=peer; ep.channel=data_channel;
    ep.capacity=capacity; ep.used=0; ep.packets.clear(); return UdsResult::Ok;
}
UdsResult UdsHost::DestroyEndpoint(std::uint32_t descriptor) {
    if(!initialized) return UdsResult::NotInitialized;
    const auto index=EndpointIndex(descriptor); if(index<0) return UdsResult::Argument;
    endpoints[index]={}; return UdsResult::Ok;
}
UdsResult UdsHost::Receive(std::uint32_t descriptor, std::size_t capacity, UdsDatagram& datagram) {
    if(!initialized) return UdsResult::NotInitialized;
    if(status.state!=6) return UdsResult::State;
    const auto index=EndpointIndex(descriptor);
    if(index<0 || !endpoints[index].channel) return UdsResult::Argument;
    auto& ep=endpoints[index]; if(ep.packets.empty()) return UdsResult::NotFound;
    if(ep.packets.front().data.size()>capacity) return UdsResult::TooLarge;
    datagram=std::move(ep.packets.front()); ep.packets.pop_front();
    ep.used-=datagram.data.size()+32; return UdsResult::Ok;
}
const UdsNode* UdsHost::Node(std::uint16_t node) const {
    return IsNode(node) && nodes[node-1].id==node ? &nodes[node-1] : nullptr;
}
Frame UdsHost::MakeFrame(FrameType type,const Mac& destination,Bytes data) const {
    return Frame{type,channel,nodes[0].mac,destination,std::move(data)};
}
UdsResult UdsHost::Send(std::uint32_t descriptor,std::uint16_t peer,std::uint8_t data_channel,
                       std::span<const std::uint8_t> data) {
    if(!initialized) return UdsResult::NotInitialized;
    if(status.state!=6) return UdsResult::State;
    if(EndpointIndex(descriptor)<0 || !data_channel) return UdsResult::Argument;
    if(data.size()>1478) return UdsResult::TooLarge;
    if(peer==1 || (peer!=0xFFFF && !Node(peer))) return UdsResult::NotFound;
    if(outgoing.size()>=128) return UdsResult::Busy;
    Bytes body(22+data.size()); LLC(body,0x876D);
    Put16(body,8,static_cast<std::uint16_t>(14+data.size()));
    Put16(body,12,static_cast<std::uint16_t>(10+data.size())); body[15]=data_channel;
    Put16(body,16,sequence++); Put16(body,18,peer); Put16(body,20,1);
    std::copy(data.begin(),data.end(),body.begin()+22);
    outgoing.push_back(MakeFrame(FrameType::Data,peer==0xFFFF?BroadcastMac:Node(peer)->mac,std::move(body)));
    ++transmitted; return UdsResult::Ok;
}
void UdsHost::QueueLogoff(const UdsNode& node) {
    Bytes body(8+0x298); LLC(body,0x888E); Put16(body,8,0x202); Put16(body,12,node.id);
    std::copy(node.mac.begin(),node.mac.end(),body.begin()+14);
    body[26]=status.count; body[27]=status.maximum;
    for(std::size_t i=0;i<nodes.size();++i) {
        const auto base=32+i*40; Put64(body,base,nodes[i].friend_code);
        for(std::size_t j=0;j<nodes[i].name.size();++j) Put16(body,base+8+j*2,nodes[i].name[j]);
        Put16(body,base+32,nodes[i].id);
    }
    outgoing.push_back(MakeFrame(FrameType::Data,node.mac,std::move(body)));
}
void UdsHost::Remove(std::size_t index) {
    if(index==0 || !nodes[index].id) return;
    for(auto& ep:endpoints) {
        for(auto packet=ep.packets.begin(); packet!=ep.packets.end();) {
            if(packet->source==nodes[index].id) {
                ep.used-=packet->data.size()+32;
                packet=ep.packets.erase(packet);
            } else ++packet;
        }
    }
    status.mask&=static_cast<std::uint16_t>(~(1U<<index)); --status.count;
    nodes[index]={}; authenticating[index]={}; Changed(static_cast<std::uint16_t>(1U<<index));
}
void UdsHost::PeerLost() {
    for(std::size_t i=1;i<nodes.size();++i) Remove(i);
    authenticating={}; outgoing.clear();
    for(auto& ep:endpoints) {ep.packets.clear(); ep.used=0;}
}
void UdsHost::Process(const Frame& frame) {
    using NativeFrameDetail::BE16;
    using NativeFrameDetail::LE16;
    if(!initialized || status.state!=6 || frame.channel!=channel ||
        (frame.destination!=nodes[0].mac && frame.destination!=BroadcastMac) ||
        frame.source==nodes[0].mac || frame.source==Mac{} || (frame.source[0]&1) ||
        ValidateNativeFrame(frame)!=NativeFrameError::None || outgoing.size()>120) { ++rejected; return; }
    const auto& b=frame.data;
    if(frame.type==FrameType::Authentication) {
        if(LE16(b,2)!=1 || !allowed) {++rejected; return;}
        std::size_t slot=nodes.size();
        for(std::size_t i=1;i<status.maximum;++i) {
            if(nodes[i].mac==frame.source && nodes[i].id) {++rejected; return;}
            if(authenticating[i]==frame.source) {slot=i; break;}
            if(slot==nodes.size() && !nodes[i].id && authenticating[i]==Mac{}) slot=i;
        }
        if(slot==nodes.size()) {++rejected; return;}
        authenticating[slot]=frame.source;
        outgoing.push_back(MakeFrame(FrameType::Authentication,frame.source,{0,0,2,0,0,0}));
        Bytes assoc{0x31,4,0,0,static_cast<std::uint8_t>(slot),0xC0};
        char name[9]{}; std::snprintf(name,sizeof(name),"%08X",network_id);
        Tag(assoc,0,Bytes(name,name+8));
        outgoing.push_back(MakeFrame(FrameType::AssociationResponse,frame.source,std::move(assoc)));
        return;
    }
    std::size_t slot=nodes.size();
    for(std::size_t i=1;i<status.maximum;++i)
        if(nodes[i].mac==frame.source || authenticating[i]==frame.source) {slot=i; break;}
    if(slot==nodes.size()) {++rejected; return;}
    if(frame.type==FrameType::Deauthentication) {Remove(slot); authenticating[slot]={}; return;}
    if(frame.type!=FrameType::Data) {++rejected; return;}
    if(BE16(b,6)==0x888E) {
        if(BE16(b,8)!=0x201 || b[12]!=1 || BE16(b,10)!=slot || !allowed) {++rejected; return;}
        if(!nodes[slot].id) {
            auto& node=nodes[slot]; node.mac=frame.source; node.id=static_cast<std::uint16_t>(slot+1);
            node.friend_code=Get64(b,16);
            for(std::size_t j=0;j<node.name.size();++j) node.name[j]=BE16(b,24+j*2);
            status.mask|=static_cast<std::uint16_t>(1U<<slot); ++status.count;
            Changed(static_cast<std::uint16_t>(1U<<slot));
        }
        QueueLogoff(nodes[slot]); return;
    }
    if(!nodes[slot].id || BE16(b,20)!=nodes[slot].id || (BE16(b,18)!=1 && BE16(b,18)!=0xFFFF)) {++rejected; return;}
    for(auto& ep:endpoints) if(ep.descriptor && ep.channel==b[15] &&
        (ep.peer==0xFFFF || ep.peer==nodes[slot].id)) {
        const auto cost=b.size()-22+32;
        if(ep.used+cost>ep.capacity || ep.packets.size()>=128) {++rejected; continue;}
        ep.packets.push_back({nodes[slot].id,Bytes(b.begin()+22,b.end())}); ep.used+=cost;
    }
    ++received;
}
std::optional<Frame> UdsHost::PopOutgoing() {
    if(outgoing.empty()) return std::nullopt;
    auto frame=std::move(outgoing.front()); outgoing.pop_front(); return frame;
}
std::optional<Frame> UdsHost::Beacon(std::uint64_t now) {
    if(!initialized || status.state!=6 || (last_beacon && now-last_beacon<102400)) return std::nullopt;
    last_beacon=now ? now : 1;
    Bytes frame(12); frame[8]=100; frame[10]=0x31; frame[11]=4;
    Tag(frame,0,Bytes(8)); Tag(frame,221,{0,0x1F,0x32,20,10,0,0});
    Bytes info(52+app_data.size()); info[1]=0x1F; info[2]=0x32; info[3]=21;
    Put32(info,4,communication_id); info[8]=id; Put32(info,12,network_id);
    info[16]=status.count; info[17]=status.maximum; info[51]=static_cast<std::uint8_t>(app_data.size());
    std::copy(app_data.begin(),app_data.end(),info.begin()+52);
    const auto sha=crypto.sha1(info); if(sha.size()!=20) throw std::runtime_error("UDS SHA1 failed");
    std::copy(sha.begin(),sha.end(),info.begin()+31); Tag(frame,221,info);
    Bytes plain(18+status.maximum*30); Put16(plain,16,status.mask);
    for(std::size_t i=0;i<status.maximum;++i) {
        const auto base=18+i*30; Put64(plain,base,nodes[i].friend_code);
        for(std::size_t j=0;j<nodes[i].name.size();++j) Put16(plain,base+8+j*2,nodes[i].name[j]);
        Put16(plain,base+28,nodes[i].id);
    }
    const auto hash=crypto.md5(std::span(plain).subspan(16));
    if(hash.size()!=16) throw std::runtime_error("UDS MD5 failed");
    std::copy(hash.begin(),hash.end(),plain.begin());
    Bytes counter(16); std::copy(nodes[0].mac.begin(),nodes[0].mac.end(),counter.begin());
    Put32(counter,6,communication_id,true); counter[10]=id; Put32(counter,12,network_id,true);
    std::array<std::uint8_t,16> ctr{}; std::copy(counter.begin(),counter.end(),ctr.begin());
    const auto encrypted=crypto.ctr(plain,ctr);
    if(encrypted.size()!=plain.size()) throw std::runtime_error("UDS beacon cipher failed");
    const auto cut=std::min<std::size_t>(250,encrypted.size());
    Bytes first{0,0x1F,0x32,24}; first.insert(first.end(),encrypted.begin(),encrypted.begin()+cut); Tag(frame,221,first);
    if(cut<encrypted.size()) {
        Bytes second{0,0x1F,0x32,25}; second.insert(second.end(),encrypted.begin()+cut,encrypted.end()); Tag(frame,221,second);
    }
    return MakeFrame(FrameType::Beacon,BroadcastMac,std::move(frame));
}
} // namespace WiiU3DSLink
