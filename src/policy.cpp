// Incast Governor - policy validation and digest.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "incast/policy.hpp"

#include <algorithm>

#include "incast/codec.hpp"

namespace incast {

Status GovernorPolicy::validate() const {
    if (!id.valid()) return fail(ErrorCode::InvalidArgument, "policy id is absent");
    if (!generation.valid()) return fail(ErrorCode::InvalidArgument, "policy generation is absent");
    if (name.empty() || name.size() > kMaxIdentifierChars) {
        return fail(ErrorCode::InvalidArgument, "policy name is empty or too long");
    }

    const auto& sync = synchronization;
    if (sync.many_to_one_min_senders < 2) {
        return fail(ErrorCode::OutOfRange, "many_to_one_min_senders must be at least 2");
    }
    if (sync.min_synchronized_senders < sync.many_to_one_min_senders) {
        return fail(ErrorCode::OutOfRange,
                    "min_synchronized_senders must be at least many_to_one_min_senders");
    }
    if (!sync.sync_window_bound.positive()) {
        return fail(ErrorCode::OutOfRange, "sync_window_bound must be positive");
    }
    if (sync.sync_index_threshold_bp > BasisPoints::kMax ||
        sync.min_aggregate_share_bp > BasisPoints::kMax) {
        return fail(ErrorCode::OutOfRange, "synchronization thresholds exceed the ratio domain");
    }

    const PressurePolicy& pressure_policy = pressure;
    if (pressure_policy.risk_oversubscription_bp > pressure_policy.collapse_oversubscription_bp) {
        return fail(ErrorCode::OutOfRange,
                    "risk_oversubscription_bp must not exceed collapse_oversubscription_bp");
    }
    if (pressure_policy.congestion_queue_pressure_bp > pressure_policy.active_queue_pressure_bp) {
        return fail(ErrorCode::OutOfRange,
                    "congestion_queue_pressure_bp must not exceed active_queue_pressure_bp");
    }
    if (pressure_policy.risk_oversubscription_bp == 0) {
        return fail(ErrorCode::OutOfRange, "risk_oversubscription_bp must be positive");
    }

    // Hysteresis requires a genuine separation between entry and exit bands.
    if (recovery.relax_oversubscription_bp >= pressure.risk_oversubscription_bp) {
        return fail(ErrorCode::OutOfRange,
                    "recovery relax_oversubscription_bp must be below the risk entry threshold");
    }
    if (recovery.relax_queue_pressure_bp >= pressure.active_queue_pressure_bp) {
        return fail(ErrorCode::OutOfRange,
                    "recovery relax_queue_pressure_bp must be below the active entry threshold");
    }
    if (recovery.enabled) {
        if (recovery.required_clean_windows == 0) {
            return fail(ErrorCode::OutOfRange,
                        "an enabled recovery policy requires at least one clean window");
        }
        if (recovery.dwell.nanos() < 0) {
            return fail(ErrorCode::OutOfRange, "recovery dwell must not be negative");
        }
    }
    if (hysteresis.min_dwell_windows == 0) {
        return fail(ErrorCode::OutOfRange, "min_dwell_windows must be positive");
    }
    if (hysteresis.max_incident_windows == 0 || hysteresis.max_clean_windows == 0) {
        return fail(ErrorCode::OutOfRange, "hysteresis window bounds must be positive");
    }

    const MitigationBounds& bounds_policy = bounds;
    if (bounds_policy.min_reduction_bp == 0 || bounds_policy.max_reduction_bp == 0) {
        return fail(ErrorCode::OutOfRange, "reduction bounds must be positive");
    }
    if (bounds_policy.min_reduction_bp > bounds_policy.max_reduction_bp) {
        return fail(ErrorCode::OutOfRange, "min_reduction_bp must not exceed max_reduction_bp");
    }
    if (bounds_policy.max_reduction_bp > 10000) {
        return fail(ErrorCode::OutOfRange, "max_reduction_bp may not exceed 10000 (100%)");
    }
    if (bounds_policy.max_admission_reduction_bp > 10000) {
        return fail(ErrorCode::OutOfRange, "max_admission_reduction_bp may not exceed 10000 (100%)");
    }
    if (bounds_policy.min_duration.nanos() <= 0 || bounds_policy.max_duration.nanos() <= 0) {
        return fail(ErrorCode::OutOfRange, "duration bounds must be positive");
    }
    if (bounds_policy.min_duration > bounds_policy.max_duration) {
        return fail(ErrorCode::OutOfRange, "min_duration must not exceed max_duration");
    }
    if (bounds_policy.max_penalized_senders == 0 || bounds_policy.max_penalized_senders > kMaxPenalizedSenders) {
        return fail(ErrorCode::OutOfRange, "max_penalized_senders outside the supported bound");
    }
    if (bounds_policy.max_stagger_slots == 0 || bounds_policy.max_stagger_slots > 4096) {
        return fail(ErrorCode::OutOfRange, "max_stagger_slots outside the supported bound");
    }

    if (protection.protected_floor_rate.bytes_per_second() != 0 && !enabled) {
        // A disabled policy may still carry a floor; this is not an error.
    }

    if (authority.max_freshness_horizon_nanos == 0) {
        return fail(ErrorCode::OutOfRange, "max_freshness_horizon_nanos must be positive");
    }
    if (authority.require_provenance != authority.required_classes[static_cast<std::size_t>(
                                           EvidenceClass::Provenance)]) {
        return fail(ErrorCode::InvalidArgument,
                    "authority.require_provenance contradicts required_classes[Provenance]");
    }
    return ok_status();
}

std::uint64_t GovernorPolicy::digest() const noexcept {
    codec::Writer writer;
    writer.u64(id.value());
    writer.u64(generation.value());
    writer.str(name);
    writer.boolean(enabled);
    writer.u32(synchronization.many_to_one_min_senders);
    writer.u32(synchronization.min_synchronized_senders);
    writer.i64(synchronization.sync_window_bound.nanos());
    writer.u32(synchronization.sync_index_threshold_bp);
    writer.u32(synchronization.min_aggregate_share_bp);
    writer.u32(pressure.risk_oversubscription_bp);
    writer.u32(pressure.collapse_oversubscription_bp);
    writer.u32(pressure.active_queue_pressure_bp);
    writer.u32(pressure.congestion_queue_pressure_bp);
    writer.u64(pressure.drop_events_for_active);
    writer.u32(pressure.headroom_warning_bp);
    writer.boolean(recovery.enabled);
    writer.u32(recovery.required_clean_windows);
    writer.i64(recovery.dwell.nanos());
    writer.u32(recovery.relax_oversubscription_bp);
    writer.u32(recovery.relax_queue_pressure_bp);
    writer.u64(recovery.max_drops_during_recovery);
    writer.boolean(recovery.require_capacity_revalidation);
    writer.u32(hysteresis.enter_margin_bp);
    writer.u32(hysteresis.exit_margin_bp);
    writer.u32(hysteresis.min_dwell_windows);
    writer.u64(protection.protected_floor_rate.bytes_per_second());
    writer.u8(static_cast<std::uint8_t>(protection.strongest_mitigable_class));
    writer.boolean(protection.never_penalize_explicitly_protected);
    writer.boolean(protection.refuse_sender_scope_when_population_saturated);
    writer.boolean(protection.escalate_instead_of_violating_floor);
    writer.u32(bounds.min_reduction_bp);
    writer.u32(bounds.max_reduction_bp);
    writer.u32(bounds.max_admission_reduction_bp);
    writer.u64(bounds.max_headroom_bytes.count());
    writer.i64(bounds.min_duration.nanos());
    writer.i64(bounds.max_duration.nanos());
    writer.u64(static_cast<std::uint64_t>(bounds.max_penalized_senders));
    writer.u32(bounds.max_stagger_slots);
    for (const bool required : authority.required_classes) writer.boolean(required);
    writer.boolean(authority.require_valid_lease);
    writer.boolean(authority.require_matching_epoch);
    writer.boolean(authority.require_matching_boot);
    writer.boolean(authority.refuse_on_identity_collision);
    writer.boolean(authority.refuse_on_event_mismatch);
    writer.u64(static_cast<std::uint64_t>(authority.max_freshness_horizon_nanos));

    const auto& buffer = writer.buffer();
    return static_cast<std::uint64_t>(crc32c(buffer.data(), buffer.size()));
}

GovernorPolicy make_default_policy() {
    GovernorPolicy policy{};
    policy.id = PolicyId{1};
    policy.generation = PolicyGeneration{1};
    policy.name = "default";
    policy.enabled = true;
    return policy;
}

}  // namespace incast
