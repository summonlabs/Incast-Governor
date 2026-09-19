// Incast Governor - state serialisation implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "incast/state.hpp"

#include <cstring>

namespace incast {
namespace {

[[nodiscard]] bool is_valid_service_class(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(ServiceClass::Replication);
}

[[nodiscard]] bool is_valid_mitigation_kind(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(MitigationKind::CongestionEscalation);
}

[[nodiscard]] bool is_valid_scope(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(MitigationScope::Resource);
}

[[nodiscard]] bool is_valid_intervention_state(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(InterventionState::Expired);
}

[[nodiscard]] bool is_valid_refusal(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(RefusalReason::CapacityNotRevalidated);
}

void encode_mutation_flags(codec::Writer& writer, const MitigationIntent& intent) {
    std::uint16_t flags = 0;
    if (intent.bounded) flags |= 1u << 0;
    if (intent.clamped) flags |= 1u << 1;
    if (intent.insufficient) flags |= 1u << 2;
    if (intent.escalating) flags |= 1u << 3;
    if (intent.relaxes_prior) flags |= 1u << 4;
    if (intent.continues_prior) flags |= 1u << 5;
    writer.u16(flags);
}

void decode_mutation_flags(std::uint16_t flags, MitigationIntent& intent) {
    intent.bounded = (flags & (1u << 0)) != 0;
    intent.clamped = (flags & (1u << 1)) != 0;
    intent.insufficient = (flags & (1u << 2)) != 0;
    intent.escalating = (flags & (1u << 3)) != 0;
    intent.relaxes_prior = (flags & (1u << 4)) != 0;
    intent.continues_prior = (flags & (1u << 5)) != 0;
}

}  // namespace

std::vector<std::byte> encode_intent(const MitigationIntent& intent) {
    codec::Writer writer;
    writer.reserve(512);
    writer.u8(static_cast<std::uint8_t>(intent.kind));
    writer.u8(static_cast<std::uint8_t>(intent.scope));
    writer.u64(intent.intervention.value());
    writer.u64(intent.generation.value());
    writer.u64(intent.event.value());
    writer.u64(intent.event_generation.value());
    writer.u64(intent.destination.value());
    writer.u64(intent.destination_generation.value());
    writer.u64(intent.window.value());
    writer.u64(intent.window_generation.value());
    writer.u64(intent.policy.value());
    writer.u64(intent.policy_generation.value());
    writer.u64(intent.epoch.value());
    writer.u64(intent.boot.value());
    writer.u64(intent.worker.value());
    writer.u64(intent.lease.value());
    writer.i64(intent.issued_at.nanos());
    writer.i64(intent.expires_at.nanos());
    writer.i64(intent.duration.nanos());
    writer.u32(intent.reduction_bp.value());
    writer.u32(intent.admission_reduction_bp.value());
    writer.u32(intent.aggregate_reduction_bp.value());
    writer.u64(intent.headroom_bytes.count());
    writer.u32(intent.stagger_slots);
    writer.u32(static_cast<std::uint32_t>(intent.penalized_senders.size()));
    for (const auto& identity : intent.penalized_senders) {
        writer.u64(identity.sender.value());
        writer.u64(identity.incarnation.value());
        writer.u64(identity.flow.value());
    }
    writer.u32(static_cast<std::uint32_t>(intent.affected_classes.size()));
    for (const auto service : intent.affected_classes) {
        writer.u8(static_cast<std::uint8_t>(service));
    }
    writer.u64(intent.queue.value());
    writer.u64(intent.resource.value());
    writer.u64(intent.protected_offered.bytes_per_second());
    writer.u64(intent.protected_floor.bytes_per_second());
    writer.u64(intent.resulting_offered.bytes_per_second());
    writer.u64(intent.required_reduction.bytes_per_second());
    writer.u64(intent.achievable_reduction.bytes_per_second());
    encode_mutation_flags(writer, intent);
    return writer.take();
}

Outcome<MitigationIntent> decode_intent(std::span<const std::byte> bytes) {
    codec::Reader reader(bytes);
    MitigationIntent intent{};

    auto kind = reader.u8();
    if (!kind) return kind.error();
    if (!is_valid_mitigation_kind(kind.value())) {
        return Error{ErrorCode::CorruptState, "intent carries an unknown mitigation kind"};
    }
    intent.kind = static_cast<MitigationKind>(kind.value());

    auto scope = reader.u8();
    if (!scope) return scope.error();
    if (!is_valid_scope(scope.value())) {
        return Error{ErrorCode::CorruptState, "intent carries an unknown mitigation scope"};
    }
    intent.scope = static_cast<MitigationScope>(scope.value());

    const auto read_u64 = [&reader](std::uint64_t& destination) -> Status {
        auto value = reader.u64();
        if (!value) return fail(value.error().code, value.error().detail);
        destination = value.value();
        return ok_status();
    };
    const auto read_i64 = [&reader](std::int64_t& destination) -> Status {
        auto value = reader.i64();
        if (!value) return fail(value.error().code, value.error().detail);
        destination = value.value();
        return ok_status();
    };
    const auto read_u32 = [&reader](std::uint32_t& destination) -> Status {
        auto value = reader.u32();
        if (!value) return fail(value.error().code, value.error().detail);
        destination = value.value();
        return ok_status();
    };

    std::uint64_t raw = 0;
    Status status = ok_status();
    status = read_u64(raw); if (!status) return status.error();
    intent.intervention = InterventionId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.generation = InterventionGeneration{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.event = EventId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.event_generation = EventGeneration{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.destination = DestinationId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.destination_generation = DestinationGeneration{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.window = WindowId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.window_generation = WindowGeneration{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.policy = PolicyId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.policy_generation = PolicyGeneration{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.epoch = EpochId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.boot = BootIncarnation{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.worker = WorkerId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.lease = LeaseId{raw};
    if (!status) return status.error();

    std::int64_t signed_value = 0;
    status = read_i64(signed_value); if (!status) return status.error();
    intent.issued_at = Timestamp{signed_value};
    status = read_i64(signed_value); if (!status) return status.error();
    intent.expires_at = Timestamp{signed_value};
    status = read_i64(signed_value); if (!status) return status.error();
    intent.duration = Duration{signed_value};

    std::uint32_t small = 0;
    status = read_u32(small); if (!status) return status.error();
    intent.reduction_bp = BasisPoints{small};
    status = read_u32(small); if (!status) return status.error();
    intent.admission_reduction_bp = BasisPoints{small};
    status = read_u32(small); if (!status) return status.error();
    intent.aggregate_reduction_bp = BasisPoints{small};

    status = read_u64(raw); if (!status) return status.error();
    intent.headroom_bytes = Bytes{raw};
    status = read_u32(small); if (!status) return status.error();
    intent.stagger_slots = small;

    status = read_u32(small); if (!status) return status.error();
    if (small > kMaxPenalizedSenders) {
        return Error{ErrorCode::OversizedInput, "penalized sender list exceeds the supported bound"};
    }
    intent.penalized_senders.reserve(small);
    for (std::uint32_t index = 0; index < small; ++index) {
        SenderIdentity identity{};
        status = read_u64(raw); if (!status) return status.error();
        identity.sender = SenderId{raw};
        status = read_u64(raw); if (!status) return status.error();
        identity.incarnation = BootIncarnation{raw};
        status = read_u64(raw); if (!status) return status.error();
        identity.flow = FlowId{raw};
        intent.penalized_senders.push_back(identity);
    }

    status = read_u32(small); if (!status) return status.error();
    if (small > kServiceClassCount) {
        return Error{ErrorCode::OversizedInput, "affected class list exceeds the service class domain"};
    }
    for (std::uint32_t index = 0; index < small; ++index) {
        auto service = reader.u8();
        if (!service) return service.error();
        if (!is_valid_service_class(service.value())) {
            return Error{ErrorCode::CorruptState, "intent carries an unknown service class"};
        }
        intent.affected_classes.push_back(static_cast<ServiceClass>(service.value()));
    }

    status = read_u64(raw); if (!status) return status.error();
    intent.queue = QueueId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.resource = ResourceId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.protected_offered = Rate{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.protected_floor = Rate{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.resulting_offered = Rate{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.required_reduction = Rate{raw};
    status = read_u64(raw); if (!status) return status.error();
    intent.achievable_reduction = Rate{raw};

    auto flags = reader.u16();
    if (!flags) return flags.error();
    decode_mutation_flags(flags.value(), intent);

    if (!reader.exhausted()) {
        return Error{ErrorCode::MalformedInput, "trailing bytes after the encoded intent"};
    }
    return intent;
}

std::vector<std::byte> encode_intervention(const Intervention& intervention) {
    codec::Writer writer;
    writer.reserve(640);
    writer.u64(intervention.id.value());
    writer.u64(intervention.generation.value());
    writer.u64(intervention.destination.value());
    writer.u64(intervention.destination_generation.value());
    writer.u64(intervention.window.value());
    writer.u64(intervention.window_generation.value());
    writer.u8(static_cast<std::uint8_t>(intervention.state));
    writer.u64(intervention.epoch.value());
    writer.u64(intervention.boot.value());
    writer.u64(intervention.worker.value());
    writer.u64(intervention.lease.value());
    writer.u64(intervention.policy.value());
    writer.u64(intervention.policy_generation.value());
    writer.i64(intervention.created_at.nanos());
    writer.i64(intervention.updated_at.nanos());
    writer.i64(intervention.expires_at.nanos());
    writer.i64(intervention.last_relaxed_at.nanos());
    writer.u32(intervention.applied_reduction_bp.value());
    writer.u32(intervention.relax_steps);
    writer.u32(intervention.continue_steps);
    writer.u8(static_cast<std::uint8_t>(intervention.refusal));
    writer.str(intervention.refusal_detail);

    codec::Writer intent_writer;
    const auto intent_bytes = encode_intent(intervention.intent);
    writer.u32(static_cast<std::uint32_t>(intent_bytes.size()));
    writer.raw(intent_bytes.data(), intent_bytes.size());
    return writer.take();
}

Outcome<Intervention> decode_intervention(std::span<const std::byte> bytes) {
    codec::Reader reader(bytes);
    Intervention intervention{};

    const auto read_u64 = [&reader](std::uint64_t& destination) -> Status {
        auto value = reader.u64();
        if (!value) return fail(value.error().code, value.error().detail);
        destination = value.value();
        return ok_status();
    };
    const auto read_i64 = [&reader](std::int64_t& destination) -> Status {
        auto value = reader.i64();
        if (!value) return fail(value.error().code, value.error().detail);
        destination = value.value();
        return ok_status();
    };
    const auto read_u32 = [&reader](std::uint32_t& destination) -> Status {
        auto value = reader.u32();
        if (!value) return fail(value.error().code, value.error().detail);
        destination = value.value();
        return ok_status();
    };

    std::uint64_t raw = 0;
    Status status = ok_status();
    status = read_u64(raw); if (!status) return status.error();
    intervention.id = InterventionId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intervention.generation = InterventionGeneration{raw};
    status = read_u64(raw); if (!status) return status.error();
    intervention.destination = DestinationId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intervention.destination_generation = DestinationGeneration{raw};
    status = read_u64(raw); if (!status) return status.error();
    intervention.window = WindowId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intervention.window_generation = WindowGeneration{raw};

    auto state = reader.u8();
    if (!state) return state.error();
    if (!is_valid_intervention_state(state.value())) {
        return Error{ErrorCode::CorruptState, "intervention carries an unknown lifecycle state"};
    }
    intervention.state = static_cast<InterventionState>(state.value());

    status = read_u64(raw); if (!status) return status.error();
    intervention.epoch = EpochId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intervention.boot = BootIncarnation{raw};
    status = read_u64(raw); if (!status) return status.error();
    intervention.worker = WorkerId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intervention.lease = LeaseId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intervention.policy = PolicyId{raw};
    status = read_u64(raw); if (!status) return status.error();
    intervention.policy_generation = PolicyGeneration{raw};

    std::int64_t signed_value = 0;
    status = read_i64(signed_value); if (!status) return status.error();
    intervention.created_at = Timestamp{signed_value};
    status = read_i64(signed_value); if (!status) return status.error();
    intervention.updated_at = Timestamp{signed_value};
    status = read_i64(signed_value); if (!status) return status.error();
    intervention.expires_at = Timestamp{signed_value};
    status = read_i64(signed_value); if (!status) return status.error();
    intervention.last_relaxed_at = Timestamp{signed_value};

    std::uint32_t small = 0;
    status = read_u32(small); if (!status) return status.error();
    intervention.applied_reduction_bp = BasisPoints{small};
    status = read_u32(small); if (!status) return status.error();
    intervention.relax_steps = small;
    status = read_u32(small); if (!status) return status.error();
    intervention.continue_steps = small;

    auto refusal = reader.u8();
    if (!refusal) return refusal.error();
    if (!is_valid_refusal(refusal.value())) {
        return Error{ErrorCode::CorruptState, "intervention carries an unknown refusal reason"};
    }
    intervention.refusal = static_cast<RefusalReason>(refusal.value());

    auto detail = reader.str();
    if (!detail) return detail.error();
    intervention.refusal_detail = detail.value();

    auto intent_length = reader.u32();
    if (!intent_length) return intent_length.error();
    if (intent_length.value() > kMaxJournalRecordBytes) {
        return Error{ErrorCode::OversizedInput, "encoded intent exceeds the record bound"};
    }
    if (reader.remaining() < intent_length.value()) {
        return Error{ErrorCode::TruncatedInput, "encoded intent is truncated"};
    }
    const std::span<const std::byte> intent_bytes(bytes.data() + reader.position(), intent_length.value());
    auto intent = decode_intent(intent_bytes);
    if (!intent) return intent.error();
    intervention.intent = intent.value();

    std::vector<std::byte> consumed(intent_length.value());
    const Status advanced = reader.raw(consumed.data(), intent_length.value());
    if (!advanced) return advanced.error();
    if (reader.remaining() != 0) {
        return Error{ErrorCode::MalformedInput, "trailing bytes after the encoded intervention"};
    }
    return intervention;
}

std::vector<std::byte> encode_state(const GovernorState& state) {
    codec::Writer writer;
    writer.reserve(4096);

    writer.u64(state.generation.value());
    writer.u64(state.destination.value());
    writer.u64(state.destination_generation.value());
    writer.u64(state.boot.value());
    writer.u64(state.epoch.value());
    writer.u64(state.policy.value());
    writer.u64(state.policy_generation.value());
    writer.i64(state.last_evaluation.nanos());
    writer.u64(state.evaluation_count);
    writer.u64(state.refusal_count);
    writer.u64(state.intervention_count);
    writer.u32(state.incident_streak);
    writer.u32(state.clean_streak);
    writer.u64(state.next_intervention_generation.value());
    writer.u8(state.requires_revalidation ? 1u : 0u);
    writer.u8(state.restored_from_durable ? 1u : 0u);
    writer.u32(state.restore_count);

    writer.u8(state.recovery.tracking ? 1u : 0u);
    writer.i64(state.recovery.since.nanos());
    writer.u32(state.recovery.clean_windows);
    writer.u32(state.recovery.incident_windows);
    writer.u64(state.recovery.drops_during_recovery);
    writer.u8(state.recovery.capacity_revalidated ? 1u : 0u);

    writer.u64(state.last_sequence_boot.value());
    for (const auto sequence : state.last_sequence) writer.u64(sequence);

    writer.u32(static_cast<std::uint32_t>(state.history.size()));
    for (const auto& record : state.history) {
        writer.u64(record.window.value());
        writer.u64(record.window_generation.value());
        writer.i64(record.start.nanos());
        writer.i64(record.end.nanos());
        writer.u8(static_cast<std::uint8_t>(record.kind));
        writer.u8(static_cast<std::uint8_t>(record.severity));
        writer.u32(record.oversubscription_bp.value());
        writer.u32(record.queue_pressure_bp.value());
        writer.u32(record.active_senders);
        writer.u8(record.authoritative ? 1u : 0u);
    }

    writer.u32(static_cast<std::uint32_t>(state.interventions.size()));
    for (const auto& intervention : state.interventions) {
        const auto bytes = encode_intervention(intervention);
        writer.u32(static_cast<std::uint32_t>(bytes.size()));
        writer.raw(bytes.data(), bytes.size());
    }

    codec::Writer container;
    const auto payload = writer.take();
    container.u32(kStateMagic);
    container.u16(kSnapshotFormatVersion);
    container.u64(state.generation.value());
    container.u32(static_cast<std::uint32_t>(payload.size()));
    container.u32(crc32c(payload.data(), payload.size()));
    container.raw(payload.data(), payload.size());
    return container.take();
}

Outcome<GovernorState> decode_state(std::span<const std::byte> bytes) {
    codec::Reader header(bytes);
    auto magic = header.u32();
    if (!magic) return magic.error();
    if (magic.value() != kStateMagic) {
        return Error{ErrorCode::CorruptState, "state container magic mismatch"};
    }
    auto version = header.u16();
    if (!version) return version.error();
    if (version.value() != kSnapshotFormatVersion) {
        return Error{ErrorCode::UnsupportedVersion, "unsupported state container version"};
    }
    auto generation = header.u64();
    if (!generation) return generation.error();
    auto length = header.u32();
    if (!length) return length.error();
    if (length.value() > kMaxJournalRecordBytes * 64) {
        return Error{ErrorCode::OversizedInput, "state payload exceeds the supported bound"};
    }
    auto checksum = header.u32();
    if (!checksum) return checksum.error();
    if (header.remaining() < length.value()) {
        return Error{ErrorCode::TruncatedInput, "state payload is truncated"};
    }
    const auto payload = bytes.subspan(header.position(), length.value());
    if (crc32c(payload.data(), payload.size()) != checksum.value()) {
        return Error{ErrorCode::ChecksumMismatch, "state payload failed its integrity check"};
    }

    codec::Reader reader(payload);
    GovernorState state{};

    const auto read_u64 = [&reader](std::uint64_t& destination) -> Status {
        auto value = reader.u64();
        if (!value) return fail(value.error().code, value.error().detail);
        destination = value.value();
        return ok_status();
    };
    const auto read_i64 = [&reader](std::int64_t& destination) -> Status {
        auto value = reader.i64();
        if (!value) return fail(value.error().code, value.error().detail);
        destination = value.value();
        return ok_status();
    };
    const auto read_u32 = [&reader](std::uint32_t& destination) -> Status {
        auto value = reader.u32();
        if (!value) return fail(value.error().code, value.error().detail);
        destination = value.value();
        return ok_status();
    };
    const auto read_u8 = [&reader](std::uint8_t& destination) -> Status {
        auto value = reader.u8();
        if (!value) return fail(value.error().code, value.error().detail);
        destination = value.value();
        return ok_status();
    };

    std::uint64_t raw = 0;
    Status status = ok_status();
    status = read_u64(raw); if (!status) return status.error();
    state.generation = StateGeneration{raw};
    status = read_u64(raw); if (!status) return status.error();
    state.destination = DestinationId{raw};
    status = read_u64(raw); if (!status) return status.error();
    state.destination_generation = DestinationGeneration{raw};
    status = read_u64(raw); if (!status) return status.error();
    state.boot = BootIncarnation{raw};
    status = read_u64(raw); if (!status) return status.error();
    state.epoch = EpochId{raw};
    status = read_u64(raw); if (!status) return status.error();
    state.policy = PolicyId{raw};
    status = read_u64(raw); if (!status) return status.error();
    state.policy_generation = PolicyGeneration{raw};

    std::int64_t signed_value = 0;
    status = read_i64(signed_value); if (!status) return status.error();
    state.last_evaluation = Timestamp{signed_value};
    status = read_u64(state.evaluation_count); if (!status) return status.error();
    status = read_u64(state.refusal_count); if (!status) return status.error();
    status = read_u64(state.intervention_count); if (!status) return status.error();
    status = read_u32(state.incident_streak); if (!status) return status.error();
    status = read_u32(state.clean_streak); if (!status) return status.error();

    status = read_u64(raw); if (!status) return status.error();
    if (raw == 0) return Error{ErrorCode::CorruptState, "next intervention generation is absent"};
    state.next_intervention_generation = InterventionGeneration{raw};

    std::uint8_t flag = 0;
    status = read_u8(flag); if (!status) return status.error();
    state.requires_revalidation = flag != 0;
    status = read_u8(flag); if (!status) return status.error();
    state.restored_from_durable = flag != 0;
    status = read_u32(state.restore_count); if (!status) return status.error();

    status = read_u8(flag); if (!status) return status.error();
    state.recovery.tracking = flag != 0;
    status = read_i64(signed_value); if (!status) return status.error();
    state.recovery.since = Timestamp{signed_value};
    status = read_u32(state.recovery.clean_windows); if (!status) return status.error();
    status = read_u32(state.recovery.incident_windows); if (!status) return status.error();
    status = read_u64(state.recovery.drops_during_recovery); if (!status) return status.error();
    status = read_u8(flag); if (!status) return status.error();
    state.recovery.capacity_revalidated = flag != 0;

    status = read_u64(raw); if (!status) return status.error();
    state.last_sequence_boot = BootIncarnation{raw};
    for (std::size_t index = 0; index < kEvidenceClassCount; ++index) {
        status = read_u64(state.last_sequence[index]); if (!status) return status.error();
    }

    std::uint32_t count = 0;
    status = read_u32(count); if (!status) return status.error();
    if (count > kMaxWindowHistory) {
        return Error{ErrorCode::OversizedInput, "state history exceeds the supported bound"};
    }
    state.history.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        WindowRecord record{};
        status = read_u64(raw); if (!status) return status.error();
        record.window = WindowId{raw};
        status = read_u64(raw); if (!status) return status.error();
        record.window_generation = WindowGeneration{raw};
        status = read_i64(signed_value); if (!status) return status.error();
        record.start = Timestamp{signed_value};
        status = read_i64(signed_value); if (!status) return status.error();
        record.end = Timestamp{signed_value};
        status = read_u8(flag); if (!status) return status.error();
        if (flag > static_cast<std::uint8_t>(IncidentKind::Recovery)) {
            return Error{ErrorCode::CorruptState, "history carries an unknown incident kind"};
        }
        record.kind = static_cast<IncidentKind>(flag);
        status = read_u8(flag); if (!status) return status.error();
        if (flag > static_cast<std::uint8_t>(Severity::Critical)) {
            return Error{ErrorCode::CorruptState, "history carries an unknown severity"};
        }
        record.severity = static_cast<Severity>(flag);
        std::uint32_t small = 0;
        status = read_u32(small); if (!status) return status.error();
        record.oversubscription_bp = BasisPoints{small};
        status = read_u32(small); if (!status) return status.error();
        record.queue_pressure_bp = BasisPoints{small};
        status = read_u32(record.active_senders); if (!status) return status.error();
        status = read_u8(flag); if (!status) return status.error();
        record.authoritative = flag != 0;
        state.history.push_back(record);
    }

    status = read_u32(count); if (!status) return status.error();
    if (count > kMaxInterventions) {
        return Error{ErrorCode::OversizedInput, "state intervention table exceeds the supported bound"};
    }
    state.interventions.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t entry_length = 0;
        status = read_u32(entry_length); if (!status) return status.error();
        if (entry_length > kMaxJournalRecordBytes) {
            return Error{ErrorCode::OversizedInput, "encoded intervention exceeds the record bound"};
        }
        if (reader.remaining() < entry_length) {
            return Error{ErrorCode::TruncatedInput, "encoded intervention is truncated"};
        }
        const std::span<const std::byte> slice(payload.data() + reader.position(), entry_length);
        auto intervention = decode_intervention(slice);
        if (!intervention) return intervention.error();
        state.interventions.push_back(intervention.value());
        // Advance the reader past the consumed intervention bytes.
        std::vector<std::byte> skip(entry_length);
        status = reader.raw(skip.data(), entry_length);
        if (!status) return status.error();
    }

    const Status validation = state.validate();
    if (!validation) return validation.error();
    if (state.generation != StateGeneration{generation.value()}) {
        return Error{ErrorCode::CorruptState, "state container generation disagrees with its payload"};
    }
    return state;
}

}  // namespace incast
