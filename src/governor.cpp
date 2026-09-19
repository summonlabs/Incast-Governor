// Incast Governor - governance engine implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "incast/governor.hpp"

#include <algorithm>
#include <utility>

namespace incast {
namespace {

[[nodiscard]] bool is_incident_kind(IncidentKind kind) noexcept {
    return kind == IncidentKind::SynchronizedIncastRisk || kind == IncidentKind::ActiveIncast;
}

[[nodiscard]] Intervention* find_live(std::vector<Intervention>& interventions) {
    for (auto& intervention : interventions) {
        if (intervention.live()) return &intervention;
    }
    return nullptr;
}

[[nodiscard]] const Intervention* find_live(const std::vector<Intervention>& interventions) {
    for (const auto& intervention : interventions) {
        if (intervention.live()) return &intervention;
    }
    return nullptr;
}

void trim_history(GovernorState& state, std::size_t max_history) {
    if (max_history == 0) {
        state.history.clear();
        return;
    }
    if (state.history.size() > max_history) {
        state.history.erase(state.history.begin(),
                            state.history.begin() + static_cast<std::ptrdiff_t>(state.history.size() - max_history));
    }
    if (state.history.size() > kMaxWindowHistory) {
        state.history.erase(state.history.begin(),
                            state.history.begin() +
                                static_cast<std::ptrdiff_t>(state.history.size() - kMaxWindowHistory));
    }
}

void trim_interventions(GovernorState& state) {
    if (state.interventions.size() <= kMaxInterventions) return;
    std::vector<Intervention> kept;
    kept.reserve(kMaxInterventions);
    // Live interventions are retained first, then the most recent terminal ones.
    for (const auto& intervention : state.interventions) {
        if (intervention.live()) kept.push_back(intervention);
    }
    for (auto iterator = state.interventions.rbegin();
         iterator != state.interventions.rend() && kept.size() < kMaxInterventions; ++iterator) {
        if (iterator->live()) continue;
        kept.push_back(*iterator);
    }
    state.interventions = std::move(kept);
}

}  // namespace

std::vector<const Intervention*> GovernorState::live_interventions() const {
    std::vector<const Intervention*> result;
    for (const auto& intervention : interventions) {
        if (intervention.live()) result.push_back(&intervention);
    }
    return result;
}

Status GovernorState::validate() const {
    if (history.size() > kMaxWindowHistory) {
        return fail(ErrorCode::CorruptState, "restored history exceeds the supported bound");
    }
    if (interventions.size() > kMaxInterventions) {
        return fail(ErrorCode::CorruptState, "restored intervention table exceeds the supported bound");
    }
    for (const auto& intervention : interventions) {
        if (!intervention.id.valid()) {
            return fail(ErrorCode::CorruptState, "restored intervention has an absent identity");
        }
        if (!intervention.intent.bounded) {
            return fail(ErrorCode::CorruptState, "restored intervention carries an unbounded intent");
        }
    }
    return ok_status();
}

Governor::Governor(GovernorConfig config) : config_(std::move(config)) {
    state_.destination = config_.destination;
    state_.destination_generation = config_.destination_generation;
    state_.policy = config_.policy.id;
    state_.policy_generation = config_.policy.generation;
    state_.epoch = config_.epoch;
    state_.boot = config_.boot;
    state_.next_intervention_generation = InterventionGeneration{1};
    state_.requires_revalidation = true;
}

Status Governor::configure_policy(const GovernorPolicy& policy, Timestamp now) {
    const Status validation = policy.validate();
    if (!validation) return validation;

    {
        const std::lock_guard<std::mutex> guard(mutex_);
        const bool generation_changed = state_.policy_generation != policy.generation ||
                                        state_.policy != policy.id;
        config_.policy = policy;
        state_.policy = policy.id;
        state_.policy_generation = policy.generation;
        if (generation_changed) {
            for (auto& intervention : state_.interventions) {
                if (!intervention.live()) continue;
                // A policy change invalidates live authority: interventions are
                // revalidated, never silently re-derived under the new policy.
                intervention.policy = policy.id;
                intervention.policy_generation = policy.generation;
                const Status moved = transition(intervention, InterventionState::Revalidating, now,
                                                "policy generation changed");
                if (!moved) {
                    intervention.state = InterventionState::Revalidating;
                    intervention.updated_at = now;
                }
            }
            state_.requires_revalidation = true;
        }
    }
    return ok_status();
}

GovernorPolicy Governor::policy() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return config_.policy;
}

