// Incast Governor - coordinator/worker implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "incast/coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <utility>

#include "incast/codec.hpp"
#include "incast/durable.hpp"

namespace incast {
namespace {

inline constexpr std::uint32_t kLeaseLedgerMagic = 0x50454749u;  // 'IGEP'

[[nodiscard]] Timestamp coordinator_now() noexcept { return monotonic_now(); }

}  // namespace

// ---------------------------------------------------------------------------
// Message codecs
// ---------------------------------------------------------------------------

std::vector<std::byte> encode_hello(const MessageHello& message) {
    codec::Writer writer;
    writer.u64(message.worker.value());
    writer.u64(message.boot.value());
    writer.str(message.name);
    return writer.take();
}

Outcome<MessageHello> decode_hello(std::span<const std::byte> bytes) {
    codec::Reader reader(bytes);
    MessageHello message{};
    auto worker = reader.u64();
    if (!worker) return worker.error();
    message.worker = WorkerId{worker.value()};
    auto boot = reader.u64();
    if (!boot) return boot.error();
    message.boot = BootIncarnation{boot.value()};
    auto name = reader.str();
    if (!name) return name.error();
    message.name = name.value();
    if (!reader.exhausted()) return Error{ErrorCode::MalformedInput, "trailing bytes in Hello"};
    return message;
}

std::vector<std::byte> encode_hello_ack(const MessageHelloAck& message) {
    codec::Writer writer;
    writer.u64(message.session);
    writer.u64(message.epoch.value());
    writer.u64(message.coordinator_boot.value());
    return writer.take();
}

Outcome<MessageHelloAck> decode_hello_ack(std::span<const std::byte> bytes) {
    codec::Reader reader(bytes);
    MessageHelloAck message{};
    auto session = reader.u64();
    if (!session) return session.error();
    message.session = session.value();
    auto epoch = reader.u64();
    if (!epoch) return epoch.error();
    message.epoch = EpochId{epoch.value()};
    auto boot = reader.u64();
    if (!boot) return boot.error();
    message.coordinator_boot = BootIncarnation{boot.value()};
    if (!reader.exhausted()) return Error{ErrorCode::MalformedInput, "trailing bytes in HelloAck"};
    return message;
}

std::vector<std::byte> encode_lease_request(const MessageLeaseRequest& message) {
    codec::Writer writer;
    writer.u64(message.worker.value());
    writer.u64(message.boot.value());
    writer.u64(message.destination.value());
    writer.u64(message.destination_generation.value());
    writer.u64(message.policy.value());
    writer.u64(message.policy_generation.value());
    writer.u64(message.attempt);
    return writer.take();
}

Outcome<MessageLeaseRequest> decode_lease_request(std::span<const std::byte> bytes) {
    codec::Reader reader(bytes);
    MessageLeaseRequest message{};
    std::uint64_t raw = 0;
    const auto take = [&reader, &raw]() -> Status {
        auto value = reader.u64();
        if (!value) return fail(value.error().code, value.error().detail);
        raw = value.value();
        return ok_status();
    };
    Status status = take(); if (!status) return status.error();
    message.worker = WorkerId{raw};
    status = take(); if (!status) return status.error();
    message.boot = BootIncarnation{raw};
    status = take(); if (!status) return status.error();
    message.destination = DestinationId{raw};
    status = take(); if (!status) return status.error();
    message.destination_generation = DestinationGeneration{raw};
    status = take(); if (!status) return status.error();
    message.policy = PolicyId{raw};
    status = take(); if (!status) return status.error();
    message.policy_generation = PolicyGeneration{raw};
    status = take(); if (!status) return status.error();
    message.attempt = raw;
    if (!reader.exhausted()) return Error{ErrorCode::MalformedInput, "trailing bytes in LeaseRequest"};
    return message;
}

std::vector<std::byte> encode_lease_grant(const MessageLeaseGrant& message) {
    codec::Writer writer;
    writer.u64(message.lease.value());
    writer.u64(message.destination.value());
    writer.u64(message.destination_generation.value());
    writer.u64(message.epoch.value());
    writer.u64(message.boot.value());
    writer.u64(message.worker.value());
    writer.u64(message.policy.value());
    writer.u64(message.policy_generation.value());
    writer.i64(message.granted_at.nanos());
    writer.i64(message.expires_at.nanos());
    writer.u64(message.attempt);
    return writer.take();
}

Outcome<MessageLeaseGrant> decode_lease_grant(std::span<const std::byte> bytes) {
    codec::Reader reader(bytes);
    MessageLeaseGrant message{};
    std::uint64_t raw = 0;
    const auto take = [&reader, &raw]() -> Status {
        auto value = reader.u64();
        if (!value) return fail(value.error().code, value.error().detail);
        raw = value.value();
        return ok_status();
    };
    Status status = take(); if (!status) return status.error();
    message.lease = LeaseId{raw};
    status = take(); if (!status) return status.error();
    message.destination = DestinationId{raw};
    status = take(); if (!status) return status.error();
    message.destination_generation = DestinationGeneration{raw};
    status = take(); if (!status) return status.error();
    message.epoch = EpochId{raw};
    status = take(); if (!status) return status.error();
    message.boot = BootIncarnation{raw};
    status = take(); if (!status) return status.error();
    message.worker = WorkerId{raw};
    status = take(); if (!status) return status.error();
    message.policy = PolicyId{raw};
    status = take(); if (!status) return status.error();
    message.policy_generation = PolicyGeneration{raw};
    auto granted = reader.i64();
    if (!granted) return granted.error();
    message.granted_at = Timestamp{granted.value()};
    auto expires = reader.i64();
    if (!expires) return expires.error();
    message.expires_at = Timestamp{expires.value()};
    status = take(); if (!status) return status.error();
    message.attempt = raw;
    if (!reader.exhausted()) return Error{ErrorCode::MalformedInput, "trailing bytes in LeaseGrant"};
    return message;
}

std::vector<std::byte> encode_lease_denied(const MessageLeaseDenied& message) {
    codec::Writer writer;
    writer.u8(static_cast<std::uint8_t>(message.reason));
    writer.str(message.detail);
    writer.u64(message.current_epoch.value());
    return writer.take();
}

Outcome<MessageLeaseDenied> decode_lease_denied(std::span<const std::byte> bytes) {
    codec::Reader reader(bytes);
    MessageLeaseDenied message{};
    auto reason = reader.u8();
    if (!reason) return reason.error();
    if (reason.value() > static_cast<std::uint8_t>(RefusalReason::CapacityNotRevalidated)) {
        return Error{ErrorCode::ProtocolViolation, "lease denial carries an unknown refusal reason"};
    }
    message.reason = static_cast<RefusalReason>(reason.value());
    auto detail = reader.str();
    if (!detail) return detail.error();
    message.detail = detail.value();
    auto epoch = reader.u64();
    if (!epoch) return epoch.error();
    message.current_epoch = EpochId{epoch.value()};
    if (!reader.exhausted()) return Error{ErrorCode::MalformedInput, "trailing bytes in LeaseDenied"};
    return message;
}

std::vector<std::byte> encode_lease_renew(const MessageLeaseRenew& message) {
    codec::Writer writer;
    writer.u64(message.lease.value());
    writer.u64(message.epoch.value());
    writer.u64(message.boot.value());
    writer.u64(message.worker.value());
    writer.u64(message.attempt);
    return writer.take();
}

Outcome<MessageLeaseRenew> decode_lease_renew(std::span<const std::byte> bytes) {
    codec::Reader reader(bytes);
    MessageLeaseRenew message{};
    std::uint64_t raw = 0;
    const auto take = [&reader, &raw]() -> Status {
        auto value = reader.u64();
        if (!value) return fail(value.error().code, value.error().detail);
        raw = value.value();
        return ok_status();
    };
    Status status = take(); if (!status) return status.error();
    message.lease = LeaseId{raw};
    status = take(); if (!status) return status.error();
    message.epoch = EpochId{raw};
    status = take(); if (!status) return status.error();
    message.boot = BootIncarnation{raw};
    status = take(); if (!status) return status.error();
    message.worker = WorkerId{raw};
    status = take(); if (!status) return status.error();
    message.attempt = raw;
    if (!reader.exhausted()) return Error{ErrorCode::MalformedInput, "trailing bytes in LeaseRenew"};
    return message;
}

std::vector<std::byte> encode_epoch_advance(const MessageEpochAdvance& message) {
    codec::Writer writer;
    writer.u64(message.epoch.value());
    writer.str(message.reason);
    return writer.take();
}

Outcome<MessageEpochAdvance> decode_epoch_advance(std::span<const std::byte> bytes) {
    codec::Reader reader(bytes);
    MessageEpochAdvance message{};
    auto epoch = reader.u64();
    if (!epoch) return epoch.error();
    message.epoch = EpochId{epoch.value()};
    auto reason = reader.str();
    if (!reason) return reason.error();
    message.reason = reason.value();
    if (!reader.exhausted()) return Error{ErrorCode::MalformedInput, "trailing bytes in EpochAdvance"};
    return message;
}

std::vector<std::byte> encode_decision_publish(const MessageDecisionPublish& message) {
    codec::Writer writer;
    writer.u64(message.lease.value());
    writer.u64(message.epoch.value());
    writer.u64(message.boot.value());
    writer.u64(message.worker.value());
    writer.u64(message.destination.value());
    writer.u64(message.window.value());
    writer.u64(message.window_generation.value());
    writer.u8(static_cast<std::uint8_t>(message.kind));
    writer.u8(static_cast<std::uint8_t>(message.severity));
    writer.u8(static_cast<std::uint8_t>(message.outcome));
    writer.u8(static_cast<std::uint8_t>(message.mitigation));
    writer.u64(message.intervention.value());
    writer.u32(message.reduction_bp.value());
    writer.u64(message.state_generation.value());
    writer.i64(message.decided_at.nanos());
    writer.u64(message.sequence);
    return writer.take();
}

Outcome<MessageDecisionPublish> decode_decision_publish(std::span<const std::byte> bytes) {
    codec::Reader reader(bytes);
    MessageDecisionPublish message{};
    std::uint64_t raw = 0;
    const auto take = [&reader, &raw]() -> Status {
        auto value = reader.u64();
        if (!value) return fail(value.error().code, value.error().detail);
        raw = value.value();
        return ok_status();
    };
    Status status = take(); if (!status) return status.error();
    message.lease = LeaseId{raw};
    status = take(); if (!status) return status.error();
    message.epoch = EpochId{raw};
    status = take(); if (!status) return status.error();
    message.boot = BootIncarnation{raw};
    status = take(); if (!status) return status.error();
    message.worker = WorkerId{raw};
    status = take(); if (!status) return status.error();
    message.destination = DestinationId{raw};
    status = take(); if (!status) return status.error();
    message.window = WindowId{raw};
    status = take(); if (!status) return status.error();
    message.window_generation = WindowGeneration{raw};

    auto kind = reader.u8();
    if (!kind) return kind.error();
    if (kind.value() > static_cast<std::uint8_t>(IncidentKind::Recovery)) {
        return Error{ErrorCode::ProtocolViolation, "publish carries an unknown incident kind"};
    }
    message.kind = static_cast<IncidentKind>(kind.value());
    auto severity = reader.u8();
    if (!severity) return severity.error();
    if (severity.value() > static_cast<std::uint8_t>(Severity::Critical)) {
        return Error{ErrorCode::ProtocolViolation, "publish carries an unknown severity"};
    }
    message.severity = static_cast<Severity>(severity.value());
    auto outcome = reader.u8();
    if (!outcome) return outcome.error();
    if (outcome.value() > static_cast<std::uint8_t>(DecisionOutcome::InterventionFenced)) {
        return Error{ErrorCode::ProtocolViolation, "publish carries an unknown decision outcome"};
    }
    message.outcome = static_cast<DecisionOutcome>(outcome.value());
    auto mitigation = reader.u8();
    if (!mitigation) return mitigation.error();
    if (mitigation.value() > static_cast<std::uint8_t>(MitigationKind::CongestionEscalation)) {
        return Error{ErrorCode::ProtocolViolation, "publish carries an unknown mitigation kind"};
    }
    message.mitigation = static_cast<MitigationKind>(mitigation.value());

    status = take(); if (!status) return status.error();
    message.intervention = InterventionId{raw};
    auto reduction = reader.u32();
    if (!reduction) return reduction.error();
    message.reduction_bp = BasisPoints{reduction.value()};
    status = take(); if (!status) return status.error();
    message.state_generation = StateGeneration{raw};
    auto decided = reader.i64();
    if (!decided) return decided.error();
    message.decided_at = Timestamp{decided.value()};
    status = take(); if (!status) return status.error();
    message.sequence = raw;
    if (!reader.exhausted()) return Error{ErrorCode::MalformedInput, "trailing bytes in DecisionPublish"};
    return message;
}

std::vector<std::byte> encode_decision_ack(const MessageDecisionAck& message) {
    codec::Writer writer;
    writer.u8(message.accepted ? 1u : 0u);
    writer.u8(static_cast<std::uint8_t>(message.reason));
    writer.str(message.detail);
    writer.u64(message.current_epoch.value());
    return writer.take();
}

Outcome<MessageDecisionAck> decode_decision_ack(std::span<const std::byte> bytes) {
    codec::Reader reader(bytes);
    MessageDecisionAck message{};
    auto accepted = reader.u8();
    if (!accepted) return accepted.error();
    message.accepted = accepted.value() != 0;
    auto reason = reader.u8();
    if (!reason) return reason.error();
    if (reason.value() > static_cast<std::uint8_t>(RefusalReason::CapacityNotRevalidated)) {
        return Error{ErrorCode::ProtocolViolation, "ack carries an unknown refusal reason"};
    }
    message.reason = static_cast<RefusalReason>(reason.value());
    auto detail = reader.str();
    if (!detail) return detail.error();
    message.detail = detail.value();
    auto epoch = reader.u64();
    if (!epoch) return epoch.error();
    message.current_epoch = EpochId{epoch.value()};
    if (!reader.exhausted()) return Error{ErrorCode::MalformedInput, "trailing bytes in DecisionAck"};
    return message;
}

std::vector<std::byte> encode_error(const MessageError& message) {
    codec::Writer writer;
    writer.u16(static_cast<std::uint16_t>(message.code));
    writer.str(message.detail);
    return writer.take();
}

Outcome<MessageError> decode_error(std::span<const std::byte> bytes) {
    codec::Reader reader(bytes);
    MessageError message{};
    auto code = reader.u16();
    if (!code) return code.error();
    message.code = static_cast<ErrorCode>(code.value());
    auto detail = reader.str();
    if (!detail) return detail.error();
    message.detail = detail.value();
    if (!reader.exhausted()) return Error{ErrorCode::MalformedInput, "trailing bytes in Error"};
    return message;
}

std::vector<std::byte> encode_control_request(const ControlRequest& message) {
    codec::Writer writer;
    writer.u32(static_cast<std::uint32_t>(message.opcode));
    writer.str(message.token);
    writer.str(message.argument);
    return writer.take();
}

Outcome<ControlRequest> decode_control_request(std::span<const std::byte> bytes) {
    codec::Reader reader(bytes);
    ControlRequest message{};
    auto opcode = reader.u32();
    if (!opcode) return opcode.error();
    if (opcode.value() == 0 ||
        opcode.value() > static_cast<std::uint32_t>(ControlOpcode::RevokeAll)) {
        return Error{ErrorCode::ProtocolViolation, "control opcode is outside the known set"};
    }
    message.opcode = static_cast<ControlOpcode>(opcode.value());
    auto token = reader.str();
    if (!token) return token.error();
    message.token = token.value();
    auto argument = reader.str(kMaxIdentifierChars * 4);
    if (!argument) return argument.error();
    message.argument = argument.value();
    if (!reader.exhausted()) return Error{ErrorCode::MalformedInput, "trailing bytes in ControlRequest"};
    return message;
}

std::vector<std::byte> encode_control_response(const ControlResponse& message) {
    codec::Writer writer;
    writer.u8(message.ok ? 1u : 0u);
    writer.str(message.detail);
    writer.u64(message.epoch.value());
    writer.u64(message.lease_count);
    writer.u64(message.decisions_accepted);
    writer.u64(message.decisions_rejected);
    return writer.take();
}

Outcome<ControlResponse> decode_control_response(std::span<const std::byte> bytes) {
    codec::Reader reader(bytes);
    ControlResponse message{};
    auto ok = reader.u8();
    if (!ok) return ok.error();
    message.ok = ok.value() != 0;
    auto detail = reader.str(kMaxIdentifierChars * 4);
    if (!detail) return detail.error();
    message.detail = detail.value();
    auto epoch = reader.u64();
    if (!epoch) return epoch.error();
    message.epoch = EpochId{epoch.value()};
    auto leases = reader.u64();
    if (!leases) return leases.error();
    message.lease_count = leases.value();
    auto accepted = reader.u64();
    if (!accepted) return accepted.error();
    message.decisions_accepted = accepted.value();
    auto rejected = reader.u64();
    if (!rejected) return rejected.error();
    message.decisions_rejected = rejected.value();
    if (!reader.exhausted()) return Error{ErrorCode::MalformedInput, "trailing bytes in ControlResponse"};
    return message;
}

// ---------------------------------------------------------------------------
// Epoch ledger
// ---------------------------------------------------------------------------

Status EpochLedger::open(const std::filesystem::path& directory) {
    if (directory.empty()) return fail(ErrorCode::InvalidArgument, "epoch ledger directory is empty");
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return fail(ErrorCode::IoFailure, "unable to create the epoch ledger directory");
    path_ = directory / "coordinator.epoch";
    return ok_status();
}

Outcome<EpochId> EpochLedger::load() {
    if (path_.empty()) return Error{ErrorCode::InvalidArgument, "epoch ledger is not open"};
    if (!std::filesystem::exists(path_)) return EpochId{0};
    std::ifstream stream(path_, std::ios::binary);
    if (!stream) return Error{ErrorCode::IoFailure, "unable to read the epoch ledger"};
    std::array<std::byte, 18> buffer{};
    stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    if (stream.gcount() != static_cast<std::streamsize>(buffer.size())) {
        return Error{ErrorCode::TruncatedInput, "epoch ledger is truncated"};
    }
    codec::Reader reader(std::span<const std::byte>(buffer.data(), buffer.size()));
    auto magic = reader.u32();
    if (!magic || magic.value() != kLeaseLedgerMagic) {
        return Error{ErrorCode::CorruptState, "epoch ledger magic mismatch"};
    }
    auto version = reader.u16();
    if (!version || version.value() != kSnapshotFormatVersion) {
        return Error{ErrorCode::UnsupportedVersion, "unsupported epoch ledger version"};
    }
    auto epoch = reader.u64();
    if (!epoch) return epoch.error();
    auto checksum = reader.u32();
    if (!checksum) return checksum.error();
    const std::array<std::byte, 14> protected_bytes{buffer[0],  buffer[1],  buffer[2],  buffer[3],
                                                    buffer[4],  buffer[5],  buffer[6],  buffer[7],
                                                    buffer[8],  buffer[9],  buffer[10], buffer[11],
                                                    buffer[12], buffer[13]};
    if (crc32c(protected_bytes.data(), protected_bytes.size()) != checksum.value()) {
        return Error{ErrorCode::ChecksumMismatch, "epoch ledger failed its integrity check"};
    }
    return EpochId{epoch.value()};
}

Status EpochLedger::store(EpochId epoch) {
    if (path_.empty()) return ok_status();
    if (!epoch.valid()) return fail(ErrorCode::InvalidArgument, "refusing to persist an absent epoch");
    codec::Writer writer;
    writer.u32(kLeaseLedgerMagic);
    writer.u16(kSnapshotFormatVersion);
    writer.u64(epoch.value());
    const auto body = writer.buffer();
    codec::Writer container;
    container.raw(body.data(), body.size());
    container.u32(crc32c(body.data(), body.size()));
    return write_file_atomic(path_, container.buffer(), true);
}

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------

Coordinator::Coordinator(CoordinatorConfig config) : config_(std::move(config)) {
    epoch_ = config_.initial_epoch.valid() ? config_.initial_epoch : EpochId{1};
}

Coordinator::~Coordinator() { stop(); }

Status Coordinator::start() {
    const Status networking = net::initialize();
    if (!networking) return networking;
    const Status bound = listener_.bind_loopback(config_.port);
    if (!bound) return bound;
    port_ = listener_.bound_port();
    boot_ = mint_boot_incarnation();

    if (!config_.directory.empty()) {
        ledger_ = EpochLedger{};
        const Status opened = ledger_.open(config_.directory);
        if (!opened) return opened;
        ledger_open_ = true;
        auto persisted = ledger_.load();
        if (persisted) {
            if (persisted.value().valid() && persisted.value() >= epoch_) {
                epoch_ = persisted.value().next();
            }
        } else if (persisted.error().code != ErrorCode::NotFound) {
            // A corrupt ledger must not silently restart the epoch sequence.
            return fail(persisted.error().code, persisted.error().detail);
        }
        const Status stored = ledger_.store(epoch_);
        if (!stored) return stored;
    }
    running_ = true;
    return ok_status();
}

void Coordinator::stop() {
    if (stopping_.exchange(true)) return;
    // Wake the accept loop with a throwaway connection. The listener itself is
    // closed by run() after the loop exits, so no thread ever closes a handle
    // that another thread is blocked on.
    if (port_ != 0) {
        net::Connection wake;
        static_cast<void>(wake.connect_loopback(port_));
        wake.close();
    }
    // Unblock any session waiting on a frame. shutdown() is the only socket
    // operation that is safe against a concurrent recv on the same handle.
    std::vector<std::shared_ptr<FramedChannel>> snapshot;
    {
        const std::lock_guard<std::mutex> guard(sessions_mutex_);
        snapshot = sessions_;
    }
    for (auto& session : snapshot) {
        if (session) session->shutdown();
    }
}

Status Coordinator::run() {
    while (!stopping_.load()) {
        auto connection = listener_.accept();
        if (!connection) {
            if (stopping_.load()) break;
            return fail(connection.error().code, connection.error().detail);
        }
        auto channel = std::make_shared<FramedChannel>(std::move(connection.value()));
        {
            const std::lock_guard<std::mutex> guard(sessions_mutex_);
            sessions_.push_back(channel);
        }
        workers_.emplace_back([this, channel]() {
            static_cast<void>(handle_connection(*channel));
            const std::lock_guard<std::mutex> guard(sessions_mutex_);
            sessions_.erase(std::remove(sessions_.begin(), sessions_.end(), channel), sessions_.end());
        });
    }
    listener_.close();
    {
        // Join only after every lock has been released: a worker never needs
        // mutex_ or sessions_mutex_ to finish once its socket is shut down.
        const std::lock_guard<std::mutex> guard(sessions_mutex_);
        for (auto& session : sessions_) {
            if (session) session->shutdown();
        }
    }
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
    {
        const std::lock_guard<std::mutex> guard(sessions_mutex_);
        sessions_.clear();
    }
    running_ = false;
    return ok_status();
}

Status Coordinator::serve_one() {
    auto connection = listener_.accept();
    if (!connection) return fail(connection.error().code, connection.error().detail);
    FramedChannel channel(std::move(connection.value()));
    return handle_connection(channel);
}

EpochId Coordinator::epoch() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return epoch_;
}

