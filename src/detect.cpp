// Incast Governor - detection implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "incast/detect.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace incast {
namespace {

[[nodiscard]] const EvidenceStamp* stamp_for(const EvidenceBundle& bundle, EvidenceClass klass) {
    switch (klass) {
        case EvidenceClass::FanIn: return &bundle.fan_in_stamp;
        case EvidenceClass::ArrivalTiming: return &bundle.timing_stamp;
        case EvidenceClass::AggregateRate: return &bundle.rate_stamp;
        case EvidenceClass::Destination: return &bundle.capacity_stamp;
        case EvidenceClass::Capacity: return &bundle.capacity_stamp;
        case EvidenceClass::Queue: return &bundle.queue_stamp;
        case EvidenceClass::Service: return &bundle.service_stamp;
        case EvidenceClass::Provenance: return &bundle.provenance_stamp;
    }
    return nullptr;
}

[[nodiscard]] RefusalReason refusal_for_state(EvidenceState state) {
    switch (state) {
        case EvidenceState::Present: return RefusalReason::None;
        case EvidenceState::Unknown: return RefusalReason::MissingEvidence;
        case EvidenceState::Stale: return RefusalReason::StaleEvidence;
        case EvidenceState::Contradictory: return RefusalReason::ContradictoryEvidence;
        case EvidenceState::Reordered: return RefusalReason::ReorderedEvidence;
        case EvidenceState::OutOfWindow: return RefusalReason::OutOfWindowEvidence;
        case EvidenceState::Superseded: return RefusalReason::SupersededEvidence;
    }
    return RefusalReason::MissingEvidence;
}

[[nodiscard]] int kind_rank(IncidentKind kind) noexcept {
    switch (kind) {
        case IncidentKind::Unknown: return -1;
        case IncidentKind::NoFanIn: return 0;
        case IncidentKind::OrdinaryManyToOne: return 1;
        case IncidentKind::Recovery: return 2;
        case IncidentKind::SynchronizedIncastRisk: return 3;
        case IncidentKind::ActiveIncast: return 4;
    }
    return -1;
}

}  // namespace

bool is_protected_sender(const SenderObservation& sender, const GovernorPolicy& policy) {
    if (policy.protection.never_penalize_explicitly_protected && sender.protected_obligation) return true;
    return service_strength(sender.service) > service_strength(policy.protection.strongest_mitigable_class);
}

bool is_synchronized_sender(const SenderObservation& sender,
                            Timestamp earliest,
                            const SynchronizationEvidence& synchronization) {
    if (!sender.first_arrival.valid() || !earliest.valid()) return false;
    if (sender.first_arrival < earliest) return false;
    const std::int64_t offset = sender.first_arrival.nanos() - earliest.nanos();
    return offset <= synchronization.sync_window_bound.nanos();
}

std::string SynchronizationEvidence::summary() const {
    std::string text;
    text.reserve(224);
    if (!evaluated) {
        text.append("synchronization=not-evaluated");
        return text;
    }
    text.append("population=");
    text.append(format_u64(population));
    text.append(" synchronized=");
    text.append(format_u64(synchronized_senders));
    text.append(" spread_ns=");
    text.append(std::to_string(arrival_spread.nanos()));
    text.append(" bound_ns=");
    text.append(std::to_string(sync_window_bound.nanos()));
    text.append(" compression_bp=");
    text.append(format_u64(arrival_compression_bp.value()));
    text.append(" share_bp=");
    text.append(format_u64(synchronized_share_bp.value()));
    text.append(synchronized ? " verdict=synchronized" : " verdict=not-synchronized");
    return text;
}