DestinationId Governor::destination() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return config_.destination;
}

Status Governor::bind_authority(EpochId epoch,
                                BootIncarnation boot,
                                WorkerId worker,
                                LeaseId lease,
                                Timestamp lease_expiry) {
    if (!epoch.valid()) return fail(ErrorCode::InvalidArgument, "epoch is absent");
    if (!boot.valid()) return fail(ErrorCode::InvalidArgument, "boot incarnation is absent");
    if (!worker.valid()) return fail(ErrorCode::InvalidArgument, "worker identity is absent");
    if (!lease.valid()) return fail(ErrorCode::InvalidArgument, "lease identity is absent");
    if (!lease_expiry.valid()) return fail(ErrorCode::InvalidArgument, "lease expiry is absent");

    const std::lock_guard<std::mutex> guard(mutex_);
    config_.epoch = epoch;
    config_.boot = boot;
    config_.worker = worker;
    config_.lease = lease;
    config_.lease_expiry = lease_expiry;
    state_.epoch = epoch;
    state_.boot = boot;
    return ok_status();
}

Decision Governor::evaluate(const EvidenceBundle& bundle) {
    Decision decision;
    std::function<void(const Decision&)> observer;
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        decision = evaluate_locked(bundle);
        observer = observer_;
    }
    // The observer runs with no lock held: a progress callback can never
    // re-enter mutable governor state.
    if (observer) observer(decision);
    return decision;
}

GovernorState Governor::snapshot_state() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return state_;
}

Status Governor::restore_state(const GovernorState& state) {
    const Status validation = state.validate();
    if (!validation) return validation;

    const std::lock_guard<std::mutex> guard(mutex_);
    state_ = state;
    // Durable state never restores liveness, authority, freshness or leases.
    // The live binding is cleared as well, so a restored governor refuses every
    // decision until bind_authority re-establishes authority from the
    // coordinator.
    config_.epoch = EpochId{};
    config_.boot = BootIncarnation{};
    config_.worker = WorkerId{};
    config_.lease = LeaseId{};
    config_.lease_expiry = Timestamp{};
    state_.epoch = EpochId{};
    state_.boot = BootIncarnation{};
    state_.recovery.reset();
    state_.incident_streak = 0;
    state_.clean_streak = 0;
    state_.requires_revalidation = true;
    state_.restored_from_durable = true;
    state_.restore_count += 1;
    for (auto& intervention : state_.interventions) {
        if (!intervention.live()) continue;
        if (intervention.state != InterventionState::Revalidating) {
            intervention.state = InterventionState::Revalidating;
        }
        intervention.updated_at = state_.last_evaluation;
    }
    state_.generation = StateGeneration{state_.generation.value() + 1};
    return ok_status();
}

void Governor::reset() {
    const std::lock_guard<std::mutex> guard(mutex_);
    state_ = GovernorState{};
    state_.destination = config_.destination;
    state_.destination_generation = config_.destination_generation;
    state_.policy = config_.policy.id;
    state_.policy_generation = config_.policy.generation;
    state_.epoch = config_.epoch;
    state_.boot = config_.boot;
    state_.next_intervention_generation = InterventionGeneration{1};
    state_.requires_revalidation = true;
    accepting_ = true;
}

std::vector<Intervention> Governor::interventions() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return state_.interventions;
}

Status Governor::retire(InterventionId id, Timestamp now, std::string reason) {
    const std::lock_guard<std::mutex> guard(mutex_);
    for (auto& intervention : state_.interventions) {
        if (intervention.id != id) continue;
        if (is_terminal_state(intervention.state)) {
            return fail(ErrorCode::IllegalTransition, "intervention is already terminal");
        }
        const Status moved = transition(intervention, InterventionState::Retired, now, std::move(reason));
        if (!moved) return moved;
        state_.generation = StateGeneration{state_.generation.value() + 1};
        return ok_status();
    }
    return fail(ErrorCode::NotFound, "no such intervention");
}

