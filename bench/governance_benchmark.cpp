// Incast Governor - synthetic governance benchmark.
//
// Sweeps sender count, arrival window width, destination capacity and event
// density. Every number produced here is SYNTHETIC: it measures completed
// governance decisions on fabricated evidence and is not a physical-network
// measurement of any kind.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "incast/governor.hpp"
#include "incast/synthetic.hpp"

namespace {

using namespace incast;

struct Sweep {
    std::vector<std::uint32_t> sender_counts{8, 32, 128, 512, 2'048};
    std::vector<std::int64_t> window_widths{100'000, 1'000'000, 10'000'000};
    std::vector<std::uint64_t> service_rates{10'000'000'000ULL, 40'000'000'000ULL, 100'000'000'000ULL,
                                             400'000'000'000ULL};
    std::uint64_t events = 500;
};

struct Row {
    std::uint32_t senders = 0;
    std::int64_t window_width = 0;
    std::uint64_t service_rate = 0;
    std::uint64_t events = 0;
    double seconds = 0.0;
    std::uint64_t active = 0;
    std::uint64_t risk = 0;
    std::uint64_t ordinary = 0;
    std::uint64_t refused = 0;
    std::uint64_t interventions = 0;
    std::uint64_t penalized_total = 0;
};

Row run_combination(const Sweep& sweep, std::uint32_t senders, std::int64_t window_width,
                    std::uint64_t service_rate) {
    Row row{};
    row.senders = senders;
    row.window_width = window_width;
    row.service_rate = service_rate;
    row.events = sweep.events;

    // One governor per destination, exactly as a worker would hold it.
    synthetic::FanInSpec template_spec{};
    template_spec.senders = senders;
    template_spec.window_width = Duration{window_width};
    template_spec.arrival_spread = Duration{window_width / 50};
    template_spec.service_rate = Rate{service_rate};
    template_spec.per_sender_rate = Rate{1'000'000'000};
    template_spec.queue_pressure_bp = 9'200;
    template_spec.drops = 4;
    template_spec.protected_senders = senders / 8;
    template_spec.now = Timestamp{1'000'000'000'000};

    GovernorConfig config{};
    config.policy = make_default_policy();
    config.policy.recovery.enabled = true;
    config.policy.recovery.required_clean_windows = 1;
    config.destination = template_spec.destination;
    config.destination_generation = template_spec.destination_generation;
    config.worker = template_spec.worker;
    config.lease = template_spec.lease;
    config.epoch = template_spec.epoch;
    config.boot = template_spec.boot;
    config.lease_expiry = Timestamp{template_spec.now.nanos() + 1'000'000'000'000};
    Governor governor(config);

    const auto started = std::chrono::steady_clock::now();
    for (std::uint64_t event = 0; event < sweep.events; ++event) {
        synthetic::FanInSpec spec = template_spec;
        spec.window = WindowId{event + 1};
        spec.window_generation = WindowGeneration{event + 1};
        spec.event = EventId{event + 1};
        spec.event_generation = EventGeneration{event + 1};
        spec.sequence = event + 1;
        spec.now = Timestamp{template_spec.now.nanos() + static_cast<std::int64_t>(event) * window_width};
        const Decision decision = governor.evaluate(synthetic::build(spec));
        switch (decision.kind) {
            case IncidentKind::ActiveIncast: row.active += 1; break;
            case IncidentKind::SynchronizedIncastRisk: row.risk += 1; break;
            case IncidentKind::OrdinaryManyToOne: row.ordinary += 1; break;
            case IncidentKind::Unknown: row.refused += 1; break;
            case IncidentKind::NoFanIn:
            case IncidentKind::Recovery: row.ordinary += 1; break;
        }
        if (decision.outcome == DecisionOutcome::InterventionIssued ||
            decision.outcome == DecisionOutcome::InterventionContinued) {
            row.interventions += 1;
        }
        row.penalized_total += decision.intent.penalized_senders.size();
    }
    const auto finished = std::chrono::steady_clock::now();
    row.seconds = std::chrono::duration<double>(finished - started).count();
    return row;
}

}  // namespace

int main(int argc, char** argv) {
    Sweep sweep{};
    for (int index = 1; index < argc; ++index) {
        const std::string token = argv[index];
        if (token == "--events" && index + 1 < argc) {
            const auto value = parse_u64(argv[++index]);
            if (value && *value > 0 && *value <= kMaxScenarioEvents) sweep.events = *value;
        } else if (token == "--senders" && index + 1 < argc) {
            const auto value = parse_u64(argv[++index]);
            if (value && *value > 0 && *value <= kMaxSendersPerWindow) {
                sweep.sender_counts.assign(1, static_cast<std::uint32_t>(*value));
            }
        } else if (token == "--repeats" && index + 1 < argc) {
            static_cast<void>(parse_u64(argv[++index]));
        } else if (token == "--help") {
            std::puts("incast-governor-bench [--events N] [--senders N] [--repeats N]");
            return 0;
        }
    }

    std::printf("Incast Governor synthetic governance benchmark\n");
    std::printf("PROVENANCE: SYNTHETIC. Fabricated evidence; no physical network was measured.\n");
    std::printf("Completed decisions per combination: %llu\n\n",
                static_cast<unsigned long long>(sweep.events));
    std::printf("%-8s %-12s %-14s %-10s %-12s %-10s %-8s %-8s %-8s %-8s\n", "senders", "window_ns",
                "service_bps", "events", "decisions/s", "us/event", "active", "risk", "ordinary", "refused");

    double total_seconds = 0.0;
    std::uint64_t total_events = 0;
    std::uint64_t total_interventions = 0;
    std::uint64_t total_penalized = 0;

    for (const auto senders : sweep.sender_counts) {
        for (const auto width : sweep.window_widths) {
            for (const auto service : sweep.service_rates) {
                const Row row = run_combination(sweep, senders, width, service);
                const double per_event_us = row.seconds * 1.0e6 / static_cast<double>(row.events);
                const double rate = static_cast<double>(row.events) / row.seconds;
                std::printf("%-8u %-12lld %-14llu %-10llu %-12.0f %-10.3f %-8llu %-8llu %-8llu %-8llu\n",
                            senders, static_cast<long long>(width),
                            static_cast<unsigned long long>(service),
                            static_cast<unsigned long long>(row.events), rate, per_event_us,
                            static_cast<unsigned long long>(row.active),
                            static_cast<unsigned long long>(row.risk),
                            static_cast<unsigned long long>(row.ordinary),
                            static_cast<unsigned long long>(row.refused));
                total_seconds += row.seconds;
                total_events += row.events;
                total_interventions += row.interventions;
                total_penalized += row.penalized_total;
            }
        }
    }

    const double aggregate = static_cast<double>(total_events) / total_seconds;
    std::printf("\naggregate: %llu completed decisions in %.3f s = %.0f decisions/s\n",
                static_cast<unsigned long long>(total_events), total_seconds, aggregate);
    std::printf("interventions issued or continued: %llu (%.1f%% of decisions)\n",
                static_cast<unsigned long long>(total_interventions),
                100.0 * static_cast<double>(total_interventions) / static_cast<double>(total_events));
    std::printf("mean penalized senders per decision: %.2f\n",
                static_cast<double>(total_penalized) / static_cast<double>(total_events));
    return 0;
}
