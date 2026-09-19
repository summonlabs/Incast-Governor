// Incast Governor - real OS socket transport with length-prefixed, CRC-checked
// frames. Used by the coordinator/worker multiprocess runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_TRANSPORT_HPP
#define INCAST_TRANSPORT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "incast/codec.hpp"
#include "incast/core.hpp"

namespace incast::net {

// One-time process-wide networking initialisation. Idempotent.
[[nodiscard]] Status initialize() noexcept;
void shutdown() noexcept;
[[nodiscard]] std::string last_socket_error();

class Connection;

// Listening socket bound to a loopback or wildcard endpoint.
class Listener {
public:
    Listener() = default;
    ~Listener();
    Listener(Listener&& other) noexcept;
    Listener& operator=(Listener&& other) noexcept;
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    [[nodiscard]] Status bind_loopback(std::uint16_t port);
    [[nodiscard]] std::uint16_t bound_port() const noexcept { return port_; }
    [[nodiscard]] Outcome<Connection> accept();
    void close();
    [[nodiscard]] bool valid() const noexcept;

private:
    std::uintptr_t handle_ = ~std::uintptr_t{0};
    std::uint16_t port_ = 0;
};

class Connection {
public:
    Connection() = default;
    ~Connection();
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] Status connect_loopback(std::uint16_t port);
    [[nodiscard]] Status send_all(std::span<const std::byte> data);
    // Returns the number of bytes read; 0 means the peer closed cleanly.
    [[nodiscard]] Outcome<std::size_t> recv_some(std::span<std::byte> buffer);
    void shutdown_send();
    // Half-closes both directions. Safe to call from a different thread than
    // the one blocked in recv_some(); this is the documented interruption path.
    void shutdown_both();
    void close();
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uintptr_t native_handle() const noexcept { return handle_; }

private:
    explicit Connection(std::uintptr_t handle) : handle_(handle) {}
    friend class Listener;

    std::uintptr_t handle_ = ~std::uintptr_t{0};
};

}  // namespace incast::net

namespace incast {

// ---------------------------------------------------------------------------
// Frame wire format (20-byte header, little-endian):
//   magic     u32   'IGF1' == 0x31464749
//   version   u16   protocol version
//   type      u16   FrameType
//   flags     u16
//   reserved  u16   must be zero
//   length    u32   payload length, bounded by kMaxFrameBytes
//   crc32c    u32   CRC of the payload
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kFrameMagic = 0x31464749u;
inline constexpr std::size_t kFrameHeaderBytes = 20;

enum class FrameType : std::uint16_t {
    Hello = 1,
    HelloAck = 2,
    LeaseRequest = 3,
    LeaseGrant = 4,
    LeaseDenied = 5,
    LeaseRenew = 6,
    LeaseRevoke = 7,
    EpochAdvance = 8,
    DecisionPublish = 9,
    DecisionAck = 10,
    Heartbeat = 11,
    HeartbeatAck = 12,
    FenceNotice = 13,
    Shutdown = 14,
    Error = 15,
    ControlRequest = 16,
    ControlResponse = 17,
};

inline constexpr std::uint16_t kMaxFrameType = static_cast<std::uint16_t>(FrameType::ControlResponse);

[[nodiscard]] constexpr std::string_view to_string(FrameType type) noexcept {
    switch (type) {
        case FrameType::Hello: return "Hello";
        case FrameType::HelloAck: return "HelloAck";
        case FrameType::LeaseRequest: return "LeaseRequest";
        case FrameType::LeaseGrant: return "LeaseGrant";
        case FrameType::LeaseDenied: return "LeaseDenied";
        case FrameType::LeaseRenew: return "LeaseRenew";
        case FrameType::LeaseRevoke: return "LeaseRevoke";
        case FrameType::EpochAdvance: return "EpochAdvance";
        case FrameType::DecisionPublish: return "DecisionPublish";
        case FrameType::DecisionAck: return "DecisionAck";
        case FrameType::Heartbeat: return "Heartbeat";
        case FrameType::HeartbeatAck: return "HeartbeatAck";
        case FrameType::FenceNotice: return "FenceNotice";
        case FrameType::Shutdown: return "Shutdown";
        case FrameType::Error: return "Error";
        case FrameType::ControlRequest: return "ControlRequest";
        case FrameType::ControlResponse: return "ControlResponse";
    }
    return "Unknown";
}

struct Frame {
    FrameType type = FrameType::Error;
    std::uint16_t flags = 0;
    std::vector<std::byte> payload{};
};

// Refuses to encode a frame whose payload exceeds kMaxFrameBytes rather than
// silently truncating it: a truncated control frame is a correctness hazard.
[[nodiscard]] Outcome<std::vector<std::byte>> encode_frame(const Frame& frame);
[[nodiscard]] Outcome<Frame> decode_frame_header(std::span<const std::byte> header, std::size_t& payload_length);
[[nodiscard]] Outcome<Frame> decode_frame(std::span<const std::byte> bytes);

// ---------------------------------------------------------------------------
// FramedChannel: reads and writes whole frames over a Connection. Handles
// partial reads, rejects oversized or malformed frames, and never blocks on a
// partially consumed frame.
// ---------------------------------------------------------------------------

class FramedChannel {
public:
    FramedChannel() = default;
    explicit FramedChannel(net::Connection connection);

    [[nodiscard]] Status send(const Frame& frame);
    [[nodiscard]] Outcome<Frame> receive();
    [[nodiscard]] bool valid() const noexcept { return connection_.valid(); }
    void close();
    void shutdown();
    [[nodiscard]] const net::Connection& connection() const noexcept { return connection_; }

    [[nodiscard]] std::uint64_t frames_sent() const noexcept { return frames_sent_; }
    [[nodiscard]] std::uint64_t frames_received() const noexcept { return frames_received_; }
    [[nodiscard]] std::uint64_t protocol_errors() const noexcept { return protocol_errors_; }

private:
    net::Connection connection_{};
    std::uint64_t frames_sent_ = 0;
    std::uint64_t frames_received_ = 0;
    std::uint64_t protocol_errors_ = 0;
};

}  // namespace incast

#endif  // INCAST_TRANSPORT_HPP
