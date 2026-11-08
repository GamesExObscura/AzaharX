// SPDX-License-Identifier: GPL-2.0-or-later
#include "Transport.h"
#include "LanDatagram.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <boost/asio.hpp>

namespace WiiU3DSLink {
namespace {
constexpr std::array<std::uint8_t, 8> Magic{'C','W','3','D','S','L','N','K'};
constexpr std::size_t QueueLimit = 128;
using Clock = std::chrono::steady_clock;
using UDP = boost::asio::ip::udp;

void Put64(std::vector<std::uint8_t>& out, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i)
        out[offset + i] = static_cast<std::uint8_t>(value >> (56 - i * 8));
}
std::uint64_t Get64(std::span<const std::uint8_t> in, std::size_t offset) {
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < 8; ++i) result = (result << 8) | in[offset + i];
    return result;
}
bool Valid(const Message& m) {
    if (m.role != Role::WiiU && m.role != Role::Nintendo3DS) return false;
    if (m.session == 0 || m.frame.data.size() > MaxPayload) return false;
    if (m.type == MessageType::Frame)
        return m.peer_session != 0 && static_cast<unsigned>(m.frame.type) <= 4 &&
               m.frame.channel >= 1 && m.frame.channel <= 14;
    if (m.type != MessageType::Hello && m.type != MessageType::HelloAck) return false;
    return m.frame.data.empty() && m.frame.channel == 0 && m.frame.type == FrameType::Beacon &&
           m.frame.source == std::array<std::uint8_t, 6>{} &&
           m.frame.destination == std::array<std::uint8_t, 6>{} &&
           (m.type == MessageType::Hello ? m.peer_session == 0 : m.peer_session != 0);
}
}

std::vector<std::uint8_t> Encode(const Message& m) {
    if (!Valid(m)) return {};
    std::vector<std::uint8_t> out(HeaderSize + m.frame.data.size());
    std::copy(Magic.begin(), Magic.end(), out.begin());
    out[8] = 1;
    out[9] = static_cast<std::uint8_t>(m.type);
    out[10] = static_cast<std::uint8_t>(m.role);
    out[11] = static_cast<std::uint8_t>(m.frame.type);
    out[12] = m.frame.channel;
    out[14] = static_cast<std::uint8_t>(m.frame.data.size() >> 8);
    out[15] = static_cast<std::uint8_t>(m.frame.data.size());
    Put64(out, 16, m.session);
    Put64(out, 24, m.peer_session);
    std::copy(m.frame.source.begin(), m.frame.source.end(), out.begin() + 32);
    std::copy(m.frame.destination.begin(), m.frame.destination.end(), out.begin() + 38);
    std::copy(m.frame.data.begin(), m.frame.data.end(), out.begin() + HeaderSize);
    return out;
}

std::optional<Message> Decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < HeaderSize || bytes.size() > HeaderSize + MaxPayload ||
        !std::equal(Magic.begin(), Magic.end(), bytes.begin()) || bytes[8] != 1 || bytes[13] != 0)
        return std::nullopt;
    const auto length = (std::size_t{bytes[14]} << 8) | bytes[15];
    if (length != bytes.size() - HeaderSize) return std::nullopt;
    Message m;
    m.type = static_cast<MessageType>(bytes[9]);
    m.role = static_cast<Role>(bytes[10]);
    m.frame.type = static_cast<FrameType>(bytes[11]);
    m.frame.channel = bytes[12];
    m.session = Get64(bytes, 16);
    m.peer_session = Get64(bytes, 24);
    std::copy_n(bytes.begin() + 32, 6, m.frame.source.begin());
    std::copy_n(bytes.begin() + 38, 6, m.frame.destination.begin());
    m.frame.data.assign(bytes.begin() + HeaderSize, bytes.end());
    return Valid(m) ? std::optional{std::move(m)} : std::nullopt;
}