Status Governor::shutdown(Timestamp now, std::size_t& retired_count) {
    const std::lock_guard<std::mutex> guard(mutex_);
    accepting_ = false;
    retired_count = 0;
    for (auto& intervention : state_.interventions) {
        if (!intervention.live()) continue;
        // Every live state can legally reach Retired; the forced assignment is
        // a deliberate shutdown path, not a silent state corruption.
        const Status moved = transition(intervention, InterventionState::Retired, now, "governor shutdown");
        if (!moved) {
            intervention.state = InterventionState::Retired;
            intervention.updated_at = now;
        }
        retired_count += 1;
    }
    state_.recovery.reset();
    state_.incident_streak = 0;
    state_.clean_streak = 0;
    state_.generation = StateGeneration{state_.generation.value() + 1};
    return ok_status();
}

void Governor::set_decision_observer(std::function<void(const Decision&)> observer) {
    const std::lock_guard<std::mutex> guard(mutex_);
    observer_ = std::move(observer);
}

std::size_t Governor::max_history() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return config_.max_history;
}

Decision Governor::evaluate_locked(const EvidenceBundle& bundle) {
    Decision decision{};
    const Timestamp now = bundle.now.valid() ? bundle.now : state_.last_evaluation;
    decision.decided_at = now;
    decision.event = bundle.event;
    decision.event_generation = bundle.event_generation;
    decision.destination = bundle.destination;
    decision.destination_generation = bundle.destination_generation;
    decision.window = bundle.window;
    decision.window_generation = bundle.window_generation;
    decision.policy = config_.policy.id;
    decision.policy_generation = config_.policy.generation;
    decision.epoch = config_.epoch;
    decision.boot = config_.boot;
    decision.worker = config_.worker;
    decision.lease = config_.lease;

    const auto refuse = [&](RefusalReason reason, std::string detail) {
        decision.outcome = DecisionOutcome::Refused;
        decision.refusal = reason;
        decision.refusal_detail = std::move(detail);
        decision.kind = IncidentKind::Unknown;
        decision.severity = Severity::None;
        decision.authority.refusal = reason;
        decision.authority.refusal_detail = decision.refusal_detail;
        decision.authority.intervention_authorized = false;
        state_.refusal_count += 1;
        state_.generation = StateGeneration{state_.generation.value() + 1};
        decision.state_generation = state_.generation;
        decision.explanation = build_explanation(decision, config_.max_contributions_in_decision);
        return decision;
    };

    if (!accepting_) {
        return refuse(RefusalReason::PolicyDisabled, "governor is shut down and no longer accepts work");
    }
    // No decision is authoritative until authority has been explicitly bound.
    // A restored governor starts unbound; durable state never restores it.
    if (!config_.epoch.valid() || !config_.boot.valid() || !config_.lease.valid()) {
        return refuse(RefusalReason::LeaseMissing,
                      "governor holds no bound authority; bind_authority must be called first");
    }
    if (!bundle.structurally_valid()) {
        return refuse(RefusalReason::MalformedBundle, "evidence bundle failed structural validation");
    }
    if (config_.destination.valid() && bundle.destination != config_.destination) {
        return refuse(RefusalReason::DestinationMismatch, "evidence is bound to a different destination");
    }
    if (state_.destination_generation.valid() && bundle.destination_generation.valid() &&
        bundle.destination_generation < state_.destination_generation) {
        return refuse(RefusalReason::StaleEvidence, "destination generation regressed");
    }
    if (bundle.policy_stamp.state == EvidenceState::Contradictory) {
        return refuse(RefusalReason::PolicyGenerationMismatch, "policy evidence is contradictory");
    }
    if (config_.epoch.valid() && (!bundle.epoch.valid() || bundle.epoch != config_.epoch)) {
        return refuse(RefusalReason::EpochMismatch, "evidence epoch does not match the bound authority");
    }
    if (config_.boot.valid() && (!bundle.boot.valid() || bundle.boot != config_.boot)) {
        return refuse(RefusalReason::BootMismatch, "evidence boot incarnation does not match this incarnation");
    }
    if (config_.worker.valid() && bundle.worker.valid() && bundle.worker != config_.worker) {
        return refuse(RefusalReason::EpochMismatch, "evidence was produced by a different worker");
    }
    if (config_.lease.valid() && (!bundle.lease.valid() || bundle.lease != config_.lease)) {
        return refuse(RefusalReason::LeaseMissing, "evidence lease does not match the bound lease");
    }
    if (config_.lease_expiry.valid() && now.valid() && now > config_.lease_expiry) {
        return refuse(RefusalReason::LeaseExpired, "authority lease expired before this evaluation");
    }

    // Evidence ordering is only meaningful inside one source incarnation: a
    // restarted worker legitimately restarts its per-class sequence space, and
    // the epoch/lease binding already fences the previous incarnation.
    const bool same_source_incarnation =
        state_.last_sequence_boot.valid() && bundle.boot.valid() && state_.last_sequence_boot == bundle.boot;
    static const std::array<std::uint64_t, kEvidenceClassCount> kNoSequences{};
    const auto& reference_sequences = same_source_incarnation ? state_.last_sequence : kNoSequences;
    const AuthorityCheck check = check_authority(bundle, config_.policy, reference_sequences);
    decision.authority.classes = check.classes;
    decision.authority.policy_enabled = config_.policy.enabled;
    decision.authority.event_bound = bundle.event.valid() && bundle.event_generation.valid();
    decision.authority.destination_bound = bundle.destination.valid() && bundle.destination_generation.valid();
    decision.authority.window_bound = bundle.window.valid() && bundle.window_generation.valid();
    decision.authority.epoch_bound = bundle.epoch.valid() && bundle.epoch == config_.epoch;
    decision.authority.boot_bound = bundle.boot.valid() && bundle.boot == config_.boot;
    decision.authority.lease_bound = bundle.lease.valid() && bundle.lease == config_.lease;
    decision.authority.provenance_bound = bundle.provenance_stamp.state == EvidenceState::Present &&
                                          bundle.provenance_stamp.provenance.valid();
    decision.authority.recovery_policy_present = config_.policy.recovery.enabled;
    if (check.reason != RefusalReason::None) {
        return refuse(check.reason, check.detail);
    }

    // Evidence passed validation: advance the accepted sequences so a later
    // replayed frame from the same incarnation is refused as reordered.
    if (!same_source_incarnation) {
        state_.last_sequence.fill(0);
        state_.last_sequence_boot = bundle.boot;
    }
    for (std::size_t index = 0; index < kEvidenceClassCount; ++index) {
        const EvidenceStamp* stamp = nullptr;
        switch (static_cast<EvidenceClass>(index)) {
            case EvidenceClass::FanIn: stamp = &bundle.fan_in_stamp; break;
            case EvidenceClass::ArrivalTiming: stamp = &bundle.timing_stamp; break;
            case EvidenceClass::AggregateRate: stamp = &bundle.rate_stamp; break;
            case EvidenceClass::Destination:
            case EvidenceClass::Capacity: stamp = &bundle.capacity_stamp; break;
            case EvidenceClass::Queue: stamp = &bundle.queue_stamp; break;
            case EvidenceClass::Service: stamp = &bundle.service_stamp; break;
            case EvidenceClass::Provenance: stamp = &bundle.provenance_stamp; break;
        }
        if (stamp != nullptr && stamp->sequence > state_.last_sequence[index]) {
            state_.last_sequence[index] = stamp->sequence;
        }
    }

    const auto collisions = detect_collisions(bundle);

    Intervention* live = find_live(state_.interventions);
    ClassificationContext context{};
    context.prior_kind = state_.history.empty() ? IncidentKind::Unknown : state_.history.back().kind;
    context.incident_streak = state_.incident_streak;
    context.clean_streak = state_.clean_streak;

    Classification classification = classify(bundle, config_.policy, collisions, context);
    // With a live intervention in place, a return to ordinary traffic is a
    // recovery: the recovery policy, not the classifier, governs relaxation.
    // Hysteresis still wins: while the classifier is holding a de-escalation
    // the window stays classified as an incident and nothing relaxes.
    if (live != nullptr && classification.authoritative && !classification.hysteresis_held &&
        !is_incident_kind(classification.kind)) {
        classification.kind = IncidentKind::Recovery;
    }
    decision.kind = classification.kind;
    decision.severity = classification.severity;
    decision.metrics = classification.metrics;
    decision.synchronization = classification.synchronization;
    decision.contributions = classification.contributions;
    if (decision.contributions.size() > config_.max_contributions_in_decision) {
        decision.contributions.resize(config_.max_contributions_in_decision);
    }
    decision.authority.population_saturated = classification.metrics.population_saturated;
    decision.authority.identity_collision_present = classification.metrics.collided_senders > 0;

    // Streak accounting drives hysteresis and recovery dwell.
    if (is_incident_kind(classification.instantaneous_kind)) {
        state_.incident_streak = std::min<std::uint32_t>(
            state_.incident_streak + 1, config_.policy.hysteresis.max_incident_windows);
        state_.clean_streak = 0;
        state_.recovery.reset();
    } else {
        state_.clean_streak = std::min<std::uint32_t>(
            state_.clean_streak + 1, config_.policy.hysteresis.max_clean_windows);
        state_.incident_streak = 0;
        if (live != nullptr) {
            state_.recovery.tracking = true;
            if (!state_.recovery.since.valid()) state_.recovery.since = now;
            state_.recovery.clean_windows = state_.clean_streak;
            state_.recovery.drops_during_recovery =
                saturating_add(state_.recovery.drops_during_recovery, classification.metrics.drop_events);
            state_.recovery.capacity_revalidated = bundle.capacity_stamp.state == EvidenceState::Present;
        }
    }

    FenceContext fence{};
    fence.epoch = config_.epoch;
    fence.boot = config_.boot;
    fence.worker = config_.worker;
    fence.lease = config_.lease;
    fence.now = now;
    fence.lease_expiry = config_.lease_expiry;

    if (live != nullptr) {
        RefusalReason fenced_reason = RefusalReason::None;
        std::string fenced_detail;
        if (is_fenced(*live, fence, fenced_reason, fenced_detail)) {
            const Status moved = transition(*live, InterventionState::Fenced, live->updated_at, fenced_detail);
            if (!moved) {
                live->state = InterventionState::Fenced;
                live->updated_at = now;
            }
            decision.outcome = DecisionOutcome::InterventionFenced;
            decision.intervention = live->id;
            decision.intervention_state = live->state;
            decision.refusal = fenced_reason;
            decision.refusal_detail = std::move(fenced_detail);
            decision.authority.intervention_authorized = false;
            decision.authority.refusal = decision.refusal;
            decision.authority.refusal_detail = decision.refusal_detail;
            state_.generation = StateGeneration{state_.generation.value() + 1};
            decision.state_generation = state_.generation;
            state_.last_evaluation = now;
            state_.evaluation_count += 1;
            state_.requires_revalidation = true;
            decision.explanation = build_explanation(decision, config_.max_contributions_in_decision);
            return decision;
        }
        // Expiry is a revalidation boundary, never a silent relaxation.
        if (live->expires_at.valid() && now.valid() && now > live->expires_at &&
            live->state != InterventionState::Revalidating) {
            const Status moved = transition(*live, InterventionState::Revalidating, now,
                                            "intervention reached its expiry boundary");
            if (!moved) live->state = InterventionState::Revalidating;
        }
    }

    // Hysteresis holds a live intervention at its current magnitude. Recomputing
    // the intent from a window the classifier does not yet trust is exactly the
    // oscillation the dwell rule exists to prevent.
    bool hysteresis_hold = false;
    if (live != nullptr && classification.hysteresis_held && classification.authoritative &&
        is_incident_kind(classification.kind) &&
        (live->state == InterventionState::Active || live->state == InterventionState::Relaxing)) {
        hysteresis_hold = true;
        live->expires_at = Timestamp{now.nanos() + live->intent.duration.nanos()};
        live->intent.expires_at = live->expires_at;
        live->updated_at = now;
        live->continue_steps += 1;
        decision.outcome = DecisionOutcome::InterventionContinued;
        decision.intervention = live->id;
        decision.intervention_state = live->state;
        decision.intent = live->intent;
        decision.authority.intervention_authorized = true;
        decision.authority.refusal = RefusalReason::None;
        decision.authority.refusal_detail.clear();
        decision.authority.classes = check.classes;
        decision.authority.recovery_policy_present = config_.policy.recovery.enabled;
        decision.explanation.notes.emplace_back(
            "de-escalation held by hysteresis: intervention magnitude is unchanged");
    }

    RecoveryVerdict recovery_verdict{};
    if (classification.kind == IncidentKind::Recovery && live != nullptr) {
        recovery_verdict = evaluate_recovery(config_.policy.recovery, state_.recovery,
                                             classification.metrics, config_.policy.recovery.enabled);
    }

    SynthesisRequest request{};
    request.bundle = &bundle;
    request.policy = &config_.policy;
    request.classification = &classification;
    request.check = check;
    request.issued_at = now;
    request.prior = live != nullptr ? &live->intent : nullptr;
    request.recovery_relaxation_permitted = recovery_verdict.relax;
    request.recovery_step_bp = recovery_verdict.step_bp;

    if (live == nullptr) {
        // A new intervention always receives a fresh identity and generation.
        request.intervention = InterventionId{state_.next_intervention_generation.value()};
        request.generation = state_.next_intervention_generation;
    } else {
        request.intervention = live->id;
        request.generation = live->generation;
    }

    if (!hysteresis_hold) {
        const SynthesisResult synthesis = synthesize(request);
        decision.authority = synthesis.authority;
        decision.intent = synthesis.intent;
        decision.authority.classes = check.classes;

        for (const auto& note : synthesis.notes) {
            if (decision.explanation.notes.size() >= config_.max_notes) break;
            decision.explanation.notes.push_back(note);
        }

        const bool wants_bounded_action = classification.kind == IncidentKind::SynchronizedIncastRisk ||
                                          classification.kind == IncidentKind::ActiveIncast;

        if (wants_bounded_action && synthesis.authority.intervention_authorized && synthesis.intent.active()) {
            if (live == nullptr) {
                Intervention created{};
                created.id = synthesis.intent.intervention;
                created.generation = synthesis.intent.generation;
                created.destination = bundle.destination;
                created.destination_generation = bundle.destination_generation;
                created.window = bundle.window;
                created.window_generation = bundle.window_generation;
                created.epoch = bundle.epoch;
                created.boot = bundle.boot;
                created.worker = bundle.worker;
                created.lease = bundle.lease;
                created.policy = config_.policy.id;
                created.policy_generation = config_.policy.generation;
                created.created_at = now;
                created.updated_at = now;
                created.expires_at = synthesis.intent.expires_at;
                created.applied_reduction_bp = synthesis.intent.reduction_bp;
                created.intent = synthesis.intent;
                created.state = InterventionState::Proposed;
                static_cast<void>(transition(created, InterventionState::Authorized, now, {}));
                static_cast<void>(transition(created, InterventionState::Active, now, {}));
                created.state = InterventionState::Active;
                state_.interventions.push_back(created);
                state_.next_intervention_generation =
                    InterventionGeneration{state_.next_intervention_generation.value() + 1};
                state_.intervention_count += 1;
                decision.outcome = DecisionOutcome::InterventionIssued;
                decision.intervention = created.id;
                decision.intervention_state = created.state;
                decision.intent = created.intent;
            } else {
                const bool revalidating = live->state == InterventionState::Revalidating;
                const bool relaxing = live->state == InterventionState::Relaxing;
                if (revalidating) {
                    static_cast<void>(transition(*live, InterventionState::Active, now, "revalidated"));
                    live->state = InterventionState::Active;
                    live->generation = InterventionGeneration{live->generation.value() + 1};
                } else if (relaxing) {
                    static_cast<void>(transition(*live, InterventionState::Active, now, "re-escalated"));
                    live->state = InterventionState::Active;
                }
                live->intent = synthesis.intent;
                live->intent.generation = live->generation;
                live->applied_reduction_bp = synthesis.intent.reduction_bp;
                live->expires_at = synthesis.intent.expires_at;
                live->updated_at = now;
                live->continue_steps += 1;
                decision.outcome = DecisionOutcome::InterventionContinued;
                decision.intervention = live->id;
                decision.intervention_state = live->state;
                decision.intent = live->intent;
            }
        } else if (classification.kind == IncidentKind::Recovery && live != nullptr) {
            if (synthesis.authority.intervention_authorized) {
                if (!synthesis.intent.active()) {
                    static_cast<void>(transition(*live, InterventionState::Relaxing, now, "recovery retired"));
                    live->state = InterventionState::Relaxing;
                    static_cast<void>(transition(*live, InterventionState::Retired, now, "recovery complete"));
                    live->state = InterventionState::Retired;
                    live->updated_at = now;
                    live->intent = synthesis.intent;
                    decision.outcome = DecisionOutcome::InterventionRetired;
                } else {
                    if (live->state == InterventionState::Active) {
                        static_cast<void>(transition(*live, InterventionState::Relaxing, now, "recovery step"));
                        live->state = InterventionState::Relaxing;
                    }
                    live->intent = synthesis.intent;
                    live->applied_reduction_bp = synthesis.intent.reduction_bp;
                    live->expires_at = synthesis.intent.expires_at;
                    live->updated_at = now;
                    decision.outcome = DecisionOutcome::InterventionRelaxed;
                }
                decision.intervention = live->id;
                decision.intervention_state = live->state;
            } else {
                decision.outcome = DecisionOutcome::NoAction;
                decision.refusal = synthesis.authority.refusal;
                decision.refusal_detail = synthesis.authority.refusal_detail;
                decision.intervention = live->id;
                decision.intervention_state = live->state;
            }
        } else {
            decision.outcome = DecisionOutcome::NoAction;
            decision.refusal = synthesis.authority.refusal;
            decision.refusal_detail = synthesis.authority.refusal_detail;
            if (live != nullptr) {
                decision.intervention = live->id;
                decision.intervention_state = live->state;
                if (!is_incident_kind(classification.kind) && classification.authoritative &&
                    live->state == InterventionState::Revalidating) {
                    static_cast<void>(transition(*live, InterventionState::Retired, now,
                                                 "revalidation found no incident"));
                    live->state = InterventionState::Retired;
                    live->updated_at = now;
                    decision.outcome = DecisionOutcome::InterventionRetired;
                    decision.intervention_state = live->state;
                }
            }
        }
    }  // !hysteresis_hold

    WindowRecord record{};
    record.window = bundle.window;
    record.window_generation = bundle.window_generation;
    record.start = bundle.window_start;
    record.end = bundle.window_end;
    record.kind = classification.kind;
    record.severity = classification.severity;
    record.oversubscription_bp = classification.metrics.oversubscription_bp;
    record.queue_pressure_bp = classification.metrics.queue_pressure_bp;
    record.active_senders = classification.metrics.active_senders;
    record.authoritative = classification.authoritative;
    state_.history.push_back(record);
    trim_history(state_, config_.max_history);

    if (bundle.destination_generation.valid() &&
        bundle.destination_generation > state_.destination_generation) {
        state_.destination_generation = bundle.destination_generation;
    }
    state_.last_evaluation = now;
    state_.evaluation_count += 1;
    state_.generation = StateGeneration{state_.generation.value() + 1};
    state_.requires_revalidation = false;
    for (const auto& intervention : state_.interventions) {
        if (intervention.state == InterventionState::Revalidating) {
            state_.requires_revalidation = true;
            break;
        }
    }
    decision.state_generation = state_.generation;
    trim_interventions(state_);

    for (const auto& note : classification.notes) {
        if (decision.explanation.notes.size() >= config_.max_notes) break;
        decision.explanation.notes.push_back(note);
    }
    decision.explanation = build_explanation(decision, classification.contributions,
                                             config_.max_contributions_in_decision);
    return decision;
}

}  // namespace incast