std::vector<SenderCollision> detect_collisions(const EvidenceBundle& bundle) {
    struct Aggregate {
        std::uint32_t occurrences = 0;
        std::unordered_set<std::uint64_t> incarnations{};
        std::uint8_t weakest_service = 0xFF;
        std::uint8_t strongest_service = 0;
        bool protected_seen = false;
        bool unprotected_seen = false;
    };

    std::unordered_map<std::uint64_t, Aggregate> by_sender;
    const std::size_t count = std::min(bundle.senders.size(), kMaxSendersPerWindow);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& sender = bundle.senders[index];
        if (sender.identity.sender.absent() || !sender.identity.incarnation.valid()) continue;

        auto& aggregate = by_sender[sender.identity.sender.value()];
        aggregate.occurrences += 1;
        aggregate.incarnations.insert(sender.identity.incarnation.value());
        const auto strength = service_strength(sender.service);
        aggregate.weakest_service = std::min<std::uint8_t>(aggregate.weakest_service, strength);
        aggregate.strongest_service = std::max<std::uint8_t>(aggregate.strongest_service, strength);
        if (sender.protected_obligation) {
            aggregate.protected_seen = true;
        } else {
            aggregate.unprotected_seen = true;
        }
    }

    std::vector<SenderCollision> collisions;
    for (const auto& [sender_value, aggregate] : by_sender) {
        CollisionKind kind = CollisionKind::None;
        if (aggregate.incarnations.size() > 1) {
            kind = CollisionKind::DuplicateSenderDifferentIncarnation;
        } else if (aggregate.weakest_service != aggregate.strongest_service ||
                   (aggregate.protected_seen && aggregate.unprotected_seen)) {
            kind = CollisionKind::ContradictoryAttributes;
        }
        if (kind == CollisionKind::None) continue;
        SenderCollision collision{};
        collision.sender = SenderId{sender_value};
        collision.kind = kind;
        collision.occurrences = aggregate.occurrences;
        collisions.push_back(collision);
    }

    std::sort(collisions.begin(), collisions.end(), [](const SenderCollision& a, const SenderCollision& b) {
        if (a.sender != b.sender) return a.sender < b.sender;
        return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
    });
    return collisions;
}

