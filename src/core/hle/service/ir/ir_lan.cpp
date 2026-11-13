// Copyright 2026 Azalon3-DS Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <array>
#include <random>
#include <enet/enet.h>
#include <fmt/ranges.h>
#include "common/logging/log.h"
#include "core/hle/service/ir/ir_lan.h"

namespace Service::IR {

namespace {

// Wire format (all multi-byte values little endian):
//   u32 magic 'AZIR', u8 version, u8 type, u16 reserved, u32 sender id, body...
constexpr std::array<u8, 4> kMagic{'A', 'Z', 'I', 'R'};
constexpr u8 kVersion = 1;
constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kMaxPayload = 0x4000; // ir:USER packets are limited to 14 bits of size

constexpr auto kBeaconInterval = std::chrono::milliseconds(200);
constexpr auto kConnectRetry = std::chrono::milliseconds(300);
constexpr auto kKeepAliveInterval = std::chrono::milliseconds(500);
// Generous: one instance may stall (loading, pausing, a debugger) while the other keeps running.
constexpr auto kPeerTimeout = std::chrono::seconds(5);

// Diagnostics: the first packets of each link are logged in full, then a summary every few s.
constexpr u32 kLoggedPackets = 24;
constexpr auto kStatsInterval = std::chrono::seconds(3);

ENetSocket ToEnet(std::intptr_t s) {
    return static_cast<ENetSocket>(s);
}

bool IsValid(std::intptr_t s) {
    return ToEnet(s) != ENET_SOCKET_NULL;
}

void PutU32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
    p[2] = static_cast<u8>(v >> 16);
    p[3] = static_cast<u8>(v >> 24);
}

u32 GetU32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

u64 HashBytes(std::span<const u8> data) {
    u64 h = 0xcbf29ce484222325ULL; // FNV-1a
    for (const u8 b : data) {
        h = (h ^ b) * 0x100000001b3ULL;
    }
    return h;
}

bool ModesFit(IrLanLink::Mode a, IrLanLink::Mode b) {
    using Mode = IrLanLink::Mode;
    return !((a == Mode::Wait && b == Mode::Wait) || (a == Mode::Require && b == Mode::Require));
}

} // namespace

IrLanLink::IrLanLink(Callbacks callbacks_) : callbacks(std::move(callbacks_)) {
    sockets[0] = static_cast<std::intptr_t>(ENET_SOCKET_NULL);
    sockets[1] = static_cast<std::intptr_t>(ENET_SOCKET_NULL);
    scratch.resize(kHeaderSize + kMaxPayload + 16);
}

IrLanLink::~IrLanLink() {
    Stop(true);
}

bool IrLanLink::OpenSockets() {
    CloseSockets();

    // Discovery socket: every instance binds the same port so a broadcast reaches all of them,
    // including other instances on this machine.
    ENetSocket discovery = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (discovery == ENET_SOCKET_NULL) {
        LOG_ERROR(Service_IR, "IR LAN: could not create the discovery socket");
        return false;
    }
    enet_socket_set_option(discovery, ENET_SOCKOPT_REUSEADDR, 1);
    enet_socket_set_option(discovery, ENET_SOCKOPT_BROADCAST, 1);
    enet_socket_set_option(discovery, ENET_SOCKOPT_NONBLOCK, 1);
    ENetAddress any{};
    any.host = ENET_HOST_ANY;
    any.port = kDiscoveryPort;
    if (enet_socket_bind(discovery, &any) != 0) {
        LOG_ERROR(Service_IR, "IR LAN: could not bind UDP port {}", kDiscoveryPort);
        enet_socket_destroy(discovery);
        return false;
    }
    sockets[0] = static_cast<std::intptr_t>(discovery);

    // Data socket: own ephemeral port. Beacons are sent from it, so their source address is
    // where the peer has to send everything else.
    ENetSocket data = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (data == ENET_SOCKET_NULL) {
        LOG_ERROR(Service_IR, "IR LAN: could not create the data socket");
        CloseSockets();
        return false;
    }
    enet_socket_set_option(data, ENET_SOCKOPT_BROADCAST, 1);
    enet_socket_set_option(data, ENET_SOCKOPT_NONBLOCK, 1);
    any.port = 0;
    if (enet_socket_bind(data, &any) != 0) {
        LOG_ERROR(Service_IR, "IR LAN: could not bind the data socket");
        enet_socket_destroy(data);
        CloseSockets();
        return false;
    }
    sockets[1] = static_cast<std::intptr_t>(data);
    return true;
}

void IrLanLink::CloseSockets() {
    for (auto& s : sockets) {
        if (IsValid(s)) {
            enet_socket_destroy(ToEnet(s));
        }
        s = static_cast<std::intptr_t>(ENET_SOCKET_NULL);
    }
}