struct Transport::Impl {
    struct Run {
        boost::asio::io_context io;
        UDP::socket socket{io};
        boost::asio::steady_timer timer{io};
        UDP::endpoint peer, sender;
        std::unique_ptr<LanDatagram> lan;
        bool lan_host{}, has_peer{true};
        Role role;
        std::uint64_t session{}, peer_session{}, pending_peer_session{};
        Clock::time_point last_seen{};
        std::array<std::uint8_t, HeaderSize + MaxPayload + 1> buffer{};
        mutable std::mutex mutex;
        LinkStatus status;
        std::deque<Frame> incoming;
        std::atomic<std::size_t> pending{};
        std::thread thread;

        Run(Role role_, std::uint16_t local_port, std::uint16_t peer_port,
            std::optional<LanOptions> options) : role(role_) {
            socket.open(UDP::v4());
            // No reuse_address: a second emulator must not steal a running link.
            if(options) {
                lan=std::make_unique<LanDatagram>(options->key,options->passphrase);
                options->key.fill(std::uint8_t{0});
                lan_host=role==Role::WiiU;has_peer=!lan_host;
                const auto address=boost::asio::ip::make_address_v4(options->address);
                status.network=std::string(options->passphrase?"Direct passphrase ":"Encrypted LAN ")+
                    (lan_host?"host ":"join ")+options->address+":"+std::to_string(options->port);
                if(lan_host) socket.bind(UDP::endpoint(address,options->port));
                else {
                    peer=UDP::endpoint(address,options->port);
                    socket.bind(UDP::endpoint(UDP::v4(),0));
                }
            } else {
                peer = UDP::endpoint(boost::asio::ip::address_v4::loopback(), peer_port);
                socket.bind(UDP::endpoint(boost::asio::ip::address_v4::loopback(), local_port));
            }
            std::random_device random;
            session = (std::uint64_t{random()} << 32) ^ random();
            if (session == 0) session = 1;
            status.enabled = true;
            Read();
            Tick();
            thread = std::thread([this] {
                try { io.run(); }
                catch (const std::exception& e) {
                    std::scoped_lock lock(mutex);
                    status.enabled = status.peer_present = false;
                    status.error = e.what();
                }
            });
        }
        ~Run() {
            io.stop();
            if (thread.joinable()) thread.join();
            boost::system::error_code ignored;
            socket.close(ignored);
        }
        void Write(Message m, bool counted = false) {
            auto bytes = std::make_shared<std::vector<std::uint8_t>>(Encode(m));
            if (bytes->empty()) { if (counted) --pending; return; }
            if(lan) {
                if(!has_peer){if(counted)--pending;return;}
                struct Batch {std::size_t remaining{};bool failed{};};
                const auto packets=lan->Seal(*bytes);
                auto batch=std::make_shared<Batch>(Batch{packets.size(),false});
                for(const auto& packet:packets) {
                    auto payload=std::make_shared<std::vector<std::uint8_t>>(packet);
                    socket.async_send_to(boost::asio::buffer(*payload),peer,
                        [this,payload,batch,counted](const boost::system::error_code& error,std::size_t count){
                            batch->failed|=bool(error)||count!=payload->size();
                            if(--batch->remaining)return;
                            if(counted)--pending;
                            std::scoped_lock lock(mutex);
                            if(batch->failed)++status.dropped;else ++status.transmitted;
                        });
                }
                return;
            }
            socket.async_send_to(boost::asio::buffer(*bytes), peer,
                [this, bytes, counted](const boost::system::error_code& e, std::size_t count) {
                    if (counted) --pending;
                    std::scoped_lock lock(mutex);
                    if (!e && count == bytes->size()) ++status.transmitted;
                    else if (e != boost::asio::error::operation_aborted) ++status.dropped;
                });
        }
        void Tick() {
            {
                std::scoped_lock lock(mutex);
                if (status.peer_present && Clock::now() - last_seen > std::chrono::seconds(2)) {
                    status.peer_present = false;
                    peer_session = 0;
                    pending_peer_session = 0;
                    incoming.clear();
                }
            }
            if(has_peer) Write(Message{MessageType::Hello, role, session, 0, {}});
            timer.expires_after(std::chrono::milliseconds(250));
            timer.async_wait([this](const boost::system::error_code& e) { if (!e) Tick(); });
        }
        void Read() {
            socket.async_receive_from(boost::asio::buffer(buffer), sender,
                [this](const boost::system::error_code& e, std::size_t size) {
                    if (!e) Process(std::span{buffer.data(), size});
                    else if (e == boost::asio::error::message_size) {
                        std::scoped_lock lock(mutex);
                        ++status.rejected;
                    }
                    if (e != boost::asio::error::operation_aborted) Read();
                });
        }
        void Process(std::span<const std::uint8_t> bytes) {
            std::vector<std::uint8_t> decrypted;
            if(lan) {
                if(lan_host){std::scoped_lock lock(mutex);if(status.peer_present&&sender!=peer){++status.rejected;return;}}
                if(!lan_host && sender!=peer){std::scoped_lock lock(mutex);++status.rejected;return;}
                auto result=lan->Open(bytes);
                if(result.state==LanRead::Rejected){std::scoped_lock lock(mutex);++status.rejected;return;}
                if(result.state==LanRead::Pending)return;
                decrypted=std::move(result.bytes);bytes=decrypted;
            }
            const auto m = Decode(bytes);
            std::scoped_lock lock(mutex);
            if(lan_host && (!has_peer || (!status.peer_present && sender!=peer))) {
                if(!m || m->type!=MessageType::Hello || m->role!=Role::Nintendo3DS){++status.rejected;return;}
                peer=sender;has_peer=true;
            }
            if (sender != peer || !m || m->role == role) { ++status.rejected; return; }
            if (m->type == MessageType::Hello) {
                pending_peer_session = m->session;
                Write(Message{MessageType::HelloAck, role, session, m->session, {}});
                return;
            }
            if (m->peer_session != session) { ++status.rejected; return; }
            if (m->type == MessageType::HelloAck) {
                if (peer_session != m->session) incoming.clear();
                peer_session = m->session;
                last_seen = Clock::now();
                status.peer_present = true;
                return;
            }
            // A peer can receive our ACK before our own HELLO has been ACKed.
            // Its echoed nonce proves it received our session. This also covers
            // a fast peer restart before the old session's expiry deadline.
            if (m->session == pending_peer_session &&
                (!status.peer_present || peer_session != m->session)) {
                incoming.clear();
                peer_session = m->session;
                status.peer_present = true;
            }
            if (!status.peer_present || m->session != peer_session) { ++status.rejected; return; }
            last_seen = Clock::now();
            if (incoming.size() == QueueLimit) { ++status.dropped; return; }
            incoming.push_back(m->frame);
            ++status.received;
        }
    };
    mutable std::mutex mutex;
    std::unique_ptr<Run> run;
    std::string error;
};