FanInMetrics compute_metrics(const EvidenceBundle& bundle, const GovernorPolicy& policy) {
    FanInMetrics metrics{};
    metrics.population_saturated = bundle.fan_in_stamp.truncated || bundle.senders.size() > kMaxSendersPerWindow;

    const std::size_t count = std::min(bundle.senders.size(), kMaxSendersPerWindow);
    bool have_arrival = false;
    std::uint64_t aggregate_offered = 0;
    std::uint64_t protected_offered = 0;
    std::uint64_t outstanding_total = 0;

    std::vector<FlowId> flows;
    flows.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        const auto& sender = bundle.senders[index];
        if (sender.identity.sender.absent() || !sender.identity.incarnation.valid()) continue;

        aggregate_offered = saturating_add(aggregate_offered, sender.offered_rate.bytes_per_second());
        outstanding_total = saturating_add(outstanding_total, sender.outstanding.count());
        if (is_protected_sender(sender, policy)) {
            protected_offered = saturating_add(protected_offered, sender.offered_rate.bytes_per_second());
            metrics.protected_senders += 1;
        }
        if (sender.first_arrival.valid()) {
            if (!have_arrival) {
                metrics.earliest_arrival = sender.first_arrival;
                metrics.latest_arrival = sender.first_arrival;
                have_arrival = true;
            } else {
                metrics.earliest_arrival = std::min(metrics.earliest_arrival, sender.first_arrival);
                metrics.latest_arrival = std::max(metrics.latest_arrival, sender.first_arrival);
            }
        }
        if (sender.identity.flow.valid()) flows.push_back(sender.identity.flow);
        metrics.active_senders += 1;
    }

    metrics.aggregate_offered = Rate{aggregate_offered};
    metrics.protected_offered = Rate{protected_offered};
    metrics.penalizable_offered = Rate{aggregate_offered >= protected_offered ? aggregate_offered - protected_offered : 0};

    std::sort(flows.begin(), flows.end());
    flows.erase(std::unique(flows.begin(), flows.end()), flows.end());
    metrics.distinct_flows = static_cast<std::uint32_t>(flows.size());

    if (have_arrival && metrics.latest_arrival >= metrics.earliest_arrival) {
        metrics.arrival_spread = Duration{metrics.latest_arrival.nanos() - metrics.earliest_arrival.nanos()};
    }
    if (bundle.window_start.valid() && bundle.window_end.valid() && bundle.window_end >= bundle.window_start) {
        metrics.window_width = Duration{bundle.window_end.nanos() - bundle.window_start.nanos()};
    }

    metrics.service_rate = bundle.capacity.service_rate;
    metrics.queue_capacity = bundle.capacity.queue_capacity;
    metrics.buffer_headroom = Bytes{bundle.capacity.buffer_capacity.count() > outstanding_total
                                        ? bundle.capacity.buffer_capacity.count() - outstanding_total
                                        : 0};

    const std::size_t queue_count = std::min(bundle.queues.size(), kMaxQueueObservations);
    std::uint64_t worst_pressure = 0;
    bool have_queue = false;
    QueueId worst_queue{};
    for (std::size_t index = 0; index < queue_count; ++index) {
        const auto& queue = bundle.queues[index];
        const auto pressure = ratio_bp(queue.occupancy.count(), queue.capacity.count());
        const bool better = !have_queue || pressure.value() > worst_pressure ||
                            (pressure.value() == worst_pressure && queue.queue < worst_queue);
        if (!better) continue;
        have_queue = true;
        worst_pressure = pressure.value();
        worst_queue = queue.queue;
        metrics.queue_occupancy = queue.occupancy;
        metrics.queue_capacity = queue.capacity.count() != 0 ? queue.capacity : bundle.capacity.queue_capacity;
        metrics.high_watermark = queue.high_watermark;
        metrics.drop_events = queue.drop_events;
        metrics.ecn_marks = queue.ecn_marks;
    }
    metrics.queue_pressure_bp = have_queue
                                    ? ratio_bp(metrics.queue_occupancy.count(), metrics.queue_capacity.count())
                                    : BasisPoints{0};
    metrics.oversubscription_bp = ratio_bp(metrics.aggregate_offered.bytes_per_second(),
                                           metrics.service_rate.bytes_per_second());
    metrics.collisions = detect_collisions(bundle);
    metrics.collided_senders = static_cast<std::uint32_t>(metrics.collisions.size());
    return metrics;
}