Status Coordinator::advance_epoch(std::string reason) {
    const std::lock_guard<std::mutex> guard(mutex_);
    epoch_ = EpochId{epoch_.value() + 1};
    statistics_.epoch_advances += 1;
    if (ledger_open_) {
        const Status stored = ledger_.store(epoch_);
        if (!stored) return stored;
    }
    static_cast<void>(reason);
    return ok_status();
}

Coordinator::Statistics Coordinator::statistics() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return statistics_;
}

std::size_t Coordinator::live_lease_count() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::size_t count = 0;
    for (const auto& [id, record] : leases_) {
        if (!record.revoked && !record.expired) count += 1;
    }
    return count;
}

Outcome<MessageLeaseGrant> Coordinator::grant_lease(const MessageLeaseRequest& request, Timestamp now) {
    if (!request.worker.valid() || !request.boot.valid()) {
        return Error{ErrorCode::InvalidArgument, "lease request carries no worker incarnation"};
    }
    if (!request.destination.valid()) {
        return Error{ErrorCode::InvalidArgument, "lease request carries no destination"};
    }

    const std::lock_guard<std::mutex> guard(mutex_);
    bool superseded = false;
    for (auto& [id, record] : leases_) {
        if (record.grant.destination != request.destination) continue;
        if (record.revoked) continue;
        if (record.grant.boot != request.boot || record.grant.worker != request.worker) {
            // A different live incarnation claims the same destination: the
            // previous holder is fenced by an epoch advance.
            record.revoked = true;
            superseded = true;
        } else {
            record.grant.attempt = request.attempt;
            record.grant.granted_at = now;
            record.grant.expires_at = Timestamp{now.nanos() + config_.lease_duration.nanos()};
            record.grant.epoch = epoch_;
            statistics_.leases_renewed += 1;
            return record.grant;
        }
    }

    if (superseded) {
        epoch_ = EpochId{epoch_.value() + 1};
        statistics_.epoch_advances += 1;
        if (ledger_open_) {
            const Status stored = ledger_.store(epoch_);
            if (!stored) return Error{stored.error().code, stored.error().detail};
        }
    }

    MessageLeaseGrant grant{};
    grant.lease = LeaseId{next_lease_++};
    grant.destination = request.destination;
    grant.destination_generation = request.destination_generation;
    grant.epoch = epoch_;
    grant.boot = request.boot;
    grant.worker = request.worker;
    grant.policy = request.policy;
    grant.policy_generation = request.policy_generation;
    grant.granted_at = now;
    grant.expires_at = Timestamp{now.nanos() + config_.lease_duration.nanos()};
    grant.attempt = request.attempt;

    LeaseRecord record{};
    record.grant = grant;
    leases_.emplace(grant.lease.value(), record);
    statistics_.leases_granted += 1;
    return grant;
}

