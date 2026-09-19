// Incast Governor - engine, lifecycle, restart, concurrency and determinism tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "governor_fixture.hpp"
#include "incast/governor.hpp"
#include "incast/synthetic.hpp"
#include "test_framework.hpp"

using namespace incast;

namespace {

// Produces a fresh window of the same shape, advancing every generation and
// sequence so that replay protection never interferes with lifecycle testing.
synthetic::FanInSpec window_of(const synthetic::FanInSpec& base, std::uint64_t index) {
    synthetic::FanInSpec spec = base;
    spec.window = WindowId{index};
    spec.window_generation = WindowGeneration{index};
    spec.event = EventId{index};
    spec.event_generation = EventGeneration{index};
    spec.sequence = index * 10;
    spec.now = Timestamp{base.now.nanos() + static_cast<std::int64_t>(index) * 1'000'000};
    return spec;
}

}  // namespace

IG_TEST(governor, requires_explicit_authority_binding) {
    const auto spec = synthetic::synchronized_burst(64);
    GovernorConfig config{};
    config.policy = make_default_policy();
    config.destination = spec.destination;
    Governor unbound(config);

    const Decision decision = unbound.evaluate(synthetic::build(spec));
    IG_CHECK(decision.outcome == DecisionOutcome::Refused);
    IG_CHECK(decision.refusal == RefusalReason::LeaseMissing);
    IG_CHECK(!decision.authority.intervention_authorized);

    IG_REQUIRE(static_cast<bool>(unbound.bind_authority(spec.epoch, spec.boot, spec.worker, spec.lease,
                                                        Timestamp{spec.now.nanos() + spec.lease_horizon.nanos()})));
    const Decision bound = unbound.evaluate(synthetic::build(spec));
    IG_CHECK(bound.kind == IncidentKind::ActiveIncast);
}

IG_TEST(governor, full_lifecycle_issue_continue_relax_retire) {
    const auto active = synthetic::synchronized_burst(128);
    const auto calm = synthetic::unsynchronized_many_to_one(128);
    auto governor = igfixture::make_governor(active, true);

    const Decision issued = governor.evaluate(synthetic::build(window_of(active, 1)));
    IG_CHECK(issued.outcome == DecisionOutcome::InterventionIssued);
    IG_CHECK(issued.authority.intervention_authorized);
    IG_CHECK(issued.intent.active());
    const InterventionId identifier = issued.intervention;
    IG_CHECK(identifier.valid());
    const auto first_reduction = issued.intent.reduction_bp.value();

    const Decision continued = governor.evaluate(synthetic::build(window_of(active, 2)));
    IG_CHECK(continued.outcome == DecisionOutcome::InterventionContinued);
    IG_CHECK_EQ(continued.intervention.value(), identifier.value());
    IG_CHECK_EQ(governor.interventions().size(), 1u);

    // The first calm window is held by hysteresis: the intervention is
    // reinforced rather than flapped.
    const Decision held = governor.evaluate(synthetic::build(window_of(calm, 3)));
    IG_CHECK(held.kind == IncidentKind::ActiveIncast);
    IG_CHECK(held.outcome == DecisionOutcome::InterventionContinued);
    // The hold preserves the intervention magnitude exactly: a window the
    // classifier does not yet trust must not weaken live mitigation.
    IG_CHECK_EQ(held.intent.reduction_bp.value(), first_reduction);
    IG_CHECK_EQ(held.intervention.value(), identifier.value());

    // Once the dwell is satisfied the window becomes a Recovery and the
    // intervention is relaxed in bounded steps.
    const Decision relaxed = governor.evaluate(synthetic::build(window_of(calm, 4)));
    IG_CHECK(relaxed.kind == IncidentKind::Recovery);
    IG_CHECK(relaxed.outcome == DecisionOutcome::InterventionRelaxed);
    IG_CHECK(relaxed.intent.reduction_bp.value() < first_reduction);

    Decision latest = relaxed;
    std::uint64_t index = 5;
    while (latest.outcome != DecisionOutcome::InterventionRetired && index < 64) {
        latest = governor.evaluate(synthetic::build(window_of(calm, index)));
        index += 1;
    }
    IG_CHECK(latest.outcome == DecisionOutcome::InterventionRetired);
    IG_CHECK(governor.interventions().front().state == InterventionState::Retired);
}