namespace {

SynchronizationEvidence compute_synchronization(const EvidenceBundle& bundle,
                                                const FanInMetrics& metrics,
                                                const GovernorPolicy& policy) {
    SynchronizationEvidence evidence{};
    evidence.evaluated = true;
    evidence.population = metrics.active_senders;
    evidence.arrival_spread = metrics.arrival_spread;
    evidence.window_width = metrics.window_width;
    evidence.sync_window_bound = policy.synchronization.sync_window_bound;
    evidence.saturated = metrics.population_saturated;

    if (metrics.window_width.positive()) {
        const auto consumed = ratio_bp(static_cast<std::uint64_t>(metrics.arrival_spread.nanos()),
                                       static_cast<std::uint64_t>(metrics.window_width.nanos()));
        const std::uint32_t capped = consumed.value() > BasisPoints::kMax ? BasisPoints::kMax : consumed.value();
        evidence.arrival_compression_bp = BasisPoints{10000u > capped ? 10000u - capped : 0u};
    } else {
        evidence.arrival_compression_bp = BasisPoints{0};
    }

    if (metrics.earliest_arrival.valid()) {
        const std::int64_t limit =
            metrics.earliest_arrival.nanos() + policy.synchronization.sync_window_bound.nanos();
        for (const auto& sender : bundle.senders) {
            if (!sender.identity.valid() || !sender.first_arrival.valid()) continue;
            if (sender.first_arrival.nanos() <= limit) evidence.synchronized_senders += 1;
        }
    }

    std::uint64_t synchronized_offered = 0;
    if (metrics.earliest_arrival.valid()) {
        const std::int64_t limit =
            metrics.earliest_arrival.nanos() + policy.synchronization.sync_window_bound.nanos();
        for (const auto& sender : bundle.senders) {
            if (!sender.identity.valid() || !sender.first_arrival.valid()) continue;
            if (sender.first_arrival.nanos() <= limit) {
                synchronized_offered = saturating_add(synchronized_offered,
                                                      sender.offered_rate.bytes_per_second());
            }
        }
    }
    evidence.synchronized_share_bp =
        ratio_bp(synchronized_offered, metrics.aggregate_offered.bytes_per_second());

    evidence.population_floor_met =
        evidence.population >= policy.synchronization.min_synchronized_senders;

    // The window being narrower than the synchronization bound makes the
    // relative-compression test trivially satisfied; otherwise the arrivals
    // must occupy no more than the configured fraction of the window.
    const bool window_inside_bound = metrics.window_width.positive() &&
                                     metrics.window_width <= policy.synchronization.sync_window_bound;
    const bool relative_ok = window_inside_bound ||
                             evidence.arrival_compression_bp.value() >=
                                 policy.synchronization.sync_index_threshold_bp;
    const bool absolute_ok = metrics.arrival_spread <= policy.synchronization.sync_window_bound &&
                             metrics.active_senders > 0 && metrics.window_width.positive();
    evidence.window_compressed = absolute_ok && relative_ok;
    evidence.share_sufficient =
        evidence.synchronized_share_bp.value() >= policy.synchronization.min_aggregate_share_bp;
    evidence.synchronized = evidence.population_floor_met && evidence.window_compressed &&
                            evidence.share_sufficient;
    return evidence;
}

}  // namespace