Status Coordinator::handle_control(FramedChannel& channel, const ControlRequest& request) {
    ControlResponse response{};
    if (config_.control_token.empty() || request.token != config_.control_token) {
        response.ok = false;
        response.detail.assign("control token rejected");
    } else {
        switch (request.opcode) {
            case ControlOpcode::Status: {
                const std::lock_guard<std::mutex> guard(mutex_);
                response.ok = true;
                response.detail.assign("ok");
                response.epoch = epoch_;
                response.lease_count = 0;
                for (const auto& [id, record] : leases_) {
                    if (!record.revoked && !record.expired) response.lease_count += 1;
                }
                response.decisions_accepted = statistics_.decisions_accepted;
                response.decisions_rejected = statistics_.decisions_rejected;
                break;
            }
            case ControlOpcode::AdvanceEpoch: {
                const Status advanced = advance_epoch(request.argument);
                response.ok = static_cast<bool>(advanced);
                response.detail = advanced ? std::string("epoch advanced") : advanced.error().detail;
                response.epoch = epoch();
                break;
            }
            case ControlOpcode::RevokeAll: {
                {
                    const std::lock_guard<std::mutex> guard(mutex_);
                    for (auto& [id, record] : leases_) record.revoked = true;
                    epoch_ = EpochId{epoch_.value() + 1};
                    statistics_.epoch_advances += 1;
                }
                response.ok = true;
                response.detail.assign("all leases revoked; epoch advanced");
                response.epoch = epoch();
                break;
            }
            case ControlOpcode::Shutdown: {
                response.ok = true;
                response.detail.assign("shutdown accepted");
                response.epoch = epoch();
                break;
            }
        }
    }

    Frame reply{};
    reply.type = FrameType::ControlResponse;
    reply.payload = encode_control_response(response);
    const Status sent = channel.send(reply);
    if (!sent) return sent;

    if (request.opcode == ControlOpcode::Shutdown && request.token == config_.control_token) {
        stop();
    }
    return ok_status();
}