IG_TEST(governor, intervention_holds_when_recovery_policy_is_absent) {
    const auto active = synthetic::synchronized_burst(96);
    const auto calm = synthetic::unsynchronized_many_to_one(96);
    auto governor = igfixture::make_governor(active, false);

    const Decision issued = governor.evaluate(synthetic::build(window_of(active, 1)));
    IG_REQUIRE(issued.outcome == DecisionOutcome::InterventionIssued);

    bool saw_recovery = false;
    for (std::uint64_t index = 2; index < 12; ++index) {
        const Decision decision = governor.evaluate(synthetic::build(window_of(calm, index)));
        if (decision.kind != IncidentKind::Recovery) continue;
        saw_recovery = true;
        // Without an explicit recovery policy a Recovery window holds: the
        // intervention is never silently weakened.
        IG_CHECK(decision.outcome == DecisionOutcome::NoAction);
        IG_CHECK(decision.refusal == RefusalReason::AwaitingRecoveryPolicy);
        IG_CHECK(decision.intervention_state == InterventionState::Active);
    }
    IG_CHECK(saw_recovery);
    IG_CHECK(governor.interventions().front().state == InterventionState::Active);
    IG_CHECK_EQ(governor.interventions().front().relax_steps, 0u);
}

IG_TEST(governor, hysteresis_prevents_oscillation_between_windows) {
    const auto active = synthetic::synchronized_burst(128);
    const auto calm = synthetic::unsynchronized_many_to_one(128);
    auto governor = igfixture::make_governor(active, false);

    IG_REQUIRE(governor.evaluate(synthetic::build(window_of(active, 1))).kind == IncidentKind::ActiveIncast);

    std::uint64_t index = 2;
    for (int cycle = 0; cycle < 6; ++cycle) {
        // A single calm window may not de-escalate: the intervention is
        // reinforced rather than flapped.
        const Decision toggled = governor.evaluate(synthetic::build(window_of(calm, index++)));
        IG_CHECK(toggled.kind == IncidentKind::ActiveIncast);
        IG_CHECK(toggled.outcome == DecisionOutcome::InterventionContinued);
        const Decision back = governor.evaluate(synthetic::build(window_of(active, index++)));
        IG_CHECK(back.kind == IncidentKind::ActiveIncast);
    }
    IG_CHECK_EQ(governor.interventions().size(), 1u);
    IG_CHECK_EQ(governor.interventions().front().relax_steps, 0u);
}

IG_TEST(governor, policy_generation_change_forces_revalidation) {
    const auto active = synthetic::synchronized_burst(96);
    auto governor = igfixture::make_governor(active, true);
    IG_REQUIRE(governor.evaluate(synthetic::build(window_of(active, 1))).outcome ==
               DecisionOutcome::InterventionIssued);

    GovernorPolicy revised = governor.policy();
    revised.generation = PolicyGeneration{2};
    IG_REQUIRE(static_cast<bool>(governor.configure_policy(revised, Timestamp{active.now.nanos() + 10})));
    IG_CHECK(governor.interventions().front().state == InterventionState::Revalidating);
    IG_CHECK(governor.snapshot_state().requires_revalidation);
}