AuthorityCheck check_authority(const EvidenceBundle& bundle,
                               const GovernorPolicy& policy,
                               const std::array<std::uint64_t, kEvidenceClassCount>& last_sequence) {
    AuthorityCheck check{};
    if (!policy.enabled) {
        check.reason = RefusalReason::PolicyDisabled;
        check.detail.assign("policy is disabled");
        return check;
    }
    if (!bundle.structurally_valid()) {
        check.reason = RefusalReason::MalformedBundle;
        check.detail.assign("evidence bundle failed structural validation");
        return check;
    }
    bool saw_provenance = false;
    for (std::size_t index = 0; index < kEvidenceClassCount; ++index) {
        const auto klass = static_cast<EvidenceClass>(index);
        const EvidenceStamp* stamp = stamp_for(bundle, klass);
        if (stamp == nullptr) continue;

        EvidenceState state = stamp->state;
        std::string detail;

        if (state == EvidenceState::Present) {
            if (!stamp->observed_at.valid()) {
                state = EvidenceState::Unknown;
                detail.assign("evidence has no observation instant");
            } else if (bundle.now.valid() && stamp->observed_at > bundle.now) {
                state = EvidenceState::Contradictory;
                detail.assign("evidence was observed after the evaluation instant");
            } else if (stamp->valid_until.valid() && bundle.now.valid() && bundle.now > stamp->valid_until) {
                state = EvidenceState::Stale;
                detail.assign("evidence passed its declared validity horizon");
            } else if (policy.authority.max_freshness_horizon_nanos > 0 && bundle.now.valid() &&
                       bundle.now.nanos() - stamp->observed_at.nanos() >
                           static_cast<std::int64_t>(policy.authority.max_freshness_horizon_nanos)) {
                state = EvidenceState::Stale;
                detail.assign("evidence is older than the configured freshness horizon");
            } else if (stamp->window.valid() && bundle.window.valid() && stamp->window != bundle.window) {
                state = EvidenceState::OutOfWindow;
                detail.assign("evidence belongs to a different window");
            } else if (stamp->sequence != 0 && last_sequence[index] != 0 &&
                       stamp->sequence <= last_sequence[index]) {
                state = EvidenceState::Reordered;
                detail.assign("evidence sequence is not newer than the last accepted sequence");
            } else if (klass == EvidenceClass::Capacity && !bundle.capacity.service_rate.positive()) {
                state = EvidenceState::Contradictory;
                detail.assign("destination capacity reports no positive service rate");
            } else if (klass == EvidenceClass::Provenance &&
                       (!stamp->provenance.valid() || !stamp->provenance_generation.valid())) {
                state = EvidenceState::Unknown;
                detail.assign("provenance identity or generation is absent");
            }
        } else if (detail.empty()) {
            detail.assign("evidence is not present");
        }

        check.classes[index] = state;
        if (klass == EvidenceClass::Provenance && state == EvidenceState::Present) saw_provenance = true;

        const bool required = policy.authority.required_classes[index];
        if (required && state != EvidenceState::Present && check.reason == RefusalReason::None) {
            check.reason = refusal_for_state(state);
            std::string composed(to_string(klass));
            composed.append(": ");
            composed.append(detail.empty() ? std::string("evidence not authoritative") : detail);
            check.detail = std::move(composed);
        }
    }

    if (policy.authority.require_provenance && !saw_provenance && check.reason == RefusalReason::None) {
        check.reason = RefusalReason::MissingEvidence;
        check.detail.assign("provenance evidence is required but not present");
    }

    // Authority bindings. These are checked before any classification outcome
    // can become actionable.
    if (check.reason == RefusalReason::None) {
        if (policy.authority.require_matching_boot && !bundle.boot.valid()) {
            check.reason = RefusalReason::BootMismatch;
            check.detail.assign("evidence carries no boot incarnation");
        } else if (policy.authority.require_valid_lease && !bundle.lease.valid()) {
            check.reason = RefusalReason::LeaseMissing;
            check.detail.assign("evidence carries no lease");
        } else if (policy.authority.require_valid_lease && bundle.lease_expiry.valid() && bundle.now.valid() &&
                   bundle.now > bundle.lease_expiry) {
            check.reason = RefusalReason::LeaseExpired;
            check.detail.assign("lease expired before the evaluation instant");
        } else if (policy.authority.require_matching_epoch && !bundle.epoch.valid()) {
            check.reason = RefusalReason::EpochMismatch;
            check.detail.assign("evidence carries no coordinator epoch");
        }
    }
    return check;
}

IncidentKind classify_instantaneous(const FanInMetrics& metrics,
                                    const SynchronizationEvidence& synchronization,
                                    const GovernorPolicy& policy) {
    if (!synchronization.evaluated) return IncidentKind::Unknown;
    if (metrics.active_senders < policy.synchronization.many_to_one_min_senders) {
        return IncidentKind::NoFanIn;
    }
    if (!synchronization.synchronized) {
        return IncidentKind::OrdinaryManyToOne;
    }

    const auto& pressure = policy.pressure;
    const bool collapse_rate = metrics.oversubscription_bp.value() >= pressure.collapse_oversubscription_bp;
    const bool saturated_queue = metrics.queue_pressure_bp.value() >= pressure.active_queue_pressure_bp;
    const bool dropping = metrics.drop_events >= pressure.drop_events_for_active;
    if (collapse_rate || saturated_queue || dropping) {
        return IncidentKind::ActiveIncast;
    }
    if (metrics.oversubscription_bp.value() >= pressure.risk_oversubscription_bp) {
        return IncidentKind::SynchronizedIncastRisk;
    }
    return IncidentKind::OrdinaryManyToOne;
}