Status Coordinator::handle_connection(FramedChannel& channel) {
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        statistics_.connections += 1;
    }
    bool control_session = false;
    while (!stopping_.load()) {
        auto frame = channel.receive();
        if (!frame) {
            const auto code = frame.error().code;
            if (code == ErrorCode::Closed || code == ErrorCode::TruncatedInput) return ok_status();
            {
                const std::lock_guard<std::mutex> guard(mutex_);
                statistics_.frames_in += 1;
            }
            Frame error_frame{};
            error_frame.type = FrameType::Error;
            error_frame.payload = encode_error(MessageError{code, frame.error().detail});
            static_cast<void>(channel.send(error_frame));
            return ok_status();
        }
        {
            const std::lock_guard<std::mutex> guard(mutex_);
            statistics_.frames_in += 1;
        }

        switch (frame.value().type) {
            case FrameType::Hello: {
                auto hello = decode_hello(frame.value().payload);
                if (!hello) return fail(hello.error().code, hello.error().detail);
                MessageHelloAck ack{};
                ack.session = next_session_++;
                ack.epoch = epoch();
                ack.coordinator_boot = boot_;
                Frame reply{};
                reply.type = FrameType::HelloAck;
                reply.payload = encode_hello_ack(ack);
                const Status sent = channel.send(reply);
                if (!sent) return sent;
                const std::lock_guard<std::mutex> guard(mutex_);
                statistics_.frames_out += 1;
                break;
            }
            case FrameType::LeaseRequest: {
                auto request = decode_lease_request(frame.value().payload);
                if (!request) return fail(request.error().code, request.error().detail);
                auto grant = grant_lease(request.value(), coordinator_now());
                Frame reply{};
                if (grant) {
                    reply.type = FrameType::LeaseGrant;
                    reply.payload = encode_lease_grant(grant.value());
                } else {
                    MessageLeaseDenied denied{};
                    denied.reason = grant.error().code == ErrorCode::StaleGeneration
                                        ? RefusalReason::EpochMismatch
                                        : RefusalReason::MissingEvidence;
                    denied.detail = grant.error().detail;
                    denied.current_epoch = epoch();
                    reply.type = FrameType::LeaseDenied;
                    reply.payload = encode_lease_denied(denied);
                    const std::lock_guard<std::mutex> guard(mutex_);
                    statistics_.leases_denied += 1;
                }
                const Status sent = channel.send(reply);
                if (!sent) return sent;
                const std::lock_guard<std::mutex> guard(mutex_);
                statistics_.frames_out += 1;
                break;
            }
            case FrameType::LeaseRenew: {
                auto renew = decode_lease_renew(frame.value().payload);
                if (!renew) return fail(renew.error().code, renew.error().detail);
                MessageLeaseRequest request{};
                request.worker = renew.value().worker;
                request.boot = renew.value().boot;
                request.attempt = renew.value().attempt;

                Frame reply{};
                bool accepted = false;
                {
                    const std::lock_guard<std::mutex> guard(mutex_);
                    const auto iterator = leases_.find(renew.value().lease.value());
                    if (iterator != leases_.end() && !iterator->second.revoked) {
                        const Timestamp now = coordinator_now();
                        if (renew.value().epoch != epoch_) {
                            MessageLeaseDenied denied{};
                            denied.reason = RefusalReason::EpochMismatch;
                            denied.detail.assign("renewal presented a superseded epoch");
                            denied.current_epoch = epoch_;
                            reply.type = FrameType::LeaseDenied;
                            reply.payload = encode_lease_denied(denied);
                        } else if (iterator->second.grant.boot != renew.value().boot) {
                            MessageLeaseDenied denied{};
                            denied.reason = RefusalReason::BootMismatch;
                            denied.detail.assign("renewal presented a previous boot incarnation");
                            denied.current_epoch = epoch_;
                            reply.type = FrameType::LeaseDenied;
                            reply.payload = encode_lease_denied(denied);
                        } else {
                            iterator->second.grant.granted_at = now;
                            iterator->second.grant.expires_at =
                                Timestamp{now.nanos() + config_.lease_duration.nanos()};
                            statistics_.leases_renewed += 1;
                            reply.type = FrameType::LeaseGrant;
                            reply.payload = encode_lease_grant(iterator->second.grant);
                            accepted = true;
                        }
                    } else {
                        MessageLeaseDenied denied{};
                        denied.reason = RefusalReason::LeaseMissing;
                        denied.detail.assign("lease is unknown or revoked");
                        denied.current_epoch = epoch_;
                        reply.type = FrameType::LeaseDenied;
                        reply.payload = encode_lease_denied(denied);
                    }
                }
                if (!accepted) {
                    const std::lock_guard<std::mutex> guard(mutex_);
                    statistics_.leases_denied += 1;
                }
                const Status sent = channel.send(reply);
                if (!sent) return sent;
                break;
            }
            case FrameType::DecisionPublish: {
                auto publish = decode_decision_publish(frame.value().payload);
                if (!publish) return fail(publish.error().code, publish.error().detail);
                MessageDecisionAck ack{};
                {
                    const std::lock_guard<std::mutex> guard(mutex_);
                    ack.current_epoch = epoch_;
                    const auto iterator = leases_.find(publish.value().lease.value());
                    const Timestamp now = coordinator_now();
                    if (iterator == leases_.end()) {
                        ack.accepted = false;
                        ack.reason = RefusalReason::LeaseMissing;
                        ack.detail.assign("decision references an unknown lease");
                    } else if (iterator->second.revoked) {
                        ack.accepted = false;
                        ack.reason = RefusalReason::LeaseMissing;
                        ack.detail.assign("decision references a revoked lease");
                    } else if (publish.value().epoch != epoch_) {
                        ack.accepted = false;
                        ack.reason = RefusalReason::EpochMismatch;
                        ack.detail.assign("decision was produced under a superseded epoch");
                        iterator->second.revoked = true;
                    } else if (iterator->second.grant.boot != publish.value().boot) {
                        ack.accepted = false;
                        ack.reason = RefusalReason::BootMismatch;
                        ack.detail.assign("decision was produced by a previous boot incarnation");
                    } else if (iterator->second.grant.worker != publish.value().worker) {
                        ack.accepted = false;
                        ack.reason = RefusalReason::EpochMismatch;
                        ack.detail.assign("decision was produced by a different worker");
                    } else if (now > iterator->second.grant.expires_at) {
                        ack.accepted = false;
                        ack.reason = RefusalReason::LeaseExpired;
                        ack.detail.assign("lease expired before the decision was published");
                    } else if (publish.value().sequence <= iterator->second.lease_sequence) {
                        ack.accepted = false;
                        ack.reason = RefusalReason::ReorderedEvidence;
                        ack.detail.assign("decision sequence is not newer than the last accepted one");
                    } else {
                        iterator->second.lease_sequence = publish.value().sequence;
                        iterator->second.last_decision_at = publish.value().decided_at;
                        iterator->second.last_kind = publish.value().kind;
                        ack.accepted = true;
                        ack.reason = RefusalReason::None;
                        ack.detail.assign("accepted");
                    }
                    if (ack.accepted) {
                        statistics_.decisions_accepted += 1;
                    } else {
                        statistics_.decisions_rejected += 1;
                    }
                }
                Frame reply{};
                reply.type = FrameType::DecisionAck;
                reply.payload = encode_decision_ack(ack);
                const Status sent = channel.send(reply);
                if (!sent) return sent;
                const std::lock_guard<std::mutex> guard(mutex_);
                statistics_.frames_out += 1;
                break;
            }
            case FrameType::Heartbeat: {
                MessageEpochAdvance advance{};
                advance.epoch = epoch();
                advance.reason.assign("heartbeat");
                Frame reply{};
                reply.type = FrameType::HeartbeatAck;
                reply.payload = encode_epoch_advance(advance);
                const Status sent = channel.send(reply);
                if (!sent) return sent;
                break;
            }
            case FrameType::ControlRequest: {
                auto request = decode_control_request(frame.value().payload);
                if (!request) return fail(request.error().code, request.error().detail);
                control_session = true;
                const Status handled = handle_control(channel, request.value());
                if (!handled) return handled;
                break;
            }
            case FrameType::Shutdown: {
                stop();
                return ok_status();
            }
            default: {
                Frame error_frame{};
                error_frame.type = FrameType::Error;
                error_frame.payload =
                    encode_error(MessageError{ErrorCode::ProtocolViolation, "unsupported frame type"});
                const Status sent = channel.send(error_frame);
                if (!sent) return sent;
                break;
            }
        }
        if (control_session) return ok_status();
    }
    return ok_status();
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

