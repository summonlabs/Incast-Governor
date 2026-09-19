// Incast Governor - coordinator/worker multiprocess governance runtime.
//
// The coordinator owns the epoch and the destination lease table. Workers own a
// governor per leased destination and may only publish decisions bound to the
// epoch, boot incarnation, worker and lease that the coordinator granted.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_COORDINATOR_HPP
#define INCAST_COORDINATOR_HPP

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "incast/core.hpp"
#include "incast/detect.hpp"
#include "incast/governor.hpp"
#include "incast/ids.hpp"
#include "incast/model.hpp"
#include "incast/transport.hpp"

namespace incast {

// ---------------------------------------------------------------------------
// Protocol messages.
// ---------------------------------------------------------------------------

struct MessageHello {
    WorkerId worker{};
    BootIncarnation boot{};
    std::string name{};
};

struct MessageHelloAck {
    std::uint64_t session = 0;
    EpochId epoch{};
    BootIncarnation coordinator_boot{};
};

struct MessageLeaseRequest {
    WorkerId worker{};
    BootIncarnation boot{};
    DestinationId destination{};
    DestinationGeneration destination_generation{};
    PolicyId policy{};
    PolicyGeneration policy_generation{};
    std::uint64_t attempt = 1;
};

struct MessageLeaseGrant {
    LeaseId lease{};
    DestinationId destination{};
    DestinationGeneration destination_generation{};
    EpochId epoch{};
    BootIncarnation boot{};
    WorkerId worker{};
    PolicyId policy{};
    PolicyGeneration policy_generation{};
    Timestamp granted_at{};
    Timestamp expires_at{};
    std::uint64_t attempt = 1;
};

struct MessageLeaseDenied {
    RefusalReason reason = RefusalReason::EpochMismatch;
    std::string detail{};
    EpochId current_epoch{};
};

struct MessageLeaseRenew {
    LeaseId lease{};
    EpochId epoch{};
    BootIncarnation boot{};
    WorkerId worker{};
    std::uint64_t attempt = 1;
};

struct MessageEpochAdvance {
    EpochId epoch{};
    std::string reason{};
};

struct MessageDecisionPublish {
    LeaseId lease{};
    EpochId epoch{};
    BootIncarnation boot{};
    WorkerId worker{};
    DestinationId destination{};
    WindowId window{};
    WindowGeneration window_generation{};
    IncidentKind kind = IncidentKind::Unknown;
    Severity severity = Severity::None;
    DecisionOutcome outcome = DecisionOutcome::Refused;
    MitigationKind mitigation = MitigationKind::None;
    InterventionId intervention{};
    BasisPoints reduction_bp{};
    StateGeneration state_generation{};
    Timestamp decided_at{};
    std::uint64_t sequence = 0;
};

struct MessageDecisionAck {
    bool accepted = false;
    RefusalReason reason = RefusalReason::None;
    std::string detail{};
    EpochId current_epoch{};
};

struct MessageError {
    ErrorCode code = ErrorCode::None;
    std::string detail{};
};

[[nodiscard]] std::vector<std::byte> encode_hello(const MessageHello& message);
[[nodiscard]] Outcome<MessageHello> decode_hello(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_hello_ack(const MessageHelloAck& message);
[[nodiscard]] Outcome<MessageHelloAck> decode_hello_ack(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_lease_request(const MessageLeaseRequest& message);
[[nodiscard]] Outcome<MessageLeaseRequest> decode_lease_request(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_lease_grant(const MessageLeaseGrant& message);
[[nodiscard]] Outcome<MessageLeaseGrant> decode_lease_grant(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_lease_denied(const MessageLeaseDenied& message);
[[nodiscard]] Outcome<MessageLeaseDenied> decode_lease_denied(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_lease_renew(const MessageLeaseRenew& message);
[[nodiscard]] Outcome<MessageLeaseRenew> decode_lease_renew(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_epoch_advance(const MessageEpochAdvance& message);
[[nodiscard]] Outcome<MessageEpochAdvance> decode_epoch_advance(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_decision_publish(const MessageDecisionPublish& message);
[[nodiscard]] Outcome<MessageDecisionPublish> decode_decision_publish(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_decision_ack(const MessageDecisionAck& message);
[[nodiscard]] Outcome<MessageDecisionAck> decode_decision_ack(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_error(const MessageError& message);
[[nodiscard]] Outcome<MessageError> decode_error(std::span<const std::byte> bytes);

// ---------------------------------------------------------------------------
// Coordinator control plane (out-of-band, token authenticated).
// ---------------------------------------------------------------------------

enum class ControlOpcode : std::uint32_t {
    Status = 1,
    AdvanceEpoch = 2,
    Shutdown = 3,
    RevokeAll = 4,
};

struct ControlRequest {
    ControlOpcode opcode = ControlOpcode::Status;
    std::string token{};
    std::string argument{};
};

struct ControlResponse {
    bool ok = false;
    std::string detail{};
    EpochId epoch{};
    std::uint64_t lease_count = 0;
    std::uint64_t decisions_accepted = 0;
    std::uint64_t decisions_rejected = 0;
};

[[nodiscard]] std::vector<std::byte> encode_control_request(const ControlRequest& message);
[[nodiscard]] Outcome<ControlRequest> decode_control_request(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_control_response(const ControlResponse& message);
[[nodiscard]] Outcome<ControlResponse> decode_control_response(std::span<const std::byte> bytes);

// ---------------------------------------------------------------------------
// Epoch ledger: durable, monotonic epoch storage. A coordinator restart may
// never reuse an epoch, so the ledger is written before a grant is issued.
// ---------------------------------------------------------------------------

class EpochLedger {
public:
    EpochLedger() = default;

    [[nodiscard]] Status open(const std::filesystem::path& directory);
    [[nodiscard]] Outcome<EpochId> load();
    [[nodiscard]] Status store(EpochId epoch);
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_{};
};

// ---------------------------------------------------------------------------
// Coordinator.
// ---------------------------------------------------------------------------

struct CoordinatorConfig {
    std::uint16_t port = 0;                 // 0 selects an ephemeral port
    Duration lease_duration{2'000'000'000};
    std::size_t max_connections = 64;
    std::string control_token{};
    std::filesystem::path directory{};      // empty disables the epoch ledger
    EpochId initial_epoch{1};
};

struct LeaseRecord {
    MessageLeaseGrant grant{};
    bool revoked = false;
    bool expired = false;
    std::uint64_t lease_sequence = 0;
    Timestamp last_decision_at{};
    IncidentKind last_kind = IncidentKind::Unknown;
};

class Coordinator {
public:
    explicit Coordinator(CoordinatorConfig config);
    ~Coordinator();

    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    [[nodiscard]] Status start();
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] EpochId epoch() const;
    [[nodiscard]] BootIncarnation boot() const noexcept { return boot_; }

    // Advances the coordinator epoch. Every advance durably re-writes the
    // ledger (when configured) before becoming visible.
    [[nodiscard]] Status advance_epoch(std::string reason);

    // Runs the accept/serve loop on the calling thread until stop() is called.
    [[nodiscard]] Status run();

    // Serves exactly one accepted connection to completion, then returns.
    // Deterministic and timeout-free; used by single-process integration tests.
    [[nodiscard]] Status serve_one();

    struct Statistics {
        std::uint64_t connections = 0;
        std::uint64_t leases_granted = 0;
        std::uint64_t leases_denied = 0;
        std::uint64_t leases_renewed = 0;
        std::uint64_t decisions_accepted = 0;
        std::uint64_t decisions_rejected = 0;
        std::uint64_t epoch_advances = 0;
        std::uint64_t frames_in = 0;
        std::uint64_t frames_out = 0;
    };

    [[nodiscard]] Statistics statistics() const;
    [[nodiscard]] std::size_t live_lease_count() const;

private:
    [[nodiscard]] Status handle_connection(FramedChannel& channel);
    [[nodiscard]] Status handle_control(FramedChannel& channel, const ControlRequest& request);
    [[nodiscard]] Outcome<MessageLeaseGrant> grant_lease(const MessageLeaseRequest& request, Timestamp now);

    CoordinatorConfig config_{};
    std::uint16_t port_ = 0;
    net::Listener listener_{};
    BootIncarnation boot_{};
    std::uint64_t next_session_ = 1;
    std::uint64_t next_lease_ = 1;

    // Guards the epoch and the lease table only. Never held across a socket
    // operation, a join or a callback.
    mutable std::mutex mutex_{};
    EpochId epoch_{};
    std::unordered_map<std::uint64_t, LeaseRecord> leases_{};
    Statistics statistics_{};
    EpochLedger ledger_{};
    bool ledger_open_ = false;

    // Session registry. Ordered after mutex_ everywhere, so the two locks can
    // never be acquired in opposite orders.
    mutable std::mutex sessions_mutex_{};
    std::vector<std::shared_ptr<FramedChannel>> sessions_{};
    std::vector<std::thread> workers_{};

    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};
};

// ---------------------------------------------------------------------------
// Worker: connects to the coordinator, acquires a destination lease and
// publishes governor decisions bound to that lease.
// ---------------------------------------------------------------------------

struct WorkerConfig {
    std::uint16_t coordinator_port = 0;
    std::string coordinator_host{"127.0.0.1"};
    WorkerId worker{};
    BootIncarnation boot{};
    DestinationId destination{};
    DestinationGeneration destination_generation{};
    PolicyId policy{};
    PolicyGeneration policy_generation{};
    std::string name{"worker"};
};

class GovernorWorker {
public:
    explicit GovernorWorker(WorkerConfig config);
    ~GovernorWorker();

    GovernorWorker(const GovernorWorker&) = delete;
    GovernorWorker& operator=(const GovernorWorker&) = delete;

    [[nodiscard]] Status connect_and_handshake();
    [[nodiscard]] Outcome<MessageLeaseGrant> acquire_lease();
    [[nodiscard]] Outcome<MessageLeaseGrant> renew_lease();

    // Publishes a decision. A rejection is returned as a successful outcome
    // carrying accepted == false: rejection is an expected, authoritative
    // answer, not a transport failure.
    [[nodiscard]] Outcome<MessageDecisionAck> publish(const Decision& decision);

    [[nodiscard]] Status send_heartbeat();

    [[nodiscard]] bool fenced() const noexcept { return fenced_; }
    [[nodiscard]] const MessageLeaseGrant& lease() const noexcept { return lease_; }
    [[nodiscard]] bool handshaken() const noexcept { return handshaken_; }

    [[nodiscard]] EpochId epoch() const noexcept { return epoch_; }
    [[nodiscard]] BootIncarnation boot() const noexcept { return config_.boot; }
    [[nodiscard]] WorkerId worker() const noexcept { return config_.worker; }

    void close();

private:
    WorkerConfig config_{};
    FramedChannel channel_{};
    EpochId epoch_{};
    bool handshaken_ = false;
    bool fenced_ = false;
    std::uint64_t sequence_ = 0;
    MessageLeaseGrant lease_{};
};

// Establishes a single control round trip. Used by tests and by the CLI.
[[nodiscard]] Outcome<ControlResponse> send_control(std::uint16_t port,
                                                    const ControlRequest& request);

// Builds a per-process boot incarnation. Never derived from durable state.
[[nodiscard]] BootIncarnation mint_boot_incarnation();
[[nodiscard]] WorkerId mint_worker_id(std::string_view name);

}  // namespace incast

#endif  // INCAST_COORDINATOR_HPP
