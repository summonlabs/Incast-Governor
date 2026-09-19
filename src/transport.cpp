// Incast Governor - socket transport and frame codec implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "incast/transport.hpp"

#include <cstring>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace incast::net {
namespace {

constexpr std::uintptr_t kInvalidHandle = ~std::uintptr_t{0};

#if defined(_WIN32)
std::mutex g_net_mutex;
int g_net_references = 0;
#endif

[[nodiscard]] int last_error_code() noexcept {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

void close_handle(std::uintptr_t handle) noexcept {
    if (handle == kInvalidHandle) return;
#if defined(_WIN32)
    ::closesocket(static_cast<SOCKET>(handle));
#else
    ::close(static_cast<int>(handle));
#endif
}

}  // namespace

Status initialize() noexcept {
#if defined(_WIN32)
    const std::lock_guard<std::mutex> guard(g_net_mutex);
    if (g_net_references == 0) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return fail(ErrorCode::IoFailure, "WSAStartup failed");
        }
    }
    g_net_references += 1;
#endif
    return ok_status();
}

void shutdown() noexcept {
#if defined(_WIN32)
    const std::lock_guard<std::mutex> guard(g_net_mutex);
    if (g_net_references > 0) {
        g_net_references -= 1;
        if (g_net_references == 0) WSACleanup();
    }
#endif
}

std::string last_socket_error() { return std::string("socket error ") + std::to_string(last_error_code()); }

Listener::~Listener() { close(); }

Listener::Listener(Listener&& other) noexcept : handle_(other.handle_), port_(other.port_) {
    other.handle_ = kInvalidHandle;
    other.port_ = 0;
}

Listener& Listener::operator=(Listener&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        port_ = other.port_;
        other.handle_ = kInvalidHandle;
        other.port_ = 0;
    }
    return *this;
}

bool Listener::valid() const noexcept { return handle_ != kInvalidHandle; }

Status Listener::bind_loopback(std::uint16_t port) {
    close();
#if defined(_WIN32)
    const SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) return fail(ErrorCode::IoFailure, "unable to create a listening socket");
#else
    const int handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle < 0) return fail(ErrorCode::IoFailure, "unable to create a listening socket");
#endif
    int reuse = 1;
    ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_handle(static_cast<std::uintptr_t>(handle));
        return fail(ErrorCode::IoFailure, "unable to bind the loopback listener");
    }
    if (::listen(handle, 64) != 0) {
        close_handle(static_cast<std::uintptr_t>(handle));
        return fail(ErrorCode::IoFailure, "unable to listen on the bound socket");
    }
    sockaddr_in bound{};
#if defined(_WIN32)
    int length = sizeof(bound);
#else
    socklen_t length = sizeof(bound);
#endif
    if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        close_handle(static_cast<std::uintptr_t>(handle));
        return fail(ErrorCode::IoFailure, "unable to query the bound endpoint");
    }
    handle_ = static_cast<std::uintptr_t>(handle);
    port_ = ntohs(bound.sin_port);
    return ok_status();
}

Outcome<Connection> Listener::accept() {
    if (!valid()) return Error{ErrorCode::Closed, "listener is not bound"};
#if defined(_WIN32)
    const SOCKET handle = ::accept(static_cast<SOCKET>(handle_), nullptr, nullptr);
    if (handle == INVALID_SOCKET) return Error{ErrorCode::IoFailure, "accept failed"};
#else
    const int handle = ::accept(static_cast<int>(handle_), nullptr, nullptr);
    if (handle < 0) return Error{ErrorCode::IoFailure, "accept failed"};
#endif
    int nodelay = 1;
    ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    return Connection(static_cast<std::uintptr_t>(handle));
}

void Listener::close() {
    if (!valid()) return;
    close_handle(handle_);
    handle_ = kInvalidHandle;
    port_ = 0;
}

Connection::~Connection() { close(); }

Connection::Connection(Connection&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalidHandle;
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = kInvalidHandle;
    }
    return *this;
}

bool Connection::valid() const noexcept { return handle_ != kInvalidHandle; }

Status Connection::connect_loopback(std::uint16_t port) {
    close();
#if defined(_WIN32)
    const SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) return fail(ErrorCode::IoFailure, "unable to create a client socket");
