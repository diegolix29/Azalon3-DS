// Copyright 2026 Azalon3-DS Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_set>
#include <vector>
#include "common/common_types.h"

namespace Service::IR {

/**
 * Stand-in for the IR port when two 3DS consoles talk to each other through ir:USER
 * (WaitConnection / RequireConnection / AutoConnection / AnyConnection).
 *
 * Every emulator instance that is trying to connect broadcasts a small beacon on the LAN
 * (UDP port kDiscoveryPort, shared by all instances on the machine through SO_REUSEADDR).
 * Two instances whose connection modes fit (wait <-> require, auto <-> anything) pair up over
 * unicast UDP, after which ir:USER payloads are forwarded as-is. This works between two
 * instances on the same PC and between machines on the same local network.
 *
 * Not thread safe: everything, including the callbacks, runs on the emulation thread
 * (Poll() is driven by a CoreTiming event owned by IR_USER).
 */
class IrLanLink {
public:
    enum class Mode : u8 {
        Wait = 1,    ///< ir:USER WaitConnection: wait for the other console to connect
        Require = 2, ///< ir:USER RequireConnection: connect to a waiting console
        Auto = 3,    ///< ir:USER AutoConnection / AnyConnection: either side
    };

    /// Values ir:USER reports with GetConnectionRole.
    static constexpr u8 RoleRequire = 2; // same value a real console reports with the Circle Pad Pro
    static constexpr u8 RoleWait = 1;

    static constexpr u16 kDiscoveryPort = 7073;

    struct Callbacks {
        std::function<void(u8 role, u8 network_id)> on_connected;
        std::function<void()> on_disconnected;
        std::function<void(std::span<const u8> payload)> on_data;
    };

    explicit IrLanLink(Callbacks callbacks);
    ~IrLanLink();

    IrLanLink(const IrLanLink&) = delete;
    IrLanLink& operator=(const IrLanLink&) = delete;

    /// Starts looking for a peer. Returns false if the sockets could not be opened.
    bool Start(Mode mode);

    /// Drops the link. With notify_peer the peer is told right away (it would time out anyway).
    /// Never calls on_disconnected.
    void Stop(bool notify_peer);

    /// Searching or connected.
    bool IsActive() const {
        return state != State::Idle;
    }
    bool IsConnected() const {
        return state == State::Connected;
    }

    /// Sends one ir:USER payload to the connected peer. Returns false if not connected.
    bool Send(std::span<const u8> payload);

    /// Receives pending datagrams, sends beacons/keepalives and detects timeouts.
    void Poll();

private:
    using Clock = std::chrono::steady_clock;

    enum class State { Idle, Searching, Connected };

    enum class MsgType : u8 {
        Beacon = 1,
        Connect = 2,
        Accept = 3,
        Data = 4,
        KeepAlive = 5,
        Bye = 6,
    };

    struct Peer {
        u32 id = 0;
        u32 host = 0; // ENetAddress::host (network byte order)
        u16 port = 0;
    };

    bool OpenSockets();
    void CloseSockets();
    void SendTo(const Peer& peer, MsgType type, std::span<const u8> body);
    void SendBeacon();
    void HandleDatagram(std::span<const u8> datagram, u32 from_host, u16 from_port);
    void HandleBeacon(const Peer& from, Mode their_mode);
    bool WeInitiate(Mode their_mode, u32 their_id) const;
    void BecomeConnected(const Peer& peer, u8 role, u8 network_id);
    void LoseConnection(const char* reason);
    void DrainSocket(std::size_t socket_index);

    Callbacks callbacks;

    State state = State::Idle;
    Mode mode = Mode::Auto;
    u32 own_id = 0;

    // ENetSocket values, kept as intptr_t-sized integers to keep enet.h out of this header.
    // [0] = discovery socket (bound to kDiscoveryPort), [1] = data socket (ephemeral port).
    std::intptr_t sockets[2] = {-1, -1};

    // Searching
    bool has_pending = false; // we sent Connect and wait for Accept
    Peer pending{};
    u8 pending_network_id = 0;
    Clock::time_point last_beacon{};
    Clock::time_point last_connect{};

    // Connected
    Peer peer{};
    u8 network_id = 0;
    Clock::time_point last_heard{};
    Clock::time_point last_sent{};
    Clock::time_point last_stats{};
    u32 sent_packets = 0;
    u32 received_packets = 0;
    u32 repeated_payloads = 0;
    u32 repeated_bodies = 0;
    std::unordered_set<u64> seen_payloads;
    std::unordered_set<u64> seen_bodies;

    std::vector<u8> scratch;
};

} // namespace Service::IR
