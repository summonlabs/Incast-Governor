// Incast Governor - detection and classification tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "incast/detect.hpp"
#include "incast/policy.hpp"
#include "governor_fixture.hpp"
#include "incast/synthetic.hpp"
#include "test_framework.hpp"

using namespace incast;

namespace {

GovernorPolicy policy_with_recovery() {
    GovernorPolicy policy = make_default_policy();
    policy.recovery.enabled = true;
    policy.recovery.required_clean_windows = 1;
    return policy;
}

}  // namespace

IG_TEST(detect, synchronized_burst_is_active_incast) {
    const auto spec = synthetic::synchronized_burst(128);
    const auto bundle = synthetic::build(spec);
    const auto policy = make_default_policy();
    const Classification classification = classify(bundle, policy, detect_collisions(bundle));

    IG_CHECK(classification.kind == IncidentKind::ActiveIncast);
    IG_CHECK(classification.severity == Severity::Critical);
    IG_CHECK(classification.synchronization.synchronized);
    IG_CHECK_EQ(classification.metrics.active_senders, 128u);
    IG_CHECK(classification.synchronization.synchronized_share_bp.value() >= 9'000u);
    IG_CHECK(classification.metrics.oversubscription_bp.value() > 0u);
}

IG_TEST(detect, many_to_one_without_synchronization_is_not_incast) {
    const auto spec = synthetic::unsynchronized_many_to_one(512);
    const auto bundle = synthetic::build(spec);
    const auto policy = make_default_policy();
    const Classification classification = classify(bundle, policy, detect_collisions(bundle));

    IG_CHECK(classification.kind == IncidentKind::OrdinaryManyToOne);
    IG_CHECK(!classification.synchronization.window_compressed);
    IG_CHECK(classification.metrics.active_senders == 512u);
    IG_CHECK(!classification.synchronization.synchronized);
}

IG_TEST(detect, sender_count_alone_never_proves_synchronization) {
    // Identical populations, only the arrival spread differs.
    auto tight = synthetic::synchronized_burst(256);
    auto loose = synthetic::synchronized_burst(256);
    loose.arrival_spread = Duration{800'000};

    const auto policy = make_default_policy();
    const auto tight_bundle = synthetic::build(tight);
    const auto loose_bundle = synthetic::build(loose);
    const auto tight_classification = classify(tight_bundle, policy, detect_collisions(tight_bundle));
    const auto loose_classification = classify(loose_bundle, policy, detect_collisions(loose_bundle));

    IG_CHECK(tight_classification.synchronization.synchronized);
    IG_CHECK(!loose_classification.synchronization.synchronized);
    IG_CHECK(tight_classification.kind != loose_classification.kind);
    IG_CHECK(tight_classification.metrics.active_senders == loose_classification.metrics.active_senders);
}

IG_TEST(detect, small_population_is_not_fan_in) {
    auto spec = synthetic::synchronized_burst(3);
    const auto bundle = synthetic::build(spec);
    const auto policy = make_default_policy();
    const Classification classification = classify(bundle, policy, detect_collisions(bundle));
    IG_CHECK(classification.kind == IncidentKind::NoFanIn);
    IG_CHECK(classification.severity == Severity::None);
}

IG_TEST(detect, risk_band_is_distinct_from_collapse) {
    auto spec = synthetic::synchronized_burst(64);
    spec.service_rate = Rate{48'000'000'000ULL};
    spec.queue_pressure_bp = 2'000;
    spec.drops = 0;
    const auto bundle = synthetic::build(spec);
    const auto policy = make_default_policy();
    const Classification classification = classify(bundle, policy, detect_collisions(bundle));
    IG_CHECK(classification.kind == IncidentKind::SynchronizedIncastRisk);
    IG_CHECK(classification.severity == Severity::Elevated);
    IG_CHECK(classification.metrics.oversubscription_bp.value() >= policy.pressure.risk_oversubscription_bp);
    IG_CHECK(classification.metrics.oversubscription_bp.value() < policy.pressure.collapse_oversubscription_bp);
}

IG_TEST(detect, stale_and_missing_evidence_stays_unknown) {
    const auto policy = make_default_policy();

    auto gap = synthetic::telemetry_gap(64);
    const auto gap_bundle = synthetic::build(gap);
    const Classification gap_classification = classify(gap_bundle, policy, detect_collisions(gap_bundle));
    IG_CHECK(gap_classification.kind == IncidentKind::Unknown);
    IG_CHECK(gap_classification.severity == Severity::None);
    IG_CHECK(!gap_classification.authoritative);
    IG_CHECK(gap_classification.refusal == RefusalReason::MissingEvidence);
    // The raw observations remain visible for diagnosis, but they are never
    // promoted into an incident.
    IG_CHECK_EQ(gap_classification.metrics.active_senders, 64u);
    IG_CHECK(gap_classification.synchronization.synchronized);

    auto stale = synthetic::synchronized_burst(64);
    stale.stale_capacity = true;
    const auto stale_bundle = synthetic::build(stale);
    const auto check = check_authority(stale_bundle, policy, {});
    IG_CHECK(check.reason == RefusalReason::StaleEvidence);
    IG_CHECK(check.classes[static_cast<std::size_t>(EvidenceClass::Capacity)] == EvidenceState::Stale);
}

IG_TEST(detect, reordered_evidence_is_refused) {
    const auto policy = make_default_policy();
    auto spec = synthetic::synchronized_burst(64);
    spec.sequence = 1;
    const auto bundle = synthetic::build(spec);

    std::array<std::uint64_t, kEvidenceClassCount> last_sequence{};
    last_sequence.fill(5);
    const auto check = check_authority(bundle, policy, last_sequence);
    IG_CHECK(check.reason == RefusalReason::ReorderedEvidence);

    std::array<std::uint64_t, kEvidenceClassCount> fresh{};
    IG_CHECK(check_authority(bundle, policy, fresh).reason == RefusalReason::None);
}

IG_TEST(detect, sequence_replay_inside_one_governor_is_refused) {
    const auto spec = synthetic::synchronized_burst(64);
    auto governor = igfixture::make_governor(spec);

    const Decision first = governor.evaluate(synthetic::build(spec));
    IG_CHECK(first.kind == IncidentKind::ActiveIncast);

    // Replaying the same window with the same per-class sequences must be
    // refused as reordered rather than re-applied.
    const Decision replay = governor.evaluate(synthetic::build(spec));
    IG_CHECK(replay.outcome == DecisionOutcome::Refused);
    IG_CHECK(replay.refusal == RefusalReason::ReorderedEvidence);
    IG_CHECK(!replay.authority.intervention_authorized);
}

IG_TEST(detect, identity_collisions_are_detected_and_excluded) {
    auto spec = synthetic::synchronized_burst(32);
    spec.duplicate_senders = 4;
    const auto bundle = synthetic::build(spec);
    const auto collisions = detect_collisions(bundle);
    IG_REQUIRE(!collisions.empty());
    IG_CHECK(collisions.front().kind == CollisionKind::DuplicateSenderDifferentIncarnation);
    IG_CHECK(collisions.front().sender == SenderId{1});

    const auto policy = make_default_policy();
    const Classification classification = classify(bundle, policy, collisions);
    std::size_t collided = 0;
    for (const auto& contribution : classification.contributions) {
        if (!contribution.collided) continue;
        collided += 1;
        IG_CHECK(!contribution.mitigable);
        IG_CHECK(contribution.disposition.find("collision") != std::string::npos);
    }
    IG_CHECK(collided >= 2);
}

IG_TEST(detect, protected_senders_are_never_marked_mitigable) {
    auto spec = synthetic::synchronized_burst(64);
    spec.protected_senders = 16;
    const auto bundle = synthetic::build(spec);
    const auto policy = make_default_policy();
    const Classification classification = classify(bundle, policy, detect_collisions(bundle));

    IG_CHECK_EQ(classification.metrics.protected_senders, 16u);
    IG_CHECK(classification.metrics.protected_offered.positive());
    for (const auto& contribution : classification.contributions) {
        if (contribution.protected_obligation) IG_CHECK(!contribution.mitigable);
    }
}

IG_TEST(detect, saturated_population_is_reported_as_a_lower_bound) {
    auto spec = synthetic::synchronized_burst(64);
    spec.omit_senders = 32;
    spec.mark_population_saturated = true;
    const auto bundle = synthetic::build(spec);
    const auto policy = make_default_policy();
    const Classification classification = classify(bundle, policy, detect_collisions(bundle));

    IG_CHECK(classification.metrics.population_saturated);
    IG_CHECK(classification.kind == IncidentKind::ActiveIncast);
    bool saw_note = false;
    for (const auto& note : classification.notes) {
        if (note.find("lower bounds") != std::string::npos) saw_note = true;
    }
    IG_CHECK(saw_note);
}

IG_TEST(detect, contribution_ranking_is_deterministic) {
    auto spec = synthetic::synchronized_burst(16);
    spec.per_sender_rate = Rate{1'000'000'000};
    spec.seed = 99;
    const auto first = synthetic::build(spec);
    const auto second = synthetic::build(spec);
    const auto policy = make_default_policy();

    const auto a = classify(first, policy, detect_collisions(first)).contributions;
    const auto b = classify(second, policy, detect_collisions(second)).contributions;
    IG_REQUIRE(a.size() == b.size());
    for (std::size_t index = 0; index < a.size(); ++index) {
        IG_CHECK(a[index].identity == b[index].identity);
        IG_CHECK_EQ(a[index].offered_rate.bytes_per_second(), b[index].offered_rate.bytes_per_second());
    }
}

IG_TEST(detect, hysteresis_holds_de_escalation_until_dwell) {
    const auto active = synthetic::synchronized_burst(128);
    const auto calm = synthetic::unsynchronized_many_to_one(128);
    const auto policy = make_default_policy();

    ClassificationContext context{};
    context.prior_kind = IncidentKind::ActiveIncast;
    context.clean_streak = 0;

    const auto calm_bundle = synthetic::build(calm);
    const Classification held = classify(calm_bundle, policy, detect_collisions(calm_bundle), context);
    IG_CHECK(held.instantaneous_kind == IncidentKind::OrdinaryManyToOne);
    IG_CHECK(held.kind == IncidentKind::ActiveIncast);
    IG_CHECK(held.hysteresis_held);

    context.clean_streak = policy.hysteresis.min_dwell_windows;
    const Classification stepped = classify(calm_bundle, policy, detect_collisions(calm_bundle), context);
    IG_CHECK(stepped.kind == IncidentKind::Recovery);

    const auto active_bundle = synthetic::build(active);
    const Classification escalation =
        classify(active_bundle, policy, detect_collisions(active_bundle), context);
    IG_CHECK(escalation.kind == IncidentKind::ActiveIncast);
    IG_CHECK(!escalation.hysteresis_held);
}

IG_TEST(detect, recovery_policy_is_required_before_any_relaxation) {
    auto clean = synthetic::unsynchronized_many_to_one(64);
    clean.service_rate = Rate{100'000'000'000ULL};
    clean.queue_pressure_bp = 4'000;
    clean.drops = 0;
    const auto metrics_bundle = synthetic::build(clean);
    const auto policy = make_default_policy();
    const auto metrics = compute_metrics(metrics_bundle, policy);

    RecoveryTracker tracker{};
    tracker.clean_windows = 100;
    tracker.capacity_revalidated = true;

    RecoveryVerdict disabled = evaluate_recovery(policy.recovery, tracker, metrics, false);
    IG_CHECK(!disabled.relax);
    IG_CHECK(disabled.reason == RefusalReason::PolicyDisabled);

    const RecoveryVerdict enabled = evaluate_recovery(policy_with_recovery().recovery, tracker, metrics, true);
    IG_CHECK(enabled.relax);

    RecoveryTracker unvalidated = tracker;
    unvalidated.capacity_revalidated = false;
    const RecoveryVerdict no_capacity =
        evaluate_recovery(policy_with_recovery().recovery, unvalidated, metrics, true);
    IG_CHECK(!no_capacity.relax);
    IG_CHECK(no_capacity.reason == RefusalReason::CapacityNotRevalidated);

    RecoveryTracker shallow = tracker;
    shallow.clean_windows = 0;
    const RecoveryVerdict dwell = evaluate_recovery(policy_with_recovery().recovery, shallow, metrics, true);
    IG_CHECK(!dwell.relax);
}