GovernorWorker::GovernorWorker(WorkerConfig config) : config_(std::move(config)) {
    if (!config_.boot.valid()) config_.boot = mint_boot_incarnation();
    if (!config_.worker.valid()) config_.worker = mint_worker_id(config_.name);
}

GovernorWorker::~GovernorWorker() { close(); }

Status GovernorWorker::connect_and_handshake() {
    const Status networking = net::initialize();
    if (!networking) return networking;
    net::Connection connection;
    const Status connected = connection.connect_loopback(config_.coordinator_port);
    if (!connected) return connected;
    channel_ = FramedChannel(std::move(connection));

    MessageHello hello{};
    hello.worker = config_.worker;
    hello.boot = config_.boot;
    hello.name = config_.name;
    Frame request{};
    request.type = FrameType::Hello;
    request.payload = encode_hello(hello);
    const Status sent = channel_.send(request);
    if (!sent) return sent;

    auto reply = channel_.receive();
    if (!reply) return fail(reply.error().code, reply.error().detail);
    if (reply.value().type != FrameType::HelloAck) {
        return fail(ErrorCode::ProtocolViolation, "coordinator did not acknowledge the handshake");
    }
    auto ack = decode_hello_ack(reply.value().payload);
    if (!ack) return fail(ack.error().code, ack.error().detail);
    epoch_ = ack.value().epoch;
    handshaken_ = true;
    fenced_ = false;
    return ok_status();
}