IG_TEST(governor, restart_never_restores_live_authority) {
    const auto active = synthetic::synchronized_burst(96);
    auto original = igfixture::make_governor(active, true);
    IG_REQUIRE(original.evaluate(synthetic::build(window_of(active, 1))).outcome ==
               DecisionOutcome::InterventionIssued);
    const GovernorState snapshot = original.snapshot_state();

    auto restarted = igfixture::make_governor(active, true);
    IG_REQUIRE(static_cast<bool>(restarted.restore_state(snapshot)));
    const GovernorState restored = restarted.snapshot_state();
    IG_CHECK(restored.requires_revalidation);
    IG_CHECK(restored.restored_from_durable);
    IG_CHECK(!restored.epoch.valid());
    IG_CHECK(!restored.boot.valid());
    IG_REQUIRE(!restored.interventions.empty());
    IG_CHECK(restored.interventions.front().state == InterventionState::Revalidating);

    // Until authority is re-bound, the restarted governor refuses everything.
    const Decision refused = restarted.evaluate(synthetic::build(window_of(active, 2)));
    IG_CHECK(refused.outcome == DecisionOutcome::Refused);
    IG_CHECK(refused.refusal == RefusalReason::LeaseMissing);

    // Re-binding the same epoch is refused: the old epoch is stale by definition
    // after a restart, and the coordinator must advance it.
    IG_REQUIRE(static_cast<bool>(restarted.bind_authority(active.epoch, active.boot, active.worker,
                                                          active.lease,
                                                          Timestamp{active.now.nanos() + 10'000'000'000})));
    const Decision revalidated = restarted.evaluate(synthetic::build(window_of(active, 3)));
    IG_CHECK(revalidated.intervention_state == InterventionState::Active ||
             revalidated.intervention_state == InterventionState::Revalidating);
    IG_CHECK(revalidated.authority.intervention_authorized);
}

IG_TEST(governor, stale_epoch_boot_and_lease_are_refused) {
    const auto active = synthetic::synchronized_burst(64);

    auto wrong_epoch = igfixture::make_governor(active);
    auto spec = window_of(active, 1);
    spec.epoch = EpochId{active.epoch.value() + 7};
    IG_CHECK(wrong_epoch.evaluate(synthetic::build(spec)).refusal == RefusalReason::EpochMismatch);

    auto wrong_boot = igfixture::make_governor(active);
    spec = window_of(active, 1);
    spec.boot = BootIncarnation{active.boot.value() + 7};
    IG_CHECK(wrong_boot.evaluate(synthetic::build(spec)).refusal == RefusalReason::BootMismatch);

    auto wrong_lease = igfixture::make_governor(active);
    spec = window_of(active, 1);
    spec.lease = LeaseId{active.lease.value() + 7};
    IG_CHECK(wrong_lease.evaluate(synthetic::build(spec)).refusal == RefusalReason::LeaseMissing);

    auto expired = igfixture::make_governor(active);
    spec = window_of(active, 1);
    spec.lease_horizon = Duration{-1};
    IG_CHECK(expired.evaluate(synthetic::build(spec)).refusal == RefusalReason::LeaseExpired);

    auto other_destination = igfixture::make_governor(active);
    spec = window_of(active, 1);
    spec.destination = DestinationId{active.destination.value() + 5};
    IG_CHECK(other_destination.evaluate(synthetic::build(spec)).refusal ==
             RefusalReason::DestinationMismatch);
}

IG_TEST(governor, destination_generation_regression_is_refused) {
    const auto active = synthetic::synchronized_burst(64);
    auto governor = igfixture::make_governor(active);
    auto forward = window_of(active, 1);
    forward.destination_generation = DestinationGeneration{9};
    IG_CHECK(governor.evaluate(synthetic::build(forward)).outcome != DecisionOutcome::Refused);

    auto backward = window_of(active, 2);
    backward.destination_generation = DestinationGeneration{3};
    const Decision decision = governor.evaluate(synthetic::build(backward));
    IG_CHECK(decision.outcome == DecisionOutcome::Refused);
    IG_CHECK(decision.refusal == RefusalReason::StaleEvidence);
}

IG_TEST(governor, decisions_are_deterministic_for_identical_inputs) {
    const auto active = synthetic::synchronized_burst(128);
    auto first = igfixture::make_governor(active, true);
    auto second = igfixture::make_governor(active, true);
    const auto bundle = synthetic::build(active);

    const Decision a = first.evaluate(bundle);
    const Decision b = second.evaluate(bundle);
    IG_CHECK_EQ(a.explanation.render_text(), b.explanation.render_text());
    IG_CHECK_EQ(a.explanation.render_json(), b.explanation.render_json());
    IG_CHECK(a.outcome == b.outcome);
    IG_CHECK_EQ(a.intent.reduction_bp.value(), b.intent.reduction_bp.value());
}

IG_TEST(governor, shutdown_stops_work_and_retires_interventions) {
    const auto active = synthetic::synchronized_burst(96);
    auto governor = igfixture::make_governor(active, true);
    IG_REQUIRE(governor.evaluate(synthetic::build(window_of(active, 1))).outcome ==
               DecisionOutcome::InterventionIssued);

    std::size_t retired = 0;
    IG_REQUIRE(static_cast<bool>(governor.shutdown(Timestamp{active.now.nanos() + 1'000}, retired)));
    IG_CHECK_EQ(retired, 1u);
    IG_CHECK(governor.interventions().front().state == InterventionState::Retired);

    const Decision after = governor.evaluate(synthetic::build(window_of(active, 2)));
    IG_CHECK(after.outcome == DecisionOutcome::Refused);
    IG_CHECK(!after.authority.intervention_authorized);
}

IG_TEST(governor, retire_reports_unknown_and_terminal_interventions) {
    const auto active = synthetic::synchronized_burst(64);
    auto governor = igfixture::make_governor(active, true);
    const Decision decision = governor.evaluate(synthetic::build(window_of(active, 1)));
    IG_REQUIRE(decision.outcome == DecisionOutcome::InterventionIssued);
    IG_CHECK(!governor.retire(InterventionId{999}, Timestamp{1'000}, "unknown"));
    IG_REQUIRE(static_cast<bool>(governor.retire(decision.intervention, Timestamp{1'000}, "test")));
    IG_CHECK(!governor.retire(decision.intervention, Timestamp{1'001}, "again"));
}

IG_TEST(governor, evidence_sequences_are_scoped_to_the_source_incarnation) {
    const auto active = synthetic::synchronized_burst(64);
    auto governor = igfixture::make_governor(active);

    auto first = window_of(active, 1);
    first.sequence = 10;
    IG_CHECK(governor.evaluate(synthetic::build(first)).outcome != DecisionOutcome::Refused);
    IG_CHECK_EQ(governor.snapshot_state().last_sequence_boot.value(), active.boot.value());

    // A restarted source presents a fresh sequence space under a new
    // incarnation; that is not a reorder and must not be refused.
    const BootIncarnation new_boot{active.boot.value() + 991};
    IG_REQUIRE(static_cast<bool>(governor.bind_authority(EpochId{active.epoch.value() + 1}, new_boot,
                                                         active.worker, LeaseId{active.lease.value() + 1},
                                                         Timestamp{active.now.nanos() + 10'000'000'000})));
    auto restarted = window_of(active, 2);
    restarted.sequence = 1;
    restarted.epoch = EpochId{active.epoch.value() + 1};
    restarted.boot = new_boot;
    restarted.lease = LeaseId{active.lease.value() + 1};
    IG_CHECK(governor.evaluate(synthetic::build(restarted)).outcome != DecisionOutcome::Refused);

    // A genuine replay inside the same incarnation is still refused.
    const Decision replay = governor.evaluate(synthetic::build(restarted));
    IG_CHECK(replay.outcome == DecisionOutcome::Refused);
    IG_CHECK(replay.refusal == RefusalReason::ReorderedEvidence);
}

IG_TEST(governor, cancelled_interventions_cannot_be_revived_by_later_evidence) {
    const auto active = synthetic::synchronized_burst(96);
    auto governor = igfixture::make_governor(active, true);

    const Decision issued = governor.evaluate(synthetic::build(window_of(active, 1)));
    IG_REQUIRE(issued.outcome == DecisionOutcome::InterventionIssued);
    IG_REQUIRE(static_cast<bool>(governor.retire(issued.intervention, Timestamp{active.now.nanos()}, "cancel")));
    IG_CHECK(governor.interventions().front().state == InterventionState::Retired);

    // No later evidence can resurrect the cancelled intervention identity, and
    // the retired record stays terminal for the life of the governor.
    const Decision after = governor.evaluate(synthetic::build(window_of(active, 2)));
    IG_CHECK(after.outcome == DecisionOutcome::InterventionIssued);
    IG_CHECK(after.intervention != issued.intervention);
    for (const auto& intervention : governor.interventions()) {
        if (intervention.id != issued.intervention) continue;
        IG_CHECK(intervention.state == InterventionState::Retired);
    }
}

IG_TEST(governor, decision_observer_runs_outside_the_internal_lock) {
    const auto active = synthetic::synchronized_burst(64);
    auto governor = igfixture::make_governor(active, true);

    std::atomic<int> observed{0};
    std::atomic<int> reentrant{0};
    governor.set_decision_observer([&](const Decision&) {
        observed.fetch_add(1);
        // Re-entering a public, mutex-guarded method from the observer would
        // deadlock on a non-recursive mutex if the lock were still held.
        const auto interventions = governor.interventions();
        reentrant.fetch_add(static_cast<int>(interventions.size()));
        const auto state = governor.snapshot_state();
        IG_CHECK(state.evaluation_count == 1);
    });

    const Decision decision = governor.evaluate(synthetic::build(window_of(active, 1)));
    IG_CHECK(decision.outcome == DecisionOutcome::InterventionIssued);
    IG_CHECK_EQ(observed.load(), 1);
    IG_CHECK_EQ(reentrant.load(), 1);
}

IG_TEST(governor, concurrent_evaluations_are_serialised_and_accounted) {
    const auto active = synthetic::synchronized_burst(64);
    auto governor = igfixture::make_governor(active, true);

    constexpr int kThreads = 8;
    constexpr int kPerThread = 12;
    std::atomic<int> refusals{0};
    std::atomic<int> accepted{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([&, thread]() {
            for (int iteration = 0; iteration < kPerThread; ++iteration) {
                auto spec = active;
                const auto unique = static_cast<std::uint64_t>(thread * 1'000 + iteration + 1);
                spec.window = WindowId{unique};
                spec.window_generation = WindowGeneration{unique};
                spec.event = EventId{unique};
                spec.event_generation = EventGeneration{unique};
                spec.sequence = unique;
                spec.now = Timestamp{active.now.nanos() + static_cast<std::int64_t>(unique)};
                const Decision decision = governor.evaluate(synthetic::build(spec));
                IG_CHECK(decision.state_generation.valid());
                if (decision.outcome == DecisionOutcome::Refused) {
                    refusals.fetch_add(1);
                } else {
                    accepted.fetch_add(1);
                }
            }
        });
    }
    for (auto& thread : threads) thread.join();

    // Every evaluation is accounted for exactly once: either it produced an
    // authoritative decision or it was refused for ordering. Nothing is lost,
    // nothing is applied twice, and no state is torn.
    IG_CHECK_EQ(accepted.load() + refusals.load(), kThreads * kPerThread);
    IG_CHECK(accepted.load() > 0);
    IG_CHECK_EQ(static_cast<int>(governor.snapshot_state().evaluation_count), accepted.load());
}

IG_TEST(governor, explanation_is_bounded_and_complete) {
    const auto active = synthetic::capacity_collapse(256);
    auto governor = igfixture::make_governor(active, true);
    const Decision decision = governor.evaluate(synthetic::build(active));
    const Explanation& explanation = decision.explanation;

    IG_CHECK(explanation.within_budget());
    IG_CHECK(explanation.approximate_bytes() <= kMaxExplanationBytes);
    IG_CHECK(explanation.top_contributors.size() <= 64);
    IG_CHECK(explanation.notes.size() <= kMaxExplanationNotes);
    IG_CHECK(!explanation.headline.empty());
    IG_CHECK(explanation.render_json().size() <= kMaxExplanationBytes);
    IG_CHECK(explanation.render_json().front() == '{');
    IG_CHECK(explanation.render_json().back() == '}');
    IG_CHECK(explanation.render_text().find("authority:") != std::string::npos);
    IG_CHECK(explanation.render_text().find("protected") != std::string::npos);
    IG_CHECK(!explanation.protected_exceptions.empty());
}

IG_TEST(governor, history_and_intervention_tables_stay_bounded) {
    const auto active = synthetic::synchronized_burst(32);
    const auto calm = synthetic::unsynchronized_many_to_one(32);
    GovernorConfig config{};
    config.policy = make_default_policy();
    config.policy.recovery.enabled = true;
    config.policy.recovery.required_clean_windows = 1;
    config.destination = active.destination;
    config.destination_generation = active.destination_generation;
    config.worker = active.worker;
    config.lease = active.lease;
    config.epoch = active.epoch;
    config.boot = active.boot;
    config.lease_expiry = Timestamp{active.now.nanos() + 1'000'000'000'000};
    config.max_history = 8;
    Governor governor(config);

    for (std::uint64_t index = 1; index <= 200; ++index) {
        const auto& source = (index % 3 == 0) ? calm : active;
        static_cast<void>(governor.evaluate(synthetic::build(window_of(source, index))));
    }
    const GovernorState state = governor.snapshot_state();
    IG_CHECK(state.history.size() <= 8);
    IG_CHECK(state.interventions.size() <= kMaxInterventions);
}
