// Incast Governor - intervention lifecycle implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "incast/lifecycle.hpp"

#include <string>

namespace incast {

bool is_legal_transition(InterventionState from, InterventionState to) noexcept {
    if (from == to) return true;
    switch (from) {
        case InterventionState::Proposed:
            return to == InterventionState::Authorized || to == InterventionState::Refused ||
                   to == InterventionState::Fenced;
        case InterventionState::Authorized:
            return to == InterventionState::Active || to == InterventionState::Fenced ||
                   to == InterventionState::Expired || to == InterventionState::Retired ||
                   to == InterventionState::Revalidating;
        case InterventionState::Active:
            return to == InterventionState::Relaxing || to == InterventionState::Fenced ||
                   to == InterventionState::Expired || to == InterventionState::Retired ||
                   to == InterventionState::Revalidating;
        case InterventionState::Relaxing:
            return to == InterventionState::Active || to == InterventionState::Retired ||
                   to == InterventionState::Fenced || to == InterventionState::Expired ||
                   to == InterventionState::Revalidating;
        case InterventionState::Revalidating:
            return to == InterventionState::Active || to == InterventionState::Retired ||
                   to == InterventionState::Fenced || to == InterventionState::Refused ||
                   to == InterventionState::Expired;
        case InterventionState::Retired:
            return to == InterventionState::Revalidating;
        case InterventionState::Fenced:
        case InterventionState::Refused:
        case InterventionState::Expired:
            return false;
    }
    return false;
}

Status transition(Intervention& intervention,
                  InterventionState to,
                  Timestamp now,
                  std::string detail) {
    if (!is_legal_transition(intervention.state, to)) {
        std::string message("illegal intervention transition: ");
        message.append(to_string(intervention.state));
        message.append(" -> ");
        message.append(to_string(to));
        if (!detail.empty()) {
            message.append(" (");
            message.append(detail);
            message.append(")");
        }
        return fail(ErrorCode::IllegalTransition, std::move(message));
    }
    intervention.state = to;
    if (now.valid()) intervention.updated_at = now;
    if (to == InterventionState::Relaxing) {
        intervention.relax_steps += 1;
        intervention.last_relaxed_at = now;
    }
    if (to == InterventionState::Refused) {
        intervention.refusal_detail = std::move(detail);
    }
    return ok_status();
}

bool is_fenced(const Intervention& intervention,
               const FenceContext& context,
               RefusalReason& reason,
               std::string& detail) {
    if (context.epoch.valid() && intervention.epoch != context.epoch) {
        reason = RefusalReason::EpochMismatch;
        detail.assign("intervention belongs to a superseded coordinator epoch");
        return true;
    }
    if (context.boot.valid() && intervention.boot != context.boot) {
        reason = RefusalReason::BootMismatch;
        detail.assign("intervention belongs to a previous boot incarnation");
        return true;
    }
    if (context.worker.valid() && intervention.worker.valid() && intervention.worker != context.worker) {
        reason = RefusalReason::EpochMismatch;
        detail.assign("intervention belongs to a different worker");
        return true;
    }
    if (context.lease.valid() && intervention.lease.valid() && intervention.lease != context.lease) {
        reason = RefusalReason::LeaseMissing;
        detail.assign("intervention belongs to a superseded lease");
        return true;
    }
    if (context.lease_expiry.valid() && context.now.valid() && context.now > context.lease_expiry) {
        reason = RefusalReason::LeaseExpired;
        detail.assign("intervention lease expired");
        return true;
    }
    if (intervention.expires_at.valid() && context.now.valid() && context.now > intervention.expires_at) {
        reason = RefusalReason::AwaitingRecoveryPolicy;
        detail.assign("intervention reached its expiry boundary and requires revalidation");
        return true;
    }
    reason = RefusalReason::None;
    detail.clear();
    return false;
}

Status EpochTracker::advance(EpochId requested) {
    if (!requested.valid()) {
        return fail(ErrorCode::InvalidArgument, "requested epoch is absent");
    }
    if (epoch_.valid() && requested <= epoch_) {
        return fail(ErrorCode::StaleGeneration, "requested epoch is not newer than the current epoch");
    }
    epoch_ = requested;
    return ok_status();
}

RecoveryVerdict evaluate_recovery(const RecoveryPolicy& policy,
                                  const RecoveryTracker& tracker,
                                  const FanInMetrics& metrics,
                                  bool policy_enabled) {
    RecoveryVerdict verdict{};
    if (!policy_enabled) {
        verdict.reason = RefusalReason::PolicyDisabled;
        verdict.detail.assign("recovery policy is disabled");
        return verdict;
    }
    if (metrics.oversubscription_bp.value() > policy.relax_oversubscription_bp) {
        verdict.reason = RefusalReason::NoIncident;
        verdict.detail.assign("aggregate offered rate has not returned inside the recovery band");
        return verdict;
    }
    if (metrics.queue_pressure_bp.value() > policy.relax_queue_pressure_bp) {
        verdict.reason = RefusalReason::NoIncident;
        verdict.detail.assign("queue pressure has not returned inside the recovery band");
        return verdict;
    }
    if (metrics.drop_events > policy.max_drops_during_recovery) {
        verdict.reason = RefusalReason::NoIncident;
        verdict.detail.assign("queue drops are still being observed");
        return verdict;
    }
    if (tracker.clean_windows < policy.required_clean_windows) {
        verdict.reason = RefusalReason::NoIncident;
        verdict.detail.assign("clean-window dwell has not been satisfied");
        return verdict;
    }
    if (policy.require_capacity_revalidation && !tracker.capacity_revalidated) {
        verdict.reason = RefusalReason::CapacityNotRevalidated;
        verdict.detail.assign("destination capacity has not been revalidated");
        return verdict;
    }
    verdict.relax = true;
    verdict.step_bp = BasisPoints{0};
    verdict.detail.assign("recovery criteria satisfied");
    return verdict;
}

}  // namespace incast
