// Incast Governor CLI - evaluate synthetic governance scenarios and explain
// the resulting decision.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "command_line.hpp"
#include "incast/governor.hpp"
#include "incast/synthetic.hpp"

namespace {

using namespace incast;

void print_usage() {
    std::puts("incast-governor <command> [options]");
    std::puts("");
    std::puts("Commands:");
    std::puts("  version                 print the product and ABI version");
    std::puts("  evaluate [options]      evaluate one synthetic evidence window");
    std::puts("  selftest                run the built-in closure scenarios");
    std::puts("");
    std::puts("evaluate options:");
    std::puts("  --senders N             active sender count (default 128)");
    std::puts("  --spread-ns N           arrival spread in nanoseconds (default 20000)");
    std::puts("  --window-ns N           observation window width (default 1000000)");
    std::puts("  --per-sender-bps N      offered rate per sender (default 1000000000)");
    std::puts("  --service-bps N         destination service rate (default 40000000000)");
    std::puts("  --queue-pressure-bp N   queue occupancy in basis points (default 9200)");
    std::puts("  --drops N               observed queue drop events (default 16)");
    std::puts("  --protected-senders N   senders carrying a protected obligation (default 0)");
    std::puts("  --protected-class NAME  service class of protected senders (default Replication)");
    std::puts("  --duplicate-senders N   colliding duplicate sender rows (default 0)");
    std::puts("  --omit-senders N        rows removed from a saturated population (default 0)");
    std::puts("  --fan-in-state STATE    Present|Unknown|Stale|Contradictory|Reordered|OutOfWindow");
    std::puts("  --recovery              enable the explicit recovery policy");
    std::puts("  --json                  render the explanation as JSON");
}

synthetic::FanInSpec build_spec(const tools::Arguments& arguments) {
    synthetic::FanInSpec spec{};
    spec.senders = static_cast<std::uint32_t>(arguments.u64("--senders", 128));
    spec.arrival_spread = Duration{arguments.i64("--spread-ns", 20'000)};
    spec.window_width = Duration{arguments.i64("--window-ns", 1'000'000)};
    spec.per_sender_rate = Rate{arguments.u64("--per-sender-bps", 1'000'000'000ULL)};
    spec.service_rate = Rate{arguments.u64("--service-bps", 40'000'000'000ULL)};
    spec.queue_pressure_bp = static_cast<std::uint32_t>(arguments.u64("--queue-pressure-bp", 9'200));
    spec.drops = arguments.u64("--drops", 16);
    spec.protected_senders = static_cast<std::uint32_t>(arguments.u64("--protected-senders", 0));
    spec.duplicate_senders = static_cast<std::uint32_t>(arguments.u64("--duplicate-senders", 0));
    spec.omit_senders = static_cast<std::uint32_t>(arguments.u64("--omit-senders", 0));
    spec.mark_population_saturated = spec.omit_senders > 0;
    spec.seed = arguments.u64("--seed", 0x5EEDC0DEULL);
    if (const auto klass = arguments.value("--protected-class")) {
        const auto parsed = parse_service_class(*klass);
        if (parsed) spec.protected_class = *parsed;
    }
    if (const auto state = arguments.value("--fan-in-state")) {
        if (*state == "Unknown") spec.fan_in_state = EvidenceState::Unknown;
        else if (*state == "Stale") spec.fan_in_state = EvidenceState::Stale;
        else if (*state == "Contradictory") spec.fan_in_state = EvidenceState::Contradictory;
        else if (*state == "Reordered") spec.fan_in_state = EvidenceState::Reordered;
        else if (*state == "OutOfWindow") spec.fan_in_state = EvidenceState::OutOfWindow;
        else spec.fan_in_state = EvidenceState::Present;
    }
    return spec;
}

Governor make_governor(const synthetic::FanInSpec& spec, bool recovery_enabled) {
    GovernorConfig config{};
    config.policy = make_default_policy();
    if (recovery_enabled) {
        config.policy.recovery.enabled = true;
        config.policy.recovery.required_clean_windows = 2;
        config.policy.recovery.require_capacity_revalidation = true;
    }
    config.destination = spec.destination;
    config.destination_generation = spec.destination_generation;
    config.worker = spec.worker;
    config.lease = spec.lease;
    config.epoch = spec.epoch;
    config.boot = spec.boot;
    config.lease_expiry = Timestamp{spec.now.nanos() + spec.lease_horizon.nanos()};
    return Governor(config);
}

int command_evaluate(const tools::Arguments& arguments) {
    const synthetic::FanInSpec spec = build_spec(arguments);
    Governor governor = make_governor(spec, arguments.has("--recovery"));
    const Decision decision = governor.evaluate(synthetic::build(spec));
    const bool as_json = arguments.has("--json");
    std::fputs(as_json ? decision.explanation.render_json().c_str()
                       : decision.explanation.render_text().c_str(),
               stdout);
    std::fputc('\n', stdout);
    return decision.authority.intervention_authorized ? 0 : 4;
}

struct ClosureCase {
    const char* name;
    synthetic::FanInSpec spec;
    bool recovery_enabled;
    IncidentKind expected_kind;
    bool expect_authorized;
};

int command_selftest() {
    std::vector<ClosureCase> cases;
    cases.push_back({"synchronized-burst", synthetic::synchronized_burst(128), false,
                     IncidentKind::ActiveIncast, true});
    cases.push_back({"unsynchronized-control", synthetic::unsynchronized_many_to_one(128), false,
                     IncidentKind::OrdinaryManyToOne, false});
    cases.push_back({"small-fan-in", synthetic::unsynchronized_many_to_one(3), false,
                     IncidentKind::NoFanIn, false});
    cases.push_back({"capacity-collapse", synthetic::capacity_collapse(256), false,
                     IncidentKind::ActiveIncast, true});
    cases.push_back({"telemetry-gap", synthetic::telemetry_gap(128), false,
                     IncidentKind::Unknown, false});

    synthetic::FanInSpec risk = synthetic::synchronized_burst(64);
    risk.service_rate = Rate{48'000'000'000ULL};   // 1.33x offered: risk band, below collapse
    risk.queue_pressure_bp = 3'000;
    risk.drops = 0;
    cases.push_back({"synchronized-risk", risk, false, IncidentKind::SynchronizedIncastRisk, true});

    synthetic::FanInSpec protected_only = synthetic::synchronized_burst(32);
    protected_only.protected_senders = 32;
    cases.push_back({"protected-only", protected_only, true, IncidentKind::ActiveIncast, true});

    synthetic::FanInSpec saturated = synthetic::synchronized_burst(64);
    saturated.omit_senders = 16;
    saturated.mark_population_saturated = true;
    cases.push_back({"saturated-population", saturated, false, IncidentKind::ActiveIncast, true});

    int failures = 0;
    for (const auto& closure : cases) {
        Governor governor = make_governor(closure.spec, closure.recovery_enabled);
        const Decision decision = governor.evaluate(synthetic::build(closure.spec));
        const bool kind_ok = decision.kind == closure.expected_kind;
        const bool authority_ok = decision.authority.intervention_authorized == closure.expect_authorized;
        const bool scope_ok =
            !decision.metrics.population_saturated || decision.intent.penalized_senders.empty();
        const bool ok = kind_ok && authority_ok && scope_ok;
        if (!ok) failures += 1;
        std::printf("%-24s kind=%-24s expected=%-24s authorized=%-5s mitigation=%-30s %s\n",
                    closure.name, std::string(to_string(decision.kind)).c_str(),
                    std::string(to_string(closure.expected_kind)).c_str(),
                    decision.authority.intervention_authorized ? "true" : "false",
                    std::string(to_string(decision.intent.kind)).c_str(), ok ? "PASS" : "FAIL");
    }
    std::printf("\nselftest: %zu scenarios, %d failures\n", cases.size(), failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const tools::Arguments arguments = tools::parse_arguments(argc, argv);
    if (arguments.tokens.empty()) {
        print_usage();
        return 1;
    }
    const std::string command = tools::find_command(
        arguments, {"--senders", "--spread-ns", "--window-ns", "--per-sender-bps", "--service-bps",
                    "--queue-pressure-bp", "--drops", "--protected-senders", "--protected-class",
                    "--duplicate-senders", "--omit-senders", "--fan-in-state", "--seed"});
    if (command.empty()) {
        print_usage();
        return 1;
    }
    if (command == "version") {
        std::printf("%s %s (ABI %u, wire %u)\n", std::string(kProductName).c_str(),
                    std::string(kProductVersion).c_str(), kAbiVersion,
                    static_cast<unsigned>(kWireProtocolVersion));
        return 0;
    }
    if (command == "evaluate") return command_evaluate(arguments);
    if (command == "selftest") return command_selftest();
    if (command == "help" || command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }
    std::fprintf(stderr, "unknown command: %s\n", command.c_str());
    print_usage();
    return 1;
}