bool IrLanLink::Start(Mode new_mode) {
    if (state == State::Connected) {
        // Already linked; a game asking again keeps the existing link.
        return true;
    }
    if (state == State::Searching) {
        // Games repeat AutoConnection every frame while waiting. Keep the handshake in flight
        // (a reset here would drop the Accept from the peer we are connecting to).
        if (new_mode != mode) {
            mode = new_mode;
            has_pending = false;
        }
        return true;
    }
    mode = new_mode;
    if (!OpenSockets()) {
        return false;
    }
    std::random_device rd;
    std::mt19937 rng(rd());
    do {
        own_id = static_cast<u32>(rng());
    } while (own_id == 0);
    pending_network_id = static_cast<u8>(rng() % 0xFE + 1);
    state = State::Searching;
    has_pending = false;
    LOG_INFO(Service_IR, "IR LAN: searching (mode {}, id {:08x}, port {})", static_cast<int>(mode),
             own_id, kDiscoveryPort);
    SendBeacon();
    return true;
}

void IrLanLink::Stop(bool notify_peer) {
    if (state == State::Idle) {
        return;
    }
    if (notify_peer && state == State::Connected) {
        SendTo(peer, MsgType::Bye, {});
    }
    LOG_INFO(Service_IR, "IR LAN: stopped");
    CloseSockets();
    state = State::Idle;
    has_pending = false;
}

bool IrLanLink::Send(std::span<const u8> payload) {
    if (state != State::Connected || payload.size() > kMaxPayload) {
        return false;
    }
    ++sent_packets;
    if (sent_packets <= kLoggedPackets) {
        LOG_INFO(Service_IR, "IR LAN: send #{} ({} bytes): {:02x}", sent_packets, payload.size(),
                 fmt::join(payload.first(std::min<std::size_t>(payload.size(), 48)), " "));
    }
    SendTo(peer, MsgType::Data, payload);
    return true;
}

void IrLanLink::SendTo(const Peer& to, MsgType type, std::span<const u8> body) {
    if (!IsValid(sockets[1]) || body.size() > kMaxPayload) {
        return;
    }
    u8* p = scratch.data();
    std::copy(kMagic.begin(), kMagic.end(), p);
    p[4] = kVersion;
    p[5] = static_cast<u8>(type);
    p[6] = 0;
    p[7] = 0;
    PutU32(p + 8, own_id);
    std::copy(body.begin(), body.end(), p + kHeaderSize);

    ENetAddress address{};
    address.host = to.host;
    address.port = to.port;
    ENetBuffer buffer{};
    buffer.data = p;
    buffer.dataLength = kHeaderSize + body.size();
    enet_socket_send(ToEnet(sockets[1]), &address, &buffer, 1);
    if (state == State::Connected) {
        last_sent = Clock::now();
    }
}

void IrLanLink::SendBeacon() {
    Peer broadcast{};
    broadcast.host = ENET_HOST_BROADCAST;
    broadcast.port = kDiscoveryPort;
    const std::array<u8, 1> body{static_cast<u8>(mode)};
    SendTo(broadcast, MsgType::Beacon, body);
    last_beacon = Clock::now();
}

bool IrLanLink::WeInitiate(Mode their_mode, u32 their_id) const {
    if (mode == Mode::Require || their_mode == Mode::Wait) {
        return true;
    }
    if (mode == Mode::Wait || their_mode == Mode::Require) {
        return false;
    }
    // Both auto: the lower id plays the requiring side.
    return own_id < their_id;
}

void IrLanLink::Poll() {
    if (state == State::Idle) {
        return;
    }

    DrainSocket(0);
    if (state != State::Idle) {
        DrainSocket(1);
    }

    const auto now = Clock::now();
    if (state == State::Searching) {
        if (now - last_beacon >= kBeaconInterval) {
            SendBeacon();
        }
        if (has_pending && now - last_connect >= kConnectRetry) {
            const std::array<u8, 1> body{pending_network_id};
            SendTo(pending, MsgType::Connect, body);
            last_connect = now;
        }
    } else if (state == State::Connected) {
        if (now - last_stats >= kStatsInterval) {
            LOG_INFO(Service_IR,
                     "IR LAN: link up, {} packets sent, {} received ({} exact repeats, {} "
                     "repeats ignoring the first 12 bytes)",
                     sent_packets, received_packets, repeated_payloads, repeated_bodies);
            last_stats = now;
        }
        if (now - last_heard >= kPeerTimeout) {
            LoseConnection("peer timed out");
        } else if (now - last_sent >= kKeepAliveInterval) {
            SendTo(peer, MsgType::KeepAlive, {});
        }
    }
}

void IrLanLink::DrainSocket(std::size_t socket_index) {
    // Bounded so a flood can't stall the emulation thread.
    for (int i = 0; i < 64 && state != State::Idle && IsValid(sockets[socket_index]); ++i) {
        ENetAddress from{};
        ENetBuffer buffer{};
        buffer.data = scratch.data();
        buffer.dataLength = scratch.size();
        const int received = enet_socket_receive(ToEnet(sockets[socket_index]), &from, &buffer, 1);
        if (received <= 0) {
            break;
        }
        HandleDatagram(std::span<const u8>(scratch.data(), static_cast<std::size_t>(received)),
                       from.host, from.port);
    }
}

