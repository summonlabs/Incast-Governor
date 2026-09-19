// Incast Governor - shared test fixtures.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_TESTS_GOVERNOR_FIXTURE_HPP
#define INCAST_TESTS_GOVERNOR_FIXTURE_HPP

#include <cstdint>
#include <string>

#include "incast/governor.hpp"
#include "incast/synthetic.hpp"

namespace igfixture {

// Builds a governor already bound to the authority that the synthetic evidence
// carries, so that the authority-binding refusal paths are exercised only where
// a test deliberately breaks the binding.
[[nodiscard]] inline incast::Governor make_governor(const incast::synthetic::FanInSpec& spec,
                                                    bool recovery_enabled = false) {
    incast::GovernorConfig config{};
    config.policy = incast::make_default_policy();
    if (recovery_enabled) {
        config.policy.recovery.enabled = true;
        config.policy.recovery.required_clean_windows = 1;
    }
    config.destination = spec.destination;
    config.destination_generation = spec.destination_generation;
    config.worker = spec.worker;
    config.lease = spec.lease;
    config.epoch = spec.epoch;
    config.boot = spec.boot;
    config.lease_expiry = incast::Timestamp{spec.now.nanos() + spec.lease_horizon.nanos()};
    return incast::Governor(config);
}

}  // namespace igfixture

#endif  // INCAST_TESTS_GOVERNOR_FIXTURE_HPP