Severity derive_severity(IncidentKind kind, const FanInMetrics& metrics, const GovernorPolicy& policy) {
    const auto& pressure = policy.pressure;
    switch (kind) {
        case IncidentKind::Unknown:
        case IncidentKind::NoFanIn:
            return Severity::None;
        case IncidentKind::OrdinaryManyToOne:
            return Severity::Advisory;
        case IncidentKind::Recovery:
            return Severity::Advisory;
        case IncidentKind::SynchronizedIncastRisk:
            return metrics.oversubscription_bp.value() >= pressure.collapse_oversubscription_bp
                       ? Severity::Severe
                       : Severity::Elevated;
        case IncidentKind::ActiveIncast: {
            const bool critical = metrics.drop_events >= pressure.drop_events_for_active ||
                                  metrics.oversubscription_bp.value() >= pressure.collapse_oversubscription_bp ||
                                  metrics.queue_pressure_bp.value() >= 9500;
            return critical ? Severity::Critical : Severity::Severe;
        }
    }
    return Severity::None;
}

std::vector<SenderContribution> rank_contributions(const EvidenceBundle& bundle,
                                                   const GovernorPolicy& policy,
                                                   const FanInMetrics& metrics,
                                                   const SynchronizationEvidence& synchronization,
                                                   const std::vector<SenderCollision>& collisions) {
    std::unordered_set<std::uint64_t> collided;
    collided.reserve(collisions.size() * 2 + 1);
    for (const auto& collision : collisions) collided.insert(collision.sender.value());

    std::vector<SenderContribution> contributions;
    const std::size_t count = std::min(bundle.senders.size(), kMaxSendersPerWindow);
    contributions.reserve(count);

    const std::int64_t limit =
        metrics.earliest_arrival.valid()
            ? metrics.earliest_arrival.nanos() + synchronization.sync_window_bound.nanos()
            : 0;

    std::unordered_set<std::uint64_t> seen_identity;
    seen_identity.reserve(count * 2 + 1);

    for (std::size_t index = 0; index < count; ++index) {
        const auto& sender = bundle.senders[index];
        if (!sender.identity.valid()) continue;
        // Duplicate (sender, incarnation, flow) rows are collapsed to the first
        // occurrence so one identity cannot be counted twice.
        const std::uint64_t key = sender.identity.sender.value() ^
                                  (sender.identity.incarnation.value() * 0x9E3779B97F4A7C15ULL) ^
                                  (sender.identity.flow.value() * 0xC2B2AE3D27D4EB4FULL);
        if (!seen_identity.insert(key).second) continue;

        SenderContribution contribution{};
        contribution.identity = sender.identity;
        contribution.service = sender.service;
        contribution.protected_obligation = is_protected_sender(sender, policy);
        contribution.collided = collided.count(sender.identity.sender.value()) != 0;
        contribution.offered_rate = sender.offered_rate;
        contribution.offered_share_bp =
            ratio_bp(sender.offered_rate.bytes_per_second(), metrics.aggregate_offered.bytes_per_second());
        contribution.aggregate_share_bp =
            ratio_bp(sender.offered_rate.bytes_per_second(), metrics.service_rate.bytes_per_second());
        if (sender.first_arrival.valid() && metrics.earliest_arrival.valid() &&
            sender.first_arrival >= metrics.earliest_arrival) {
            contribution.arrival_offset =
                Duration{sender.first_arrival.nanos() - metrics.earliest_arrival.nanos()};
        }
        contribution.synchronized =
            synchronization.synchronized && limit != 0 && sender.first_arrival.valid() &&
            sender.first_arrival.nanos() <= limit && sender.first_arrival.nanos() >= metrics.earliest_arrival.nanos();
        contribution.mitigable = contribution.synchronized && !contribution.protected_obligation &&
                                 !contribution.collided;

        if (contribution.collided) {
            contribution.disposition.assign("identity-collision: excluded from penalization");
        } else if (contribution.protected_obligation) {
            contribution.disposition.assign(
                sender.protected_obligation ? "protected-obligation: never penalized"
                                            : "protected-class: above the mitigable class ceiling");
        } else if (!contribution.synchronized) {
            contribution.disposition.assign("outside the synchronized arrival window");
        } else {
            contribution.disposition.assign("mitigable contributor");
        }
        contributions.push_back(contribution);
    }

    std::sort(contributions.begin(), contributions.end(),
              [](const SenderContribution& a, const SenderContribution& b) {
                  if (a.offered_rate != b.offered_rate) return a.offered_rate > b.offered_rate;
                  if (a.identity.incarnation != b.identity.incarnation) {
                      return a.identity.incarnation < b.identity.incarnation;
                  }
                  if (a.identity.flow != b.identity.flow) return a.identity.flow < b.identity.flow;
                  return a.identity.sender < b.identity.sender;
              });
    return contributions;
}