void IrLanLink::HandleDatagram(std::span<const u8> datagram, u32 from_host, u16 from_port) {
    if (datagram.size() < kHeaderSize ||
        !std::equal(kMagic.begin(), kMagic.end(), datagram.begin()) || datagram[4] != kVersion) {
        return;
    }
    const auto type = static_cast<MsgType>(datagram[5]);
    const u32 sender = GetU32(datagram.data() + 8);
    if (sender == own_id) {
        return; // our own broadcast looped back
    }
    const auto body = datagram.subspan(kHeaderSize);
    const Peer from{sender, from_host, from_port};

    switch (type) {
    case MsgType::Beacon:
        if (state == State::Searching && body.size() >= 1) {
            const u8 their_mode = body[0];
            if (their_mode >= 1 && their_mode <= 3) {
                HandleBeacon(from, static_cast<Mode>(their_mode));
            }
        }
        break;

    case MsgType::Connect:
        if (body.size() < 1) {
            break;
        }
        if (state == State::Searching) {
            // Only accept if we would not be the one initiating with this peer, so two
            // instances never both believe they are the requiring side.
            if (has_pending && pending.id != sender) {
                break;
            }
            BecomeConnected(from, RoleWait, body[0]);
            const std::array<u8, 1> reply{body[0]};
            SendTo(peer, MsgType::Accept, reply);
        } else if (state == State::Connected && sender == peer.id) {
            // Our Accept got lost; repeat it.
            const std::array<u8, 1> reply{network_id};
            SendTo(peer, MsgType::Accept, reply);
        }
        break;

    case MsgType::Accept:
        if (state == State::Searching && has_pending && sender == pending.id) {
            BecomeConnected(from, RoleRequire, pending_network_id);
        }
        break;

    case MsgType::Data:
    case MsgType::KeepAlive:
        if (state == State::Searching && has_pending && sender == pending.id) {
            // The peer already considers the link up: our Accept arrived late or got lost.
            BecomeConnected(from, RoleRequire, pending_network_id);
        }
        if (state == State::Connected && sender == peer.id) {
            last_heard = Clock::now();
            if (type == MsgType::Data) {
                ++received_packets;
                // Diagnostics: is the game making progress or resending the same frames?
                // Bytes 0..11 look like a per-frame header, so also compare from byte 12 on.
                if (!seen_payloads.insert(HashBytes(body)).second) {
                    ++repeated_payloads;
                }
                if (body.size() > 12 && !seen_bodies.insert(HashBytes(body.subspan(12))).second) {
                    ++repeated_bodies;
                }
                if (received_packets <= kLoggedPackets) {
                    LOG_INFO(Service_IR, "IR LAN: recv #{} ({} bytes): {:02x}", received_packets,
                             body.size(),
                             fmt::join(body.first(std::min<std::size_t>(body.size(), 48)), " "));
                }
                if (callbacks.on_data) {
                    callbacks.on_data(body);
                }
            }
        }
        break;

    case MsgType::Bye:
        if (state == State::Connected && sender == peer.id) {
            LoseConnection("peer disconnected");
        } else if (has_pending && sender == pending.id) {
            has_pending = false;
        }
        break;

    default:
        break;
    }
}

void IrLanLink::HandleBeacon(const Peer& from, Mode their_mode) {
    if (!ModesFit(mode, their_mode) || !WeInitiate(their_mode, from.id)) {
        return;
    }
    if (has_pending && pending.id != from.id) {
        return; // already talking to someone else
    }
    if (!has_pending) {
        LOG_INFO(Service_IR, "IR LAN: found peer {:08x}, connecting", from.id);
    }
    has_pending = true;
    pending = from;
    const auto now = Clock::now();
    if (now - last_connect >= kConnectRetry) {
        const std::array<u8, 1> body{pending_network_id};
        SendTo(pending, MsgType::Connect, body);
        last_connect = now;
    }
}

void IrLanLink::BecomeConnected(const Peer& new_peer, u8 role, u8 new_network_id) {
    state = State::Connected;
    has_pending = false;
    peer = new_peer;
    network_id = new_network_id;
    last_heard = Clock::now();
    last_sent = Clock::now();
    last_stats = Clock::now();
    sent_packets = 0;
    received_packets = 0;
    repeated_payloads = 0;
    repeated_bodies = 0;
    seen_payloads.clear();
    seen_bodies.clear();
    LOG_INFO(Service_IR, "IR LAN: connected to {:08x} as {} (network id {:02x})", peer.id,
             role == RoleRequire ? "requiring side" : "waiting side", network_id);
    if (callbacks.on_connected) {
        callbacks.on_connected(role, network_id);
    }
}

void IrLanLink::LoseConnection(const char* reason) {
    LOG_INFO(Service_IR, "IR LAN: {}", reason);
    CloseSockets();
    state = State::Idle;
    has_pending = false;
    if (callbacks.on_disconnected) {
        callbacks.on_disconnected();
    }
}

} // namespace Service::IR