Transport::Transport() : impl(std::make_unique<Impl>()) {}
Transport::~Transport() = default;
bool Transport::Start(Role role, std::uint16_t local_port, std::uint16_t peer_port) {
    std::scoped_lock lock(impl->mutex);
    impl->run.reset();
    impl->error.clear();
    if ((role != Role::WiiU && role != Role::Nintendo3DS) || local_port == 0 ||
        peer_port == 0 || local_port == peer_port) {
        impl->error = "Invalid link role or ports";
        return false;
    }
    try { impl->run = std::make_unique<Impl::Run>(role, local_port, peer_port,LanOptionsFromEnvironment()); }
    catch (const std::exception& e) { impl->error = e.what(); return false; }
    return true;
}
bool Transport::StartDirect(Role role,const std::string& address,std::uint16_t port,std::string passphrase){
    std::optional<LanOptions> options;
    try{
        if(role!=Role::WiiU&&role!=Role::Nintendo3DS)throw std::invalid_argument("Invalid emulator role.");
        options=DirectOptions(role==Role::WiiU,address,port,std::move(passphrase));
    }catch(const std::exception& e){std::scoped_lock lock(impl->mutex);impl->error=e.what();return false;}
    std::scoped_lock lock(impl->mutex);
    impl->run.reset();impl->error.clear();
    try{impl->run=std::make_unique<Impl::Run>(role,std::uint16_t{44660},std::uint16_t{44661},options);}
    catch(const std::exception& e){options->key.fill(0);impl->error=e.what();return false;}
    options->key.fill(0);return true;
}
void Transport::Stop() {
    std::scoped_lock lock(impl->mutex);
    impl->run.reset();
    impl->error.clear();
}
bool Transport::Send(const Frame& frame) {
    std::scoped_lock lock(impl->mutex);
    if (!impl->run || frame.data.size() > MaxPayload ||
        static_cast<unsigned>(frame.type) > 4 || frame.channel < 1 || frame.channel > 14) return false;
    auto& run = *impl->run;
    std::uint64_t peer_session;
    {
        std::scoped_lock state_lock(run.mutex);
        if (!run.status.peer_present || !run.status.enabled) return false;
        peer_session = run.peer_session;
    }
    if (run.pending.fetch_add(1) >= QueueLimit) { --run.pending; return false; }
    boost::asio::post(run.io, [&run, frame, peer_session] {
        if (run.peer_session == peer_session)
            run.Write(Message{MessageType::Frame, run.role, run.session, peer_session, frame}, true);
        else --run.pending;
    });
    return true;
}
std::optional<Frame> Transport::Receive() {
    std::scoped_lock lock(impl->mutex);
    if (!impl->run) return std::nullopt;
    std::scoped_lock state_lock(impl->run->mutex);
    if (impl->run->incoming.empty()) return std::nullopt;
    auto result = std::move(impl->run->incoming.front());
    impl->run->incoming.pop_front();
    return result;
}
LinkStatus Transport::GetStatus() const {
    std::scoped_lock lock(impl->mutex);
    if (!impl->run) { LinkStatus result; result.error = impl->error; return result; }
    std::scoped_lock state_lock(impl->run->mutex);
    auto status = impl->run->status;
    if (!impl->error.empty()) status.error = impl->error;
    return status;
}
void Transport::SetGuestStatus(std::string value) {
    std::scoped_lock lock(impl->mutex);
    if (!impl->run) return;
    std::scoped_lock state_lock(impl->run->mutex);
    impl->run->status.guest_status=std::move(value);
}
Transport& GetTransport() { static Transport transport; return transport; }
std::string DescribeStatus(const LinkStatus& s) {
    if (!s.error.empty()) return "Link transport error: " + s.error;
    if (!s.enabled) return "Disabled. No link socket or discovery traffic.";
    const auto guest = s.guest_status.empty() ? "No native UDS session initialized." : s.guest_status;
    if(!s.network.empty())
        return std::string(s.peer_present?"Authenticated peer connected.\n":"Waiting for an authenticated peer.\n")+
            s.network+"\n"+guest+"\nReceived: "+std::to_string(s.received)+"; sent: "+std::to_string(s.transmitted)+
            "; rejected: "+std::to_string(s.rejected)+"; dropped: "+std::to_string(s.dropped)+
            "\nNo room server or Pretendo settings changed. Internet hosting requires a reachable UDP port.";
    return std::string(s.peer_present ? "Same-PC emulator peer found.\n" : "Waiting for the other emulator on this PC.\n") +
           guest + "\nNative emulator-to-emulator link. External LAN and real-console connections are not supported.";
}
} // namespace WiiU3DSLink
