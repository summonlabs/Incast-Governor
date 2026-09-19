// Incast Governor - mitigation intent, authority and lifecycle tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "governor_fixture.hpp"
#include "incast/lifecycle.hpp"
#include "incast/mitigate.hpp"
#include "incast/synthetic.hpp"
#include "test_framework.hpp"

using namespace incast;

namespace {

SynthesisResult run_synthesis(const synthetic::FanInSpec& spec,
                              const GovernorPolicy& policy,
                              bool recovery_enabled = false,
                              const MitigationIntent* prior = nullptr) {
    const auto bundle = synthetic::build(spec);
    const auto collisions = detect_collisions(bundle);
    const Classification classification = classify(bundle, policy, collisions);
    const auto check = check_authority(bundle, policy, {});

    SynthesisRequest request{};
    request.bundle = &bundle;
    request.policy = &policy;
    request.classification = &classification;
    request.check = check;
    request.intervention = InterventionId{1};
    request.generation = InterventionGeneration{1};
    request.issued_at = spec.now;
    request.prior = prior;
    request.recovery_relaxation_permitted = recovery_enabled;
    return synthesize(request);
}

}  // namespace

IG_TEST(mitigate, every_intent_component_is_bounded) {
    auto spec = synthetic::synchronized_burst(512);
    spec.service_rate = Rate{1'000'000'000ULL};   // extreme oversubscription
    spec.queue_pressure_bp = 10'000;
    spec.per_sender_rate = Rate{100'000'000'000ULL};
    spec.outstanding_per_sender = Bytes{64u << 20};
    spec.buffer_capacity = Bytes{1u << 20};

    const auto policy = make_default_policy();
    const SynthesisResult result = run_synthesis(spec, policy);
    IG_REQUIRE(result.authority.intervention_authorized);
    const auto& intent = result.intent;

    IG_CHECK(intent.bounded);
    IG_CHECK(intent.reduction_bp.value() <= policy.bounds.max_reduction_bp);
    IG_CHECK(intent.admission_reduction_bp.value() <= policy.bounds.max_admission_reduction_bp);
    IG_CHECK(intent.headroom_bytes.count() <= policy.bounds.max_headroom_bytes.count());
    IG_CHECK(intent.stagger_slots <= policy.bounds.max_stagger_slots);
    IG_CHECK(intent.penalized_senders.size() <= policy.bounds.max_penalized_senders);
    IG_CHECK(intent.duration.nanos() >= policy.bounds.min_duration.nanos());
    IG_CHECK(intent.duration.nanos() <= policy.bounds.max_duration.nanos());
    IG_CHECK(intent.expires_at > intent.issued_at);
}

IG_TEST(mitigate, protected_senders_are_never_penalized) {
    auto spec = synthetic::synchronized_burst(64);
    spec.protected_senders = 24;
    const auto policy = make_default_policy();
    const auto bundle = synthetic::build(spec);
    const auto collisions = detect_collisions(bundle);
    const Classification classification = classify(bundle, policy, collisions);
    const SynthesisResult result = run_synthesis(spec, policy);

    IG_REQUIRE(result.authority.intervention_authorized);
    IG_CHECK(result.authority.protected_floor_preserved);
    for (const auto& identity : result.intent.penalized_senders) {
        for (const auto& contribution : classification.contributions) {
            if (contribution.identity == identity) IG_CHECK(!contribution.protected_obligation);
        }
    }
}

IG_TEST(mitigate, protected_floor_conflict_refuses_when_escalation_is_disabled) {
    auto spec = synthetic::synchronized_burst(32);
    spec.protected_senders = 32;    // no mitigable population at all
    spec.service_rate = Rate{4'000'000'000ULL};
    spec.queue_pressure_bp = 9'900;
    spec.drops = 1'024;

    GovernorPolicy policy = make_default_policy();
    policy.protection.escalate_instead_of_violating_floor = false;
    const SynthesisResult result = run_synthesis(spec, policy);
    IG_CHECK(!result.authority.intervention_authorized);
    IG_CHECK(result.authority.refusal == RefusalReason::ProtectedFloorConflict);
    IG_CHECK(!result.intent.active());
}

IG_TEST(mitigate, protected_floor_conflict_escalates_when_policy_allows) {
    auto spec = synthetic::synchronized_burst(32);
    spec.protected_senders = 32;
    spec.service_rate = Rate{4'000'000'000ULL};
    spec.queue_pressure_bp = 9'900;
    spec.drops = 1'024;

    const auto policy = make_default_policy();
    const SynthesisResult result = run_synthesis(spec, policy);
    IG_REQUIRE(result.authority.intervention_authorized);
    IG_CHECK(result.intent.escalating);
    IG_CHECK(result.authority.protected_floor_preserved);
    IG_CHECK(result.intent.penalized_senders.empty());
}

IG_TEST(mitigate, saturated_population_forbids_sender_scoped_penalization) {
    auto spec = synthetic::synchronized_burst(64);
    spec.omit_senders = 32;
    spec.mark_population_saturated = true;
    spec.service_rate = Rate{8'000'000'000ULL};   // a reduction is genuinely required
    const auto policy = make_default_policy();
    const SynthesisResult result = run_synthesis(spec, policy);

    IG_REQUIRE(result.authority.intervention_authorized);
    IG_CHECK(result.authority.population_saturated);
    IG_CHECK(result.intent.penalized_senders.empty());
    IG_CHECK(result.intent.scope == MitigationScope::Destination);
    IG_CHECK(result.intent.kind == MitigationKind::TemporaryAdmissionReduction);
    IG_CHECK(result.intent.insufficient);
}

IG_TEST(mitigate, identity_collision_excludes_the_colliding_sender) {
    auto spec = synthetic::synchronized_burst(32);
    spec.duplicate_senders = 3;
    const auto policy = make_default_policy();
    const SynthesisResult result = run_synthesis(spec, policy);

    IG_REQUIRE(result.authority.intervention_authorized);
    IG_CHECK(result.authority.identity_collision_present);
    for (const auto& identity : result.intent.penalized_senders) {
        IG_CHECK(identity.sender != SenderId{1});
    }
}

IG_TEST(mitigate, unsynchronized_demand_never_authorizes_incast_mitigation) {
    const auto spec = synthetic::unsynchronized_many_to_one(256);
    const auto policy = make_default_policy();
    const SynthesisResult result = run_synthesis(spec, policy);
    IG_CHECK(!result.authority.intervention_authorized);
    IG_CHECK(result.authority.refusal == RefusalReason::NoIncident);
    IG_CHECK(!result.intent.active());
}

IG_TEST(mitigate, unknown_evidence_never_authorizes_intervention) {
    const auto spec = synthetic::telemetry_gap(128);
    const auto policy = make_default_policy();
    const SynthesisResult result = run_synthesis(spec, policy);
    IG_CHECK(!result.authority.intervention_authorized);
    IG_CHECK(result.authority.refusal == RefusalReason::MissingEvidence);
    IG_CHECK(!result.intent.active());
}

IG_TEST(mitigate, recovery_without_a_policy_holds_the_intervention) {
    auto relaxation = synthetic::unsynchronized_many_to_one(64);
    GovernorPolicy policy = make_default_policy();

    MitigationIntent prior{};
    prior.kind = MitigationKind::PriorityAwareRateReduction;
    prior.scope = MitigationScope::SenderSet;
    prior.reduction_bp = BasisPoints{3'000};
    prior.bounded = true;

    // The classification is not a Recovery here, so synthesis reports NoIncident;
    // the explicit recovery gate is exercised below through a Recovery window.
    const auto result = run_synthesis(relaxation, policy, false, &prior);
    IG_CHECK(!result.authority.intervention_authorized);
    IG_CHECK(result.authority.refusal == RefusalReason::NoIncident);
}

IG_TEST(mitigate, recovery_step_relaxes_only_under_an_enabled_policy) {
    auto spec = synthetic::unsynchronized_many_to_one(64);
    GovernorPolicy enabled = make_default_policy();
    enabled.recovery.enabled = true;

    const auto bundle = synthetic::build(spec);
    const auto collisions = detect_collisions(bundle);
    Classification classification = classify(bundle, enabled, collisions);
    classification.kind = IncidentKind::Recovery;   // the governor promotes calm windows with a live intent

    MitigationIntent prior{};
    prior.kind = MitigationKind::PriorityAwareRateReduction;
    prior.scope = MitigationScope::SenderSet;
    prior.reduction_bp = BasisPoints{4'000};
    prior.bounded = true;
    prior.duration = enabled.bounds.max_duration;
    prior.penalized_senders.push_back(SenderIdentity{SenderId{1}, BootIncarnation{1}, FlowId{1}});

    const auto check = check_authority(bundle, enabled, {});
    SynthesisRequest request{};
    request.bundle = &bundle;
    request.policy = &enabled;
    request.classification = &classification;
    request.check = check;
    request.intervention = InterventionId{9};
    request.generation = InterventionGeneration{4};
    request.issued_at = spec.now;
    request.prior = &prior;
    request.recovery_relaxation_permitted = true;

    const SynthesisResult result = synthesize(request);
    IG_REQUIRE(result.authority.intervention_authorized);
    IG_CHECK(result.intent.active());
    IG_CHECK(result.intent.relaxes_prior);
    IG_CHECK(result.intent.reduction_bp.value() < prior.reduction_bp.value());

    // The same request with the recovery policy disabled holds instead.
    GovernorPolicy disabled = make_default_policy();
    request.policy = &disabled;
    const SynthesisResult held = synthesize(request);
    IG_CHECK(!held.authority.intervention_authorized);
    IG_CHECK(held.authority.refusal == RefusalReason::AwaitingRecoveryPolicy);
}

IG_TEST(mitigate, repeated_recovery_steps_eventually_retire_the_intent) {
    auto spec = synthetic::unsynchronized_many_to_one(64);
    GovernorPolicy enabled = make_default_policy();
    enabled.recovery.enabled = true;
    const auto bundle = synthetic::build(spec);
    const auto collisions = detect_collisions(bundle);
    Classification classification = classify(bundle, enabled, collisions);
    classification.kind = IncidentKind::Recovery;
    const auto check = check_authority(bundle, enabled, {});

    MitigationIntent prior{};
    prior.kind = MitigationKind::PriorityAwareRateReduction;
    prior.scope = MitigationScope::SenderSet;
    prior.reduction_bp = BasisPoints{1'000};
    prior.bounded = true;
    prior.duration = enabled.bounds.max_duration;

    SynthesisRequest request{};
    request.bundle = &bundle;
    request.policy = &enabled;
    request.classification = &classification;
    request.check = check;
    request.intervention = InterventionId{11};
    request.generation = InterventionGeneration{1};
    request.issued_at = spec.now;
    request.prior = &prior;
    request.recovery_relaxation_permitted = true;

    SynthesisResult result = synthesize(request);
    IG_REQUIRE(result.authority.intervention_authorized);
    int steps = 0;
    while (result.intent.active() && steps < 64) {
        request.prior = &result.intent;
        result = synthesize(request);
        IG_REQUIRE(result.authority.intervention_authorized);
        steps += 1;
    }
    IG_CHECK(result.intent.kind == MitigationKind::None);
    IG_CHECK(result.intent.relaxes_prior);
    IG_CHECK(steps > 1);
    IG_CHECK(steps < 64);
}

IG_TEST(lifecycle, transition_table_rejects_illegal_moves) {
    IG_CHECK(is_legal_transition(InterventionState::Proposed, InterventionState::Authorized));
    IG_CHECK(is_legal_transition(InterventionState::Active, InterventionState::Relaxing));
    IG_CHECK(is_legal_transition(InterventionState::Revalidating, InterventionState::Active));
    IG_CHECK(!is_legal_transition(InterventionState::Retired, InterventionState::Active));
    IG_CHECK(!is_legal_transition(InterventionState::Refused, InterventionState::Active));
    IG_CHECK(!is_legal_transition(InterventionState::Proposed, InterventionState::Active));

    Intervention intervention{};
    intervention.state = InterventionState::Proposed;
    IG_CHECK(!transition(intervention, InterventionState::Active, Timestamp{1}, {}).has_value());
    IG_CHECK(intervention.state == InterventionState::Proposed);
    IG_REQUIRE(static_cast<bool>(transition(intervention, InterventionState::Authorized, Timestamp{2}, {})));
    IG_REQUIRE(static_cast<bool>(transition(intervention, InterventionState::Active, Timestamp{3}, {})));
    IG_CHECK_EQ(intervention.updated_at.nanos(), 3);
    IG_REQUIRE(static_cast<bool>(transition(intervention, InterventionState::Relaxing, Timestamp{4}, {})));
    IG_CHECK_EQ(intervention.relax_steps, 1u);
}

IG_TEST(lifecycle, fencing_rejects_stale_epoch_boot_and_lease) {
    Intervention intervention{};
    intervention.epoch = EpochId{4};
    intervention.boot = BootIncarnation{7};
    intervention.worker = WorkerId{1};
    intervention.lease = LeaseId{2};
    intervention.expires_at = Timestamp{1'000};

    FenceContext context{};
    context.epoch = EpochId{4};
    context.boot = BootIncarnation{7};
    context.worker = WorkerId{1};
    context.lease = LeaseId{2};
    context.now = Timestamp{500};
    context.lease_expiry = Timestamp{900};

    RefusalReason reason = RefusalReason::None;
    std::string detail;
    IG_CHECK(!is_fenced(intervention, context, reason, detail));

    context.epoch = EpochId{5};
    IG_CHECK(is_fenced(intervention, context, reason, detail));
    IG_CHECK(reason == RefusalReason::EpochMismatch);

    context.epoch = EpochId{4};
    context.boot = BootIncarnation{8};
    IG_CHECK(is_fenced(intervention, context, reason, detail));
    IG_CHECK(reason == RefusalReason::BootMismatch);

    context.boot = BootIncarnation{7};
    context.lease_expiry = Timestamp{400};
    IG_CHECK(is_fenced(intervention, context, reason, detail));
    IG_CHECK(reason == RefusalReason::LeaseExpired);

    context.lease_expiry = Timestamp{900};
    context.now = Timestamp{1'500};
    IG_CHECK(is_fenced(intervention, context, reason, detail));
}

IG_TEST(lifecycle, epoch_tracker_is_monotonic) {
    EpochTracker tracker{};
    tracker.reset(EpochId{3}, BootIncarnation{1});
    IG_CHECK(tracker.accepts(EpochId{3}));
    IG_CHECK(!tracker.accepts(EpochId{2}));
    IG_CHECK(!tracker.accepts(EpochId{4}));
    IG_CHECK(!tracker.advance(EpochId{3}));
    IG_CHECK(!tracker.advance(EpochId{2}));
    IG_REQUIRE(static_cast<bool>(tracker.advance(EpochId{4})));
    IG_CHECK(tracker.accepts(EpochId{4}));
    IG_CHECK(!tracker.advance(EpochId{}));
}
