// Incast Governor - bounded mitigation intent synthesis.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "incast/mitigate.hpp"

#include <algorithm>
#include <string>

namespace incast {
namespace {

[[nodiscard]] std::uint32_t clamp_bp(std::uint64_t value, std::uint32_t low, std::uint32_t high) {
    if (high < low) high = low;
    if (value < low) return low;
    if (value > high) return high;
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::uint64_t apply_bp(std::uint64_t amount, std::uint32_t basis_points) {
    return saturating_mul(amount, basis_points) / 10000ULL;
}

[[nodiscard]] std::int64_t clamp_nanos(std::int64_t value, std::int64_t low, std::int64_t high) {
    if (high < low) high = low;
    return value < low ? low : (value > high ? high : value);
}

[[nodiscard]] std::int64_t severity_weight(Severity severity) {
    switch (severity) {
        case Severity::None: return 0;
        case Severity::Advisory: return 1;
        case Severity::Elevated: return 2;
        case Severity::Severe: return 3;
        case Severity::Critical: return 4;
    }
    return 0;
}

[[nodiscard]] std::string describe_refusal(RefusalReason reason, const std::string& detail) {
    std::string text(to_string(reason));
    if (!detail.empty()) {
        text.append(": ");
        text.append(detail);
    }
    return text;
}

}  // namespace

std::size_t AuthorityVector::authoritative_class_count() const noexcept {
    std::size_t count = 0;
    for (const auto state : classes) {
        if (state == EvidenceState::Present) count += 1;
    }
    return count;
}

std::string AuthorityVector::summary() const {
    std::string text;
    text.reserve(256);
    text.append("authorized=");
    text.append(intervention_authorized ? "true" : "false");
    text.append(" refusal=");
    text.append(to_string(refusal));
    text.append(" classes=");
    for (std::size_t index = 0; index < kEvidenceClassCount; ++index) {
        if (index != 0) text.push_back(',');
        text.append(to_string(static_cast<EvidenceClass>(index)));
        text.push_back('=');
        text.append(to_string(classes[index]));
    }
    text.append(" bindings=");
    text.append(policy_enabled ? "P" : "-");
    text.append(event_bound ? "E" : "-");
    text.append(destination_bound ? "D" : "-");
    text.append(window_bound ? "W" : "-");
    text.append(epoch_bound ? "X" : "-");
    text.append(boot_bound ? "B" : "-");
    text.append(lease_bound ? "L" : "-");
    text.append(provenance_bound ? "V" : "-");
    if (population_saturated) text.append(" population=saturated");
    if (identity_collision_present) text.append(" identity_collision=true");
    if (!protected_floor_preserved) text.append(" protected_floor=violated");
    if (clamped) text.append(" clamped=true");
    return text;
}

bool clamp_to_bounds(MitigationIntent& intent, const MitigationBounds& bounds) noexcept {
    bool clamped = false;
    if (intent.reduction_bp.value() > bounds.max_reduction_bp) {
        intent.reduction_bp = BasisPoints{bounds.max_reduction_bp};
        clamped = true;
    }
    if (intent.admission_reduction_bp.value() > bounds.max_admission_reduction_bp) {
        intent.admission_reduction_bp = BasisPoints{bounds.max_admission_reduction_bp};
        clamped = true;
    }
    if (intent.headroom_bytes.count() > bounds.max_headroom_bytes.count()) {
        intent.headroom_bytes = bounds.max_headroom_bytes;
        clamped = true;
    }
    if (intent.stagger_slots > bounds.max_stagger_slots) {
        intent.stagger_slots = bounds.max_stagger_slots;
        clamped = true;
    }
    if (intent.penalized_senders.size() > bounds.max_penalized_senders) {
        intent.penalized_senders.resize(bounds.max_penalized_senders);
        clamped = true;
    }
    if (intent.duration.nanos() < bounds.min_duration.nanos()) {
        intent.duration = bounds.min_duration;
        clamped = true;
    }
    if (intent.duration.nanos() > bounds.max_duration.nanos()) {
        intent.duration = bounds.max_duration;
        clamped = true;
    }
    intent.clamped = intent.clamped || clamped;
    intent.bounded = true;
    return clamped;
}

SynthesisResult synthesize(const SynthesisRequest& request) {
    SynthesisResult result{};

    if (request.bundle == nullptr || request.policy == nullptr || request.classification == nullptr) {
        result.authority.refusal = RefusalReason::MalformedBundle;
        result.authority.refusal_detail.assign("synthesis request is incomplete");
        result.notes.emplace_back("synthesis refused: incomplete request");
        return result;
    }

    const EvidenceBundle& bundle = *request.bundle;
    const GovernorPolicy& policy = *request.policy;
    const Classification& classification = *request.classification;

    AuthorityVector& authority = result.authority;
    authority.classes = request.check.classes;
    authority.policy_enabled = policy.enabled;
    authority.policy_generation_bound = bundle.policy_stamp.state == EvidenceState::Present ||
                                        bundle.policy_stamp.state == EvidenceState::Unknown;
    authority.event_bound = bundle.event.valid() && bundle.event_generation.valid();
    authority.destination_bound = bundle.destination.valid() && bundle.destination_generation.valid();
    authority.window_bound = bundle.window.valid() && bundle.window_generation.valid();
    authority.epoch_bound = bundle.epoch.valid();
    authority.boot_bound = bundle.boot.valid();
    authority.lease_bound = bundle.lease.valid();
    authority.provenance_bound = bundle.provenance_stamp.state == EvidenceState::Present &&
                                 bundle.provenance_stamp.provenance.valid();
    authority.population_saturated = classification.metrics.population_saturated;
    authority.identity_collision_present = classification.metrics.collided_senders > 0;
    authority.recovery_policy_present = policy.recovery.enabled;

    MitigationIntent& intent = result.intent;
    intent.intervention = request.intervention;
    intent.generation = request.generation;
    intent.event = bundle.event;
    intent.event_generation = bundle.event_generation;
    intent.destination = bundle.destination;
    intent.destination_generation = bundle.destination_generation;
    intent.window = bundle.window;
    intent.window_generation = bundle.window_generation;
    intent.policy = policy.id;
    intent.policy_generation = policy.generation;
    intent.epoch = bundle.epoch;
    intent.boot = bundle.boot;
    intent.worker = bundle.worker;
    intent.lease = bundle.lease;
    intent.issued_at = request.issued_at.valid() ? request.issued_at : bundle.now;
    {
        // The affected queue is the most pressurized observation, matching the
        // metric-selection rule used by the detector.
        std::uint64_t worst = 0;
        bool have = false;
        for (const auto& queue : bundle.queues) {
            const auto pressure = ratio_bp(queue.occupancy.count(), queue.capacity.count());
            if (!have || pressure.value() > worst || (pressure.value() == worst && queue.queue < intent.queue)) {
                have = true;
                worst = pressure.value();
                intent.queue = queue.queue;
                intent.resource = queue.resource;
            }
        }
        if (!have) intent.resource = bundle.capacity.resource;
    }
    intent.protected_offered = classification.metrics.protected_offered;
    intent.protected_floor = policy.protection.protected_floor_rate.bytes_per_second() != 0
                                 ? policy.protection.protected_floor_rate
                                 : bundle.capacity.protected_floor_rate;
    intent.resulting_offered = classification.metrics.aggregate_offered;

    if (request.check.reason != RefusalReason::None) {
        authority.intervention_authorized = false;
        authority.refusal = request.check.reason;
        authority.refusal_detail = request.check.detail;
        result.notes.push_back(describe_refusal(request.check.reason, request.check.detail));
        return result;
    }

    if (!policy.enabled) {
        authority.intervention_authorized = false;
        authority.refusal = RefusalReason::PolicyDisabled;
        authority.refusal_detail.assign("policy document is disabled");
        result.notes.emplace_back("no intervention: policy disabled");
        return result;
    }

    const IncidentKind kind = classification.kind;
    if (kind == IncidentKind::Unknown) {
        authority.intervention_authorized = false;
        authority.refusal = classification.refusal == RefusalReason::None ? RefusalReason::MissingEvidence
                                                                          : classification.refusal;
        authority.refusal_detail = classification.refusal_detail;
        result.notes.emplace_back("no intervention: incident classification is UNKNOWN");
        return result;
    }
    if (kind == IncidentKind::NoFanIn || kind == IncidentKind::OrdinaryManyToOne) {
        authority.intervention_authorized = false;
        authority.refusal = RefusalReason::NoIncident;
        authority.refusal_detail.assign(
            kind == IncidentKind::NoFanIn
                ? "observed demand is not many-to-one"
                : "many-to-one demand is not synchronized: incast mitigation is not authorized");
        result.notes.emplace_back("no intervention: synchronization not demonstrated");
        return result;
    }

    const auto& bounds = policy.bounds;
    const auto& metrics = classification.metrics;

    // Recovery: relaxation of a live intervention is only reachable through an
    // explicit recovery policy and an explicit recovery step.
    if (kind == IncidentKind::Recovery) {
        if (!policy.recovery.enabled) {
            authority.intervention_authorized = false;
            authority.refusal = RefusalReason::AwaitingRecoveryPolicy;
            authority.refusal_detail.assign("recovery policy is not enabled; intervention holds");
            result.notes.emplace_back("no relaxation: recovery policy is not enabled");
            return result;
        }
        if (request.prior == nullptr || !request.prior->active()) {
            authority.intervention_authorized = false;
            authority.refusal = RefusalReason::NoIncident;
            authority.refusal_detail.assign("no live intervention to relax");
            result.notes.emplace_back("no relaxation: no live intervention");
            return result;
        }
        const std::uint32_t step = request.recovery_step_bp.value() != 0
                                       ? request.recovery_step_bp.value()
                                       : std::max<std::uint32_t>(bounds.min_reduction_bp,
                                                                 request.prior->reduction_bp.value() / 4);
        const std::uint32_t prior_reduction = request.prior->reduction_bp.value();
        if (prior_reduction <= step || prior_reduction - step < bounds.min_reduction_bp) {
            intent = *request.prior;
            intent.kind = MitigationKind::None;
            intent.scope = MitigationScope::None;
            intent.reduction_bp = BasisPoints{0};
            intent.admission_reduction_bp = BasisPoints{0};
            intent.headroom_bytes = Bytes{0};
            intent.stagger_slots = 0;
            intent.penalized_senders.clear();
            intent.affected_classes.clear();
            intent.relaxes_prior = true;
            intent.continues_prior = false;
            intent.duration = bounds.min_duration;
            intent.issued_at = request.issued_at.valid() ? request.issued_at : bundle.now;
            intent.expires_at = Timestamp{intent.issued_at.nanos() + intent.duration.nanos()};
            intent.insufficient = false;
            intent.escalating = false;
            authority.intervention_authorized = true;
            authority.protected_floor_preserved = true;
            result.notes.emplace_back("recovery complete: intervention retired under the recovery policy");
            return result;
        }
        intent = *request.prior;
        intent.kind = request.prior->kind;
        intent.scope = request.prior->scope;
        intent.reduction_bp = BasisPoints{prior_reduction - step};
        intent.relaxes_prior = true;
        intent.continues_prior = true;
        intent.affected_classes = request.prior->affected_classes;
        intent.penalized_senders = request.prior->penalized_senders;
        intent.issued_at = request.issued_at.valid() ? request.issued_at : bundle.now;
        authority.intervention_authorized = true;
        authority.protected_floor_preserved = true;
        static_cast<void>(clamp_to_bounds(intent, bounds));
        intent.expires_at = Timestamp{intent.issued_at.nanos() + intent.duration.nanos()};
        authority.clamped = intent.clamped;
        result.notes.emplace_back("recovery step applied: reduction relaxed under the recovery policy");
        return result;
    }

    // Risk and active incast both require a bounded reduction toward service capacity.
    const std::uint64_t aggregate = metrics.aggregate_offered.bytes_per_second();
    const std::uint64_t service = metrics.service_rate.bytes_per_second();
    const std::uint64_t relax_target = apply_bp(service, policy.recovery.relax_oversubscription_bp);
    std::uint64_t target = service;
    if (kind == IncidentKind::ActiveIncast && relax_target != 0 && relax_target < service) {
        target = relax_target;
    }
    std::uint64_t required = aggregate > target ? aggregate - target : 0;
    intent.required_reduction = Rate{required};

    const bool sender_scope_allowed = !(metrics.population_saturated &&
                                        policy.protection.refuse_sender_scope_when_population_saturated);

    std::uint64_t mitigable_offer = 0;
    std::vector<SenderIdentity> penalized;
    std::vector<ServiceClass> classes;
    if (sender_scope_allowed) {
        for (const auto& contribution : classification.contributions) {
            if (!contribution.mitigable) continue;
            if (penalized.size() >= bounds.max_penalized_senders) break;
            penalized.push_back(contribution.identity);
            mitigable_offer = saturating_add(mitigable_offer, contribution.offered_rate.bytes_per_second());
            if (std::find(classes.begin(), classes.end(), contribution.service) == classes.end()) {
                classes.push_back(contribution.service);
            }
        }
    }

    std::uint32_t reduction_bp = 0;
    std::uint32_t admission_bp = 0;
    std::uint64_t achievable = 0;

    if (required == 0) {
        // Capacity is not exceeded but the queue is still collapsing: the
        // authorized action is structural (headroom and staggering), not rate.
        if (kind == IncidentKind::ActiveIncast) {
            intent.kind = MitigationKind::TemporaryHeadroomAdjustment;
            intent.scope = MitigationScope::Queue;
            const std::uint64_t headroom_needed =
                metrics.queue_capacity.count() > metrics.high_watermark.count()
                    ? metrics.queue_capacity.count() - metrics.high_watermark.count()
                    : metrics.queue_capacity.count() / 4;
            intent.headroom_bytes = Bytes{std::min<std::uint64_t>(headroom_needed,
                                                                  bounds.max_headroom_bytes.count())};
            intent.stagger_slots = clamp_bp(metrics.active_senders, 1, bounds.max_stagger_slots);
            authority.intervention_authorized = true;
        } else {
            authority.intervention_authorized = false;
            authority.refusal = RefusalReason::NoIncident;
            authority.refusal_detail.assign("offered rate is already within the destination service rate");
            result.notes.emplace_back("no intervention: no reduction is required");
            return result;
        }
    } else if (sender_scope_allowed && mitigable_offer > 0) {
        const std::uint64_t needed_bp = saturating_mul(required, 10000ULL) / mitigable_offer +
                                        ((saturating_mul(required, 10000ULL) % mitigable_offer) != 0 ? 1 : 0);
        reduction_bp = clamp_bp(needed_bp, bounds.min_reduction_bp, bounds.max_reduction_bp);
        achievable = apply_bp(mitigable_offer, reduction_bp);
        intent.kind = kind == IncidentKind::ActiveIncast ? MitigationKind::PriorityAwareRateReduction
                                                         : MitigationKind::SenderStaggering;
        intent.scope = MitigationScope::SenderSet;
        intent.penalized_senders = std::move(penalized);
        intent.affected_classes = std::move(classes);
        intent.stagger_slots = intent.kind == MitigationKind::SenderStaggering
                                   ? clamp_bp(metrics.active_senders, 1, bounds.max_stagger_slots)
                                   : 0;
        authority.intervention_authorized = true;
    } else {
        // No provable per-sender scope: a saturated population cannot prove
        // which senders are protected, so only a destination-scoped bounded
        // admission reduction is authorized.
        const std::uint64_t needed_bp = saturating_mul(required, 10000ULL) / (aggregate == 0 ? 1 : aggregate);
        admission_bp = clamp_bp(needed_bp, std::min<std::uint32_t>(bounds.min_reduction_bp,
                                                                   bounds.max_admission_reduction_bp),
                                bounds.max_admission_reduction_bp);
        achievable = apply_bp(aggregate, admission_bp);
        intent.kind = MitigationKind::TemporaryAdmissionReduction;
        intent.scope = MitigationScope::Destination;
        authority.intervention_authorized = true;
        result.notes.emplace_back(
            sender_scope_allowed ? "no mitigable per-sender population: destination-scoped action"
                                 : "population saturated: sender-scoped penalization is not authorized");
    }

    intent.reduction_bp = BasisPoints{reduction_bp};
    intent.admission_reduction_bp = BasisPoints{admission_bp};
    intent.achievable_reduction = Rate{achievable};

    // Structural headroom relief accompanies active incast regardless of the
    // primary action, bounded by policy.
    if (kind == IncidentKind::ActiveIncast && intent.kind != MitigationKind::TemporaryHeadroomAdjustment) {
        const std::uint64_t headroom_ratio =
            ratio_bp(metrics.buffer_headroom.count(), bundle.capacity.buffer_capacity.count()).value();
        if (bundle.capacity.buffer_capacity.count() != 0 &&
            headroom_ratio < policy.pressure.headroom_warning_bp) {
            intent.headroom_bytes = Bytes{std::min<std::uint64_t>(bundle.capacity.buffer_capacity.count() / 4,
                                                                  bounds.max_headroom_bytes.count())};
            result.notes.emplace_back("buffer headroom relief included (bounded)");
        }
    }

    const std::int64_t weight = severity_weight(classification.severity);
    const std::int64_t span = bounds.max_duration.nanos() - bounds.min_duration.nanos();
    const std::int64_t derived = bounds.min_duration.nanos() + (span * weight) / 4;
    intent.duration = Duration{clamp_nanos(derived, bounds.min_duration.nanos(), bounds.max_duration.nanos())};

    intent.resulting_offered = Rate{aggregate >= achievable ? aggregate - achievable : 0};
    intent.continues_prior = request.prior != nullptr && request.prior->active();
    intent.relaxes_prior = false;

    // Residual demand that cannot be removed without penalizing protected
    // senders is escalated rather than absorbed by violating protection.
    const std::uint64_t residual = required > achievable ? required - achievable : 0;
    if (residual > 0) {
        intent.insufficient = true;
        const bool floor_conflict = metrics.protected_offered.positive() ||
                                    intent.protected_floor.bytes_per_second() != 0;
        if (floor_conflict && !policy.protection.escalate_instead_of_violating_floor) {
            authority.intervention_authorized = false;
            authority.refusal = RefusalReason::ProtectedFloorConflict;
            authority.refusal_detail.assign(
                "required reduction cannot be achieved without penalizing protected obligations");
            result.intent = MitigationIntent{};
            result.notes.emplace_back("refused: protected floor conflict and escalation is disabled");
            return result;
        }
        intent.escalating = true;
        authority.protected_floor_preserved = true;
        result.notes.emplace_back("residual demand escalated: protected obligations are preserved");
        if (achievable == 0) {
            intent.kind = MitigationKind::CongestionEscalation;
            intent.scope = MitigationScope::Destination;
            intent.escalating = true;
        }
    }

    if (intent.protected_floor.bytes_per_second() != 0 &&
        intent.protected_offered.bytes_per_second() < intent.protected_floor.bytes_per_second()) {
        // The protected floor is already breached before any mitigation. The
        // governor records the breach; it never resolves it by penalizing
        // protected traffic.
        authority.protected_floor_preserved = true;
        result.notes.emplace_back("protected demand is already below its declared floor");
    }

    static_cast<void>(clamp_to_bounds(intent, bounds));
    authority.clamped = intent.clamped;
    authority.bounds_respected = true;
    authority.refusal = RefusalReason::None;
    authority.refusal_detail.clear();

    intent.expires_at = Timestamp{intent.issued_at.nanos() + intent.duration.nanos()};
    return result;
}

}  // namespace incast