Outcome<MessageLeaseGrant> GovernorWorker::acquire_lease() {
    if (!handshaken_) return Error{ErrorCode::NotAuthorized, "worker has not completed the handshake"};
    MessageLeaseRequest request{};
    request.worker = config_.worker;
    request.boot = config_.boot;
    request.destination = config_.destination;
    request.destination_generation = config_.destination_generation;
    request.policy = config_.policy;
    request.policy_generation = config_.policy_generation;
    request.attempt = 1;

    Frame frame{};
    frame.type = FrameType::LeaseRequest;
    frame.payload = encode_lease_request(request);
    const Status sent = channel_.send(frame);
    if (!sent) return Error{sent.error().code, sent.error().detail};

    auto reply = channel_.receive();
    if (!reply) return reply.error();
    if (reply.value().type == FrameType::LeaseDenied) {
        auto denied = decode_lease_denied(reply.value().payload);
        if (!denied) return denied.error();
        fenced_ = true;
        epoch_ = denied.value().current_epoch;
        return Error{ErrorCode::FencedEpoch, denied.value().detail};
    }
    if (reply.value().type != FrameType::LeaseGrant) {
        return Error{ErrorCode::ProtocolViolation, "unexpected reply to a lease request"};
    }
    auto grant = decode_lease_grant(reply.value().payload);
    if (!grant) return grant.error();
    lease_ = grant.value();
    epoch_ = grant.value().epoch;
    fenced_ = false;
    return grant;
}