#else
    const int handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle < 0) return fail(ErrorCode::IoFailure, "unable to create a client socket");
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_handle(static_cast<std::uintptr_t>(handle));
        return fail(ErrorCode::IoFailure, std::string("unable to connect to the coordinator: ") + last_socket_error());
    }
    int nodelay = 1;
    ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    handle_ = static_cast<std::uintptr_t>(handle);
    return ok_status();
}

Status Connection::send_all(std::span<const std::byte> data) {
    if (!valid()) return fail(ErrorCode::Closed, "connection is closed");
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t remaining = data.size() - offset;
        const int chunk = static_cast<int>(remaining > 1u << 20 ? 1u << 20 : remaining);
#if defined(_WIN32)
        const int sent = ::send(static_cast<SOCKET>(handle_),
                                reinterpret_cast<const char*>(data.data() + offset), chunk, 0);
#else
        const int sent = static_cast<int>(::send(static_cast<int>(handle_),
                                                 reinterpret_cast<const char*>(data.data() + offset), chunk,
                                                 MSG_NOSIGNAL));
#endif
        if (sent <= 0) return fail(ErrorCode::IoFailure, "send failed");
        offset += static_cast<std::size_t>(sent);
    }
    return ok_status();
}

Outcome<std::size_t> Connection::recv_some(std::span<std::byte> buffer) {
    if (!valid()) return Error{ErrorCode::Closed, "connection is closed"};
    if (buffer.empty()) return std::size_t{0};
    const int chunk = static_cast<int>(buffer.size() > 1u << 20 ? 1u << 20 : buffer.size());
#if defined(_WIN32)
    const int received = ::recv(static_cast<SOCKET>(handle_), reinterpret_cast<char*>(buffer.data()), chunk, 0);
#else
    const int received = static_cast<int>(::recv(static_cast<int>(handle_), reinterpret_cast<char*>(buffer.data()),
                                                 chunk, 0));
#endif
    if (received < 0) return Error{ErrorCode::IoFailure, "receive failed"};
    return static_cast<std::size_t>(received);
}

void Connection::shutdown_send() {
    if (!valid()) return;
#if defined(_WIN32)
    ::shutdown(static_cast<SOCKET>(handle_), SD_SEND);
#else
    ::shutdown(static_cast<int>(handle_), SHUT_WR);
#endif
}

void Connection::shutdown_both() {
    if (!valid()) return;
#if defined(_WIN32)
    ::shutdown(static_cast<SOCKET>(handle_), SD_BOTH);
#else
    ::shutdown(static_cast<int>(handle_), SHUT_RDWR);
#endif
}

void Connection::close() {
    if (!valid()) return;
    close_handle(handle_);
    handle_ = kInvalidHandle;
}

}  // namespace incast::net

namespace incast {

Outcome<std::vector<std::byte>> encode_frame(const Frame& frame) {
    const std::size_t size = frame.payload.size();
    if (size > kMaxFrameBytes) {
        return Error{ErrorCode::OversizedInput, "frame payload exceeds the supported bound"};
    }
    const std::size_t capped = size;
    std::vector<std::byte> buffer;
    buffer.reserve(kFrameHeaderBytes + capped);
    const auto push_u16 = [&buffer](std::uint16_t value) {
        for (int shift = 0; shift < 16; shift += 8) {
            buffer.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
        }
    };
    const auto push_u32 = [&buffer](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            buffer.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
        }
    };
    push_u32(kFrameMagic);
    push_u16(kWireProtocolVersion);
    push_u16(static_cast<std::uint16_t>(frame.type));
    push_u16(frame.flags);
    push_u16(0);
    push_u32(static_cast<std::uint32_t>(capped));
    push_u32(crc32c(frame.payload.data(), capped));
    buffer.insert(buffer.end(), frame.payload.begin(), frame.payload.begin() + static_cast<std::ptrdiff_t>(capped));
    return buffer;
}

namespace {

std::uint32_t load_u32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + static_cast<std::size_t>(index)]))
                 << (index * 8);
    }
    return value;
}

std::uint16_t load_u16(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint16_t value = 0;
    for (int index = 0; index < 2; ++index) {
        value |= static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[offset + static_cast<std::size_t>(index)]))
                 << (index * 8);
    }
    return value;
}

}  // namespace