Classification classify(const EvidenceBundle& bundle,
                        const GovernorPolicy& policy,
                        std::vector<SenderCollision> collisions,
                        const ClassificationContext& context) {
    Classification classification{};
    classification.refusal = RefusalReason::None;
    classification.authoritative = true;

    FanInMetrics metrics = compute_metrics(bundle, policy);
    const SynchronizationEvidence synchronization = compute_synchronization(bundle, metrics, policy);
    classification.metrics = metrics;
    classification.synchronization = synchronization;

    {
        // A standalone classification never promotes refused evidence to an
        // incident. Ordering against previously accepted sequences is applied
        // by the governor, which owns that history; presence, freshness and
        // contradiction are decided here.
        const AuthorityCheck standalone = check_authority(bundle, policy, {});
        if (standalone.reason != RefusalReason::None) {
            classification.kind = IncidentKind::Unknown;
            classification.instantaneous_kind = IncidentKind::Unknown;
            classification.severity = Severity::None;
            classification.authoritative = false;
            classification.refusal = standalone.reason;
            classification.refusal_detail = standalone.detail;
            classification.notes.push_back(std::string("evidence refused: ") +
                                            std::string(to_string(standalone.reason)));
            return classification;
        }
    }
    if (!collisions.empty()) metrics.collisions = collisions;
    metrics.collided_senders = static_cast<std::uint32_t>(metrics.collisions.size());
    classification.metrics = metrics;

    const IncidentKind instantaneous = classify_instantaneous(metrics, synchronization, policy);
    classification.instantaneous_kind = instantaneous;
    IncidentKind effective = instantaneous;

    // Hysteresis: escalation is immediate, de-escalation requires a sustained
    // clean streak and steps down through Recovery instead of dropping straight
    // back to an ordinary classification.
    const int prior_rank = kind_rank(context.prior_kind);
    const int current_rank = kind_rank(instantaneous);
    if (prior_rank > current_rank && prior_rank > 0) {
        const bool dwell_met = (context.clean_streak + 1) >= policy.hysteresis.min_dwell_windows;
        if (!dwell_met) {
            effective = context.prior_kind;
            classification.hysteresis_held = true;
            classification.notes.emplace_back("de-escalation held: minimum dwell not satisfied");
        } else if (context.prior_kind == IncidentKind::ActiveIncast &&
                   current_rank < kind_rank(IncidentKind::Recovery)) {
            effective = IncidentKind::Recovery;
            classification.hysteresis_held = true;
            classification.notes.emplace_back("de-escalation stepped down to Recovery");
        } else if (current_rank > 0) {
            effective = instantaneous;
        }
    }

    classification.kind = effective;
    classification.severity = derive_severity(effective, metrics, policy);
    classification.contributions = rank_contributions(bundle, policy, metrics, synchronization, metrics.collisions);

    if (!classification.contributions.empty()) {
        classification.notes.push_back(synchronization.summary());
    }
    if (metrics.population_saturated) {
        classification.notes.emplace_back(
            "sender population saturated: counts and aggregate offer are lower bounds");
    }
    if (metrics.collided_senders > 0) {
        classification.notes.emplace_back("identity collisions detected: affected senders are not penalized");
    }
    if (!metrics.protected_offered.positive() && metrics.active_senders > 0) {
        classification.notes.emplace_back("no protected demand observed in this window");
    }
    return classification;
}

}  // namespace incast