Outcome<MessageLeaseGrant> GovernorWorker::renew_lease() {
    if (!handshaken_) return Error{ErrorCode::NotAuthorized, "worker has not completed the handshake"};
    if (!lease_.lease.valid()) return Error{ErrorCode::NotAuthorized, "worker holds no lease"};
    MessageLeaseRenew renew{};
    renew.lease = lease_.lease;
    renew.epoch = epoch_;
    renew.boot = config_.boot;
    renew.worker = config_.worker;
    renew.attempt = lease_.attempt;

    Frame frame{};
    frame.type = FrameType::LeaseRenew;
    frame.payload = encode_lease_renew(renew);
    const Status sent = channel_.send(frame);
    if (!sent) return Error{sent.error().code, sent.error().detail};

    auto reply = channel_.receive();
    if (!reply) return reply.error();
    if (reply.value().type == FrameType::LeaseDenied) {
        auto denied = decode_lease_denied(reply.value().payload);
        if (!denied) return denied.error();
        fenced_ = true;
        epoch_ = denied.value().current_epoch;
        return Error{ErrorCode::FencedEpoch, denied.value().detail};
    }
    if (reply.value().type != FrameType::LeaseGrant) {
        return Error{ErrorCode::ProtocolViolation, "unexpected reply to a lease renewal"};
    }
    auto grant = decode_lease_grant(reply.value().payload);
    if (!grant) return grant.error();
    lease_ = grant.value();
    epoch_ = grant.value().epoch;
    return grant;
}