Outcome<Frame> decode_frame_header(std::span<const std::byte> header, std::size_t& payload_length) {
    payload_length = 0;
    if (header.size() < kFrameHeaderBytes) {
        return Error{ErrorCode::TruncatedInput, "frame header is incomplete"};
    }
    if (load_u32(header, 0) != kFrameMagic) {
        return Error{ErrorCode::ProtocolViolation, "frame magic mismatch"};
    }
    if (load_u16(header, 4) != kWireProtocolVersion) {
        return Error{ErrorCode::UnsupportedVersion, "unsupported wire protocol version"};
    }
    const std::uint16_t raw_type = load_u16(header, 6);
    if (raw_type == 0 || raw_type > kMaxFrameType) {
        return Error{ErrorCode::ProtocolViolation, "frame type is outside the known set"};
    }
    if (load_u16(header, 10) != 0) {
        return Error{ErrorCode::ProtocolViolation, "frame reserved field must be zero"};
    }
    const std::uint32_t length = load_u32(header, 12);
    if (length > kMaxFrameBytes) {
        return Error{ErrorCode::OversizedInput, "frame payload exceeds the supported bound"};
    }
    payload_length = length;
    Frame frame{};
    frame.type = static_cast<FrameType>(raw_type);
    frame.flags = load_u16(header, 8);
    return frame;
}

Outcome<Frame> decode_frame(std::span<const std::byte> bytes) {
    std::size_t payload_length = 0;
    auto header = decode_frame_header(bytes, payload_length);
    if (!header) return header.error();
    if (bytes.size() < kFrameHeaderBytes + payload_length) {
        return Error{ErrorCode::TruncatedInput, "frame payload is incomplete"};
    }
    const auto payload = bytes.subspan(kFrameHeaderBytes, payload_length);
    const std::uint32_t expected = load_u32(bytes, 16);
    if (crc32c(payload.data(), payload.size()) != expected) {
        return Error{ErrorCode::ChecksumMismatch, "frame payload failed its integrity check"};
    }
    Frame frame = header.value();
    frame.payload.assign(payload.begin(), payload.end());
    return frame;
}

FramedChannel::FramedChannel(net::Connection connection) : connection_(std::move(connection)) {}

Status FramedChannel::send(const Frame& frame) {
    if (frame.payload.size() > kMaxFrameBytes) {
        return fail(ErrorCode::OversizedInput, "frame payload exceeds the supported bound");
    }
    auto bytes = encode_frame(frame);
    if (!bytes) return fail(bytes.error().code, bytes.error().detail);
    const Status sent = connection_.send_all(bytes.value());
    if (!sent) return sent;
    frames_sent_ += 1;
    return ok_status();
}

Outcome<Frame> FramedChannel::receive() {
    std::array<std::byte, kFrameHeaderBytes> header{};
    std::size_t filled = 0;
    while (filled < header.size()) {
        auto chunk = connection_.recv_some(std::span<std::byte>(header.data() + filled, header.size() - filled));
        if (!chunk) {
            protocol_errors_ += 1;
            return chunk.error();
        }
        if (chunk.value() == 0) {
            return Error{ErrorCode::Closed, "peer closed the connection"};
        }
        filled += chunk.value();
    }
    std::size_t payload_length = 0;
    auto header_frame = decode_frame_header(std::span<const std::byte>(header.data(), header.size()), payload_length);
    if (!header_frame) {
        protocol_errors_ += 1;
        return header_frame.error();
    }
    Frame frame = header_frame.value();
    frame.payload.resize(payload_length);
    std::size_t received = 0;
    while (received < payload_length) {
        auto chunk = connection_.recv_some(
            std::span<std::byte>(frame.payload.data() + received, payload_length - received));
        if (!chunk) {
            protocol_errors_ += 1;
            return chunk.error();
        }
        if (chunk.value() == 0) {
            protocol_errors_ += 1;
            return Error{ErrorCode::TruncatedInput, "peer closed mid-frame"};
        }
        received += chunk.value();
    }
    const std::array<std::byte, 4> checksum_bytes{header[16], header[17], header[18], header[19]};
    const std::uint32_t expected = load_u32(std::span<const std::byte>(checksum_bytes.data(), checksum_bytes.size()), 0);
    if (crc32c(frame.payload.data(), frame.payload.size()) != expected) {
        protocol_errors_ += 1;
        return Error{ErrorCode::ChecksumMismatch, "frame payload failed its integrity check"};
    }
    frames_received_ += 1;
    return frame;
}

void FramedChannel::close() { connection_.close(); }

void FramedChannel::shutdown() { connection_.shutdown_both(); }

}  // namespace incast
