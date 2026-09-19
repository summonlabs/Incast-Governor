// Incast Governor - independent downstream find_package consumer.
//
// Uses only the installed public headers and the exported CMake target. It
// builds a synthetic evidence window, runs one governance decision and prints
// the bounded explanation, proving that the installed package is usable from an
// unrelated project.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <string>

#include <incast/governor.hpp>
#include <incast/synthetic.hpp>

int main() {
    using namespace incast;

    const auto spec = synthetic::synchronized_burst(128);
    GovernorConfig config{};
    config.policy = make_default_policy();
    config.policy.recovery.enabled = true;
    config.destination = spec.destination;
    config.destination_generation = spec.destination_generation;
    config.worker = spec.worker;
    config.lease = spec.lease;
    config.epoch = spec.epoch;
    config.boot = spec.boot;
    config.lease_expiry = Timestamp{spec.now.nanos() + spec.lease_horizon.nanos()};

    Governor governor(config);
    const Decision decision = governor.evaluate(synthetic::build(spec));

    std::printf("consumer: %s %s\n", std::string(kProductName).c_str(),
                std::string(kProductVersion).c_str());
    std::printf("kind=%s severity=%s authorized=%s mitigation=%s reduction_bp=%u\n",
                std::string(to_string(decision.kind)).c_str(),
                std::string(to_string(decision.severity)).c_str(),
                decision.authority.intervention_authorized ? "true" : "false",
                std::string(to_string(decision.intent.kind)).c_str(),
                decision.intent.reduction_bp.value());

    if (decision.kind != IncidentKind::ActiveIncast) return 1;
    if (!decision.authority.intervention_authorized) return 2;
    if (!decision.intent.active()) return 3;
    if (!decision.explanation.within_budget()) return 4;
    return 0;
}