Outcome<MessageDecisionAck> GovernorWorker::publish(const Decision& decision) {
    if (!handshaken_) return Error{ErrorCode::NotAuthorized, "worker has not completed the handshake"};
    MessageDecisionPublish publish{};
    publish.lease = lease_.lease;
    publish.epoch = epoch_;
    publish.boot = config_.boot;
    publish.worker = config_.worker;
    publish.destination = config_.destination;
    publish.window = decision.window;
    publish.window_generation = decision.window_generation;
    publish.kind = decision.kind;
    publish.severity = decision.severity;
    publish.outcome = decision.outcome;
    publish.mitigation = decision.intent.kind;
    publish.intervention = decision.intent.intervention;
    publish.reduction_bp = decision.intent.reduction_bp;
    publish.state_generation = decision.state_generation;
    publish.decided_at = decision.decided_at;
    publish.sequence = ++sequence_;

    Frame frame{};
    frame.type = FrameType::DecisionPublish;
    frame.payload = encode_decision_publish(publish);
    const Status sent = channel_.send(frame);
    if (!sent) return Error{sent.error().code, sent.error().detail};

    auto reply = channel_.receive();
    if (!reply) return reply.error();
    if (reply.value().type == FrameType::EpochAdvance) {
        auto advance = decode_epoch_advance(reply.value().payload);
        if (!advance) return advance.error();
        epoch_ = advance.value().epoch;
        fenced_ = true;
        MessageDecisionAck ack{};
        ack.accepted = false;
        ack.reason = RefusalReason::EpochMismatch;
        ack.detail = advance.value().reason;
        ack.current_epoch = epoch_;
        return ack;
    }
    if (reply.value().type != FrameType::DecisionAck) {
        return Error{ErrorCode::ProtocolViolation, "unexpected reply to a decision publish"};
    }
    auto ack = decode_decision_ack(reply.value().payload);
    if (!ack) return ack.error();
    if (!ack.value().accepted &&
        (ack.value().reason == RefusalReason::EpochMismatch ||
         ack.value().reason == RefusalReason::BootMismatch)) {
        fenced_ = true;
        epoch_ = ack.value().current_epoch;
    }
    return ack;
}

Status GovernorWorker::send_heartbeat() {
    if (!handshaken_) return fail(ErrorCode::NotAuthorized, "worker has not completed the handshake");
    Frame frame{};
    frame.type = FrameType::Heartbeat;
    const Status sent = channel_.send(frame);
    if (!sent) return sent;
    auto reply = channel_.receive();
    if (!reply) return fail(reply.error().code, reply.error().detail);
    if (reply.value().type != FrameType::HeartbeatAck) {
        return fail(ErrorCode::ProtocolViolation, "unexpected reply to a heartbeat");
    }
    auto advance = decode_epoch_advance(reply.value().payload);
    if (!advance) return fail(advance.error().code, advance.error().detail);
    if (advance.value().epoch != epoch_) {
        epoch_ = advance.value().epoch;
        fenced_ = true;
    }
    return ok_status();
}

void GovernorWorker::close() {
    channel_.close();
    handshaken_ = false;
}

Outcome<ControlResponse> send_control(std::uint16_t port, const ControlRequest& request) {
    const Status networking = net::initialize();
    if (!networking) return Error{networking.error().code, networking.error().detail};
    net::Connection connection;
    const Status connected = connection.connect_loopback(port);
    if (!connected) return Error{connected.error().code, connected.error().detail};
    FramedChannel channel(std::move(connection));
    Frame frame{};
    frame.type = FrameType::ControlRequest;
    frame.payload = encode_control_request(request);
    const Status sent = channel.send(frame);
    if (!sent) return Error{sent.error().code, sent.error().detail};
    auto reply = channel.receive();
    if (!reply) return reply.error();
    if (reply.value().type != FrameType::ControlResponse) {
        return Error{ErrorCode::ProtocolViolation, "unexpected reply to a control request"};
    }
    return decode_control_response(reply.value().payload);
}

BootIncarnation mint_boot_incarnation() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto nanos = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    const auto ticks = static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    DeterministicRng rng(nanos ^ (ticks * 0x9E3779B97F4A7C15ULL));
    std::uint64_t value = rng.next();
    if (value == 0) value = 1;
    return BootIncarnation{value};
}

WorkerId mint_worker_id(std::string_view name) {
    std::size_t seed = 0xCBF29CE484222325ULL;
    for (const char character : name) {
        seed ^= static_cast<std::size_t>(static_cast<unsigned char>(character));
        seed *= 0x100000001B3ULL;
    }
    std::uint64_t value = static_cast<std::uint64_t>(seed);
    if (value == 0) value = 1;
    return WorkerId{value};
}

}  // namespace incast
