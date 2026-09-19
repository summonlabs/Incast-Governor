// Incast Governor - seeded randomized property tests over the governance engine.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

#include "governor_fixture.hpp"
#include "incast/detect.hpp"
#include "incast/synthetic.hpp"
#include "test_framework.hpp"

using namespace incast;

namespace {

struct RandomSpec {
    synthetic::FanInSpec spec{};
    std::uint64_t seed = 0;
};

RandomSpec random_spec(std::uint64_t seed) {
    DeterministicRng rng(seed);
    synthetic::FanInSpec spec{};

    spec.senders = static_cast<std::uint32_t>(2 + rng.bounded(512));
    spec.window_width = Duration{static_cast<std::int64_t>(100'000 + rng.bounded(5'000'000))};
    const std::uint64_t spread_choice = rng.bounded(3);
    if (spread_choice == 0) {
        spec.arrival_spread = Duration{static_cast<std::int64_t>(rng.bounded(40'000))};      // synchronized
    } else if (spread_choice == 1) {
        spec.arrival_spread = Duration{static_cast<std::int64_t>(spec.window_width.nanos() - 1)};  // spread out
    } else {
        spec.arrival_spread = Duration{static_cast<std::int64_t>(rng.bounded(
            static_cast<std::uint64_t>(spec.window_width.nanos())))};
    }
    const std::uint64_t per_sender = 10'000'000 + rng.bounded(5'000'000'000ULL);
    spec.per_sender_rate = Rate{per_sender};
    spec.service_rate = Rate{10'000'000 + rng.bounded(200'000'000'000ULL)};
    spec.queue_pressure_bp = static_cast<std::uint32_t>(rng.bounded(11'000));
    spec.buffer_capacity = Bytes{1u << 20};
    spec.queue_capacity = Bytes{64u << 10};
    spec.outstanding_per_sender = Bytes{rng.bounded((64u << 10))};
    spec.drops = rng.bounded(4) == 0 ? rng.bounded(100'000) : 0;
    spec.protected_senders = static_cast<std::uint32_t>(rng.bounded(spec.senders + 1));
    spec.duplicate_senders = static_cast<std::uint32_t>(rng.bounded(4) == 0 ? rng.bounded(8) : 0);
    spec.omit_senders = static_cast<std::uint32_t>(rng.bounded(6) == 0 ? rng.bounded(spec.senders / 2 + 1) : 0);
    spec.mark_population_saturated = spec.omit_senders > 0;
    spec.churn_incarnations = rng.bounded(4) == 0;
    spec.arrivals_ascending = rng.bounded(2) == 0;
    spec.seed = rng.next();

    if (rng.bounded(8) == 0) spec.fan_in_state = EvidenceState::Stale;
    if (rng.bounded(8) == 0) spec.timing_state = EvidenceState::Unknown;
    if (rng.bounded(8) == 0) spec.rate_state = EvidenceState::Contradictory;
    if (rng.bounded(8) == 0) spec.queue_state = EvidenceState::OutOfWindow;
    if (rng.bounded(8) == 0) spec.omit_queue = true;
    if (rng.bounded(8) == 0) spec.stale_capacity = true;

    RandomSpec result{};
    result.spec = spec;
    result.seed = seed;
    return result;
}

}  // namespace

IG_TEST(property, invariants_hold_across_seeded_random_scenarios) {
    for (std::uint64_t seed = 1; seed <= 400; ++seed) {
        const RandomSpec random = random_spec(seed);
        const auto spec = random.spec;
        const auto bundle = synthetic::build(spec);
        const auto policy = make_default_policy();
        const auto collisions = detect_collisions(bundle);
        const Classification classification = classify(bundle, policy, collisions);

        // Classification is always one of the defined kinds and severity never
        // exceeds the ordering implied by the kind.
        if (classification.kind == IncidentKind::Unknown) {
            IG_CHECK(classification.severity == Severity::None);
        }
        if (classification.kind == IncidentKind::NoFanIn ||
            classification.kind == IncidentKind::OrdinaryManyToOne ||
            classification.kind == IncidentKind::Recovery) {
            IG_CHECK(classification.severity == Severity::None ||
                     classification.severity == Severity::Advisory);
        }

        // Synchronization is never claimed without the population floor, the
        // compressed window and a sufficient synchronized share.
        if (classification.synchronization.synchronized) {
            IG_CHECK(classification.synchronization.population_floor_met);
            IG_CHECK(classification.synchronization.window_compressed);
            IG_CHECK(classification.synchronization.share_sufficient);
            IG_CHECK(classification.metrics.active_senders >=
                     policy.synchronization.min_synchronized_senders);
        }

        // No protected or collided sender is ever mitigable.
        for (const auto& contribution : classification.contributions) {
            if (contribution.protected_obligation || contribution.collided) {
                IG_CHECK(!contribution.mitigable);
            }
            if (contribution.mitigable) {
                IG_CHECK(contribution.synchronized);
                IG_CHECK(!contribution.collided);
                IG_CHECK(!contribution.protected_obligation);
            }
        }

        auto governor = igfixture::make_governor(spec, true);
        const Decision decision = governor.evaluate(bundle);
        const auto bounds = policy.bounds;

        if (decision.authority.intervention_authorized) {
            IG_CHECK(decision.refusal == RefusalReason::None);
            IG_CHECK(decision.authority.classes[static_cast<std::size_t>(EvidenceClass::FanIn)] ==
                     EvidenceState::Present);
            IG_CHECK(decision.intent.bounded);
            IG_CHECK(decision.intent.reduction_bp.value() <= bounds.max_reduction_bp);
            IG_CHECK(decision.intent.admission_reduction_bp.value() <= bounds.max_admission_reduction_bp);
            IG_CHECK(decision.intent.headroom_bytes.count() <= bounds.max_headroom_bytes.count());
            IG_CHECK(decision.intent.stagger_slots <= bounds.max_stagger_slots);
            IG_CHECK(decision.intent.penalized_senders.size() <= bounds.max_penalized_senders);
            IG_CHECK(decision.intent.duration.nanos() >= bounds.min_duration.nanos());
            IG_CHECK(decision.intent.duration.nanos() <= bounds.max_duration.nanos());
            IG_CHECK(decision.authority.protected_floor_preserved);
            if (decision.metrics.population_saturated) {
                IG_CHECK(decision.intent.penalized_senders.empty());
            }
            // Authority is always bound to a concrete generation set.
            IG_CHECK(decision.explanation.event.valid() || decision.event.valid());
            IG_CHECK(decision.policy_generation.valid());
            IG_CHECK(decision.state_generation.valid());
        } else {
            IG_CHECK(decision.intent.kind == MitigationKind::None ||
                     decision.outcome == DecisionOutcome::Refused ||
                     decision.outcome == DecisionOutcome::NoAction);
        }

        // Determinism: the same seed yields the same rendered decision.
        auto repeat = igfixture::make_governor(spec, true);
        const Decision again = repeat.evaluate(synthetic::build(spec));
        IG_CHECK_EQ(again.explanation.render_json(), decision.explanation.render_json());
    }
}

IG_TEST(property, wider_arrival_spread_never_increases_synchronization) {
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        DeterministicRng rng(seed * 7919);
        auto spec = synthetic::synchronized_burst(static_cast<std::uint32_t>(16 + rng.bounded(200)));
        const auto policy = make_default_policy();

        std::uint32_t previous_compression = 10'000;
        std::int64_t previous_spread = 0;
        for (int step = 0; step < 12; ++step) {
            spec.arrival_spread = Duration{previous_spread};
            const auto bundle = synthetic::build(spec);
            const Classification classification = classify(bundle, policy, detect_collisions(bundle));
            IG_CHECK(classification.synchronization.arrival_compression_bp.value() <= previous_compression);
            previous_compression = classification.synchronization.arrival_compression_bp.value();
            previous_spread = previous_spread == 0 ? 5'000 : previous_spread * 2;
            if (previous_spread >= spec.window_width.nanos()) break;
        }
    }
}

IG_TEST(property, mitigation_is_monotone_in_oversubscription) {
    std::uint32_t previous_reduction = 0;
    for (std::uint64_t service = 40'000'000'000ULL; service >= 8'000'000'000ULL; service -= 4'000'000'000ULL) {
        auto spec = synthetic::synchronized_burst(64);
        spec.service_rate = Rate{service};
        spec.queue_pressure_bp = 9'500;
        spec.drops = 8;
        auto governor = igfixture::make_governor(spec, true);
        const Decision decision = governor.evaluate(synthetic::build(spec));
        IG_REQUIRE(decision.authority.intervention_authorized);
        IG_CHECK(decision.intent.reduction_bp.value() >= previous_reduction);
        previous_reduction = decision.intent.reduction_bp.value();
    }
}

IG_TEST(property, capping_the_population_never_lowers_the_classification) {
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        DeterministicRng rng(seed * 104729);
        auto full = synthetic::synchronized_burst(static_cast<std::uint32_t>(32 + rng.bounded(300)));
        full.queue_pressure_bp = 9'200;
        full.drops = 4;

        auto truncated = full;
        truncated.omit_senders = static_cast<std::uint32_t>(1 + rng.bounded(full.senders / 4));
        truncated.mark_population_saturated = true;

        const auto policy = make_default_policy();
        const auto full_bundle = synthetic::build(full);
        const auto truncated_bundle = synthetic::build(truncated);
        const auto full_classification = classify(full_bundle, policy, detect_collisions(full_bundle));
        const auto truncated_classification =
            classify(truncated_bundle, policy, detect_collisions(truncated_bundle));

        const int rank_full = static_cast<int>(full_classification.kind);
        const int rank_truncated = static_cast<int>(truncated_classification.kind);
        IG_CHECK(rank_truncated == -1 || full_classification.kind == IncidentKind::Unknown ||
                 rank_truncated <= rank_full);
        IG_CHECK(truncated_classification.metrics.population_saturated);
    }
}
