// Incast Governor worker process.
//
// Connects to a coordinator, acquires a destination lease, evaluates synthetic
// evidence windows through the governor and publishes every decision back to
// the coordinator. Rejections are reported, never hidden.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "command_line.hpp"
#include "incast/coordinator.hpp"
#include "incast/durable.hpp"
#include "incast/governor.hpp"
#include "incast/synthetic.hpp"

namespace {

using namespace incast;

struct WorkerOptions {
    WorkerConfig worker{};
    synthetic::FanInSpec spec{};
    std::uint32_t cycles = 3;
    std::int64_t sleep_ms = 0;
    bool recovery = false;
    bool durable = false;
    DurableConfig durable_config{};
};

synthetic::FanInSpec build_spec(const tools::Arguments& arguments) {
    synthetic::FanInSpec spec{};
    spec.destination = DestinationId{arguments.u64("--destination", 1)};
    spec.destination_generation = DestinationGeneration{arguments.u64("--destination-generation", 1)};
    spec.senders = static_cast<std::uint32_t>(arguments.u64("--senders", 128));
    spec.arrival_spread = Duration{arguments.i64("--spread-ns", 20'000)};
    spec.window_width = Duration{arguments.i64("--window-ns", 1'000'000)};
    spec.per_sender_rate = Rate{arguments.u64("--per-sender-bps", 1'000'000'000ULL)};
    spec.service_rate = Rate{arguments.u64("--service-bps", 40'000'000'000ULL)};
    spec.queue_pressure_bp = static_cast<std::uint32_t>(arguments.u64("--queue-pressure-bp", 9'200));
    spec.drops = arguments.u64("--drops", 16);
    spec.protected_senders = static_cast<std::uint32_t>(arguments.u64("--protected-senders", 0));
    spec.seed = arguments.u64("--seed", 0x5EEDC0DEULL);
    spec.label.assign(arguments.value("--scenario").value_or("synchronized-burst"));
    if (spec.label == "unsynchronized") {
        spec = synthetic::unsynchronized_many_to_one(spec.senders);
    }
    return spec;
}

}  // namespace

int main(int argc, char** argv) {
    const tools::Arguments arguments = tools::parse_arguments(argc, argv);
    if (arguments.has("--help")) {
        std::puts("incast-governor-worker --port N --name NAME [--destination N] [--cycles N]");
        std::puts("                      [--sleep-ms N] [--senders N] [--spread-ns N] [--service-bps N]");
        std::puts("                      [--queue-pressure-bp N] [--drops N] [--protected-senders N]");
        std::puts("                      [--recovery] [--scenario synchronized-burst|unsynchronized]");
        return 0;
    }

    WorkerOptions options{};
    options.spec = build_spec(arguments);
    options.cycles = static_cast<std::uint32_t>(arguments.u64("--cycles", 3));
    options.sleep_ms = arguments.i64("--sleep-ms", 0);
    options.recovery = arguments.has("--recovery");
    options.worker.coordinator_port = static_cast<std::uint16_t>(arguments.u64("--port", 0));
    options.worker.name = arguments.value("--name").value_or("worker");
    options.worker.destination = options.spec.destination;
    options.worker.destination_generation = options.spec.destination_generation;
    options.worker.boot = mint_boot_incarnation();
    options.worker.worker = mint_worker_id(options.worker.name);
    options.worker.policy = options.spec.policy;
    options.worker.policy_generation = options.spec.policy_generation;
    if (const auto directory = arguments.value("--state-dir")) {
        options.durable = true;
        options.durable_config.directory = *directory;
        options.durable_config.fsync_on_commit = true;
    }

    GovernorWorker worker(options.worker);
    const Status connected = worker.connect_and_handshake();
    if (!connected) {
        std::fprintf(stderr, "handshake failed: %s\n", connected.error().detail.c_str());
        return 1;
    }

    auto grant = worker.acquire_lease();
    if (!grant) {
        std::printf("lease=denied reason=%s detail=%s epoch=%llu\n",
                    std::string(to_string(RefusalReason::EpochMismatch)).c_str(),
                    grant.error().detail.c_str(),
                    static_cast<unsigned long long>(worker.epoch().value()));
        std::fflush(stdout);
        return 2;
    }

    options.spec.epoch = grant.value().epoch;
    options.spec.boot = grant.value().boot;
    options.spec.worker = grant.value().worker;
    options.spec.lease = grant.value().lease;
    options.spec.destination = grant.value().destination;
    options.spec.destination_generation = grant.value().destination_generation;
    options.spec.policy = grant.value().policy;
    options.spec.policy_generation = grant.value().policy_generation;

    GovernorConfig governor_config{};
    governor_config.policy = make_default_policy();
    governor_config.policy.recovery.enabled = options.recovery;
    governor_config.policy.recovery.required_clean_windows = 1;
    governor_config.destination = options.spec.destination;
    governor_config.destination_generation = options.spec.destination_generation;
    governor_config.worker = options.spec.worker;
    governor_config.lease = options.spec.lease;
    governor_config.epoch = options.spec.epoch;
    governor_config.boot = options.spec.boot;
    governor_config.lease_expiry = grant.value().expires_at;
    Governor governor(governor_config);

    // Durable state is recovered before authority is bound, so a restarted
    // worker can never resume enforcement from a previous incarnation: the
    // restore clears the binding and moves live interventions to Revalidating.
    DurableStore store;
    std::uint32_t revalidating = 0;
    bool restored = false;
    if (options.durable) {
        const Status opened = store.open(options.durable_config);
        if (!opened) {
            std::fprintf(stderr, "durable store open failed: %s\n", opened.error().detail.c_str());
            return 1;
        }
        auto report = store.recover();
        if (!report) {
            std::fprintf(stderr, "durable recovery failed: %s\n", report.error().detail.c_str());
            return 1;
        }
        restored = report.value().recovered_committed_state;
        revalidating = report.value().interventions_requiring_revalidation;
        if (restored) {
            if (report.value().state.destination.valid() &&
                report.value().state.destination != options.spec.destination) {
                std::fprintf(stderr, "durable state belongs to a different destination\n");
                return 1;
            }
            const Status applied = governor.restore_state(report.value().state);
            if (!applied) {
                std::fprintf(stderr, "durable restore failed: %s\n", applied.error().detail.c_str());
                return 1;
            }
            const Status rebound = governor.bind_authority(
                options.spec.epoch, options.spec.boot, options.spec.worker, options.spec.lease,
                grant.value().expires_at);
            if (!rebound) {
                std::fprintf(stderr, "authority rebind failed: %s\n", rebound.error().detail.c_str());
                return 1;
            }
        }
    }

    std::printf("ready name=%s worker=%llu boot=%llu epoch=%llu lease=%llu restored=%s revalidating=%u\n",
                options.worker.name.c_str(),
                static_cast<unsigned long long>(worker.worker().value()),
                static_cast<unsigned long long>(worker.boot().value()),
                static_cast<unsigned long long>(worker.epoch().value()),
                static_cast<unsigned long long>(grant.value().lease.value()),
                restored ? "true" : "false", revalidating);
    std::fflush(stdout);

    int exit_code = 0;
    for (std::uint32_t cycle = 0; cycle < options.cycles; ++cycle) {
        if (options.sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.sleep_ms));
        }
        synthetic::FanInSpec spec = options.spec;
        spec.now = monotonic_now();
        spec.window = WindowId{cycle + 1};
        spec.window_generation = WindowGeneration{cycle + 1};
        spec.event = EventId{cycle + 1};
        spec.event_generation = EventGeneration{cycle + 1};
        spec.sequence = cycle + 1;

        const Decision decision = governor.evaluate(synthetic::build(spec));
        auto ack = worker.publish(decision);
        if (!ack) {
            std::printf("cycle=%u transport=error detail=%s\n", cycle, ack.error().detail.c_str());
            std::fflush(stdout);
            return 4;
        }
        std::printf("cycle=%u kind=%s outcome=%s mitigation=%s reduction_bp=%u penalized=%zu accepted=%s "
                    "reason=%s epoch=%llu\n",
                    cycle, std::string(to_string(decision.kind)).c_str(),
                    std::string(to_string(decision.outcome)).c_str(),
                    std::string(to_string(decision.intent.kind)).c_str(),
                    decision.intent.reduction_bp.value(), decision.intent.penalized_senders.size(),
                    ack.value().accepted ? "true" : "false",
                    std::string(to_string(ack.value().reason)).c_str(),
                    static_cast<unsigned long long>(ack.value().current_epoch.value()));
        std::fflush(stdout);
        if (!ack.value().accepted) {
            if (worker.fenced()) {
                std::printf("fenced epoch=%llu\n",
                            static_cast<unsigned long long>(worker.epoch().value()));
                std::fflush(stdout);
                return 3;
            }
            exit_code = 5;
        }
        if (options.durable) {
            // The decision is published before it is journalled: an enforcement
            // intent that was never delivered must not be replayed from durable
            // state after a restart.
            const Status committed = store.commit(governor.snapshot_state());
            if (!committed) {
                std::fprintf(stderr, "durable commit failed: %s\n", committed.error().detail.c_str());
                return 1;
            }
        }
    }
    return exit_code;
}
