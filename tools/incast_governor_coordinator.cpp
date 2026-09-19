// Incast Governor coordinator process.
//
// Owns the epoch and the destination lease table. Prints a single machine
// readable readiness line on stdout and then serves framed TCP sessions until
// a control shutdown arrives or the process is terminated.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <string>

#include "command_line.hpp"
#include "incast/coordinator.hpp"

int main(int argc, char** argv) {
    using namespace incast;
    const tools::Arguments arguments = tools::parse_arguments(argc, argv);
    if (arguments.has("--help")) {
        std::puts("incast-governor-coordinator --port N --token T [--state-dir DIR] [--lease-ms N]");
        return 0;
    }

    CoordinatorConfig config{};
    config.port = static_cast<std::uint16_t>(arguments.u64("--port", 0));
    config.control_token = arguments.value("--token").value_or("incast-control");
    config.lease_duration = Duration{arguments.i64("--lease-ms", 2'000) * 1'000'000};
    config.max_connections = static_cast<std::size_t>(arguments.u64("--max-connections", 64));
    if (const auto directory = arguments.value("--state-dir")) {
        config.directory = *directory;
    }

    Coordinator coordinator(config);
    const Status started = coordinator.start();
    if (!started) {
        std::fprintf(stderr, "coordinator start failed: %s\n", started.error().detail.c_str());
        return 1;
    }

    std::printf("ready port=%u boot=%llu epoch=%llu\n", static_cast<unsigned>(coordinator.port()),
                static_cast<unsigned long long>(coordinator.boot().value()),
                static_cast<unsigned long long>(coordinator.epoch().value()));
    std::fflush(stdout);

    const Status ran = coordinator.run();
    const auto statistics = coordinator.statistics();
    std::printf("stopped connections=%llu leases_granted=%llu leases_denied=%llu decisions_accepted=%llu "
                "decisions_rejected=%llu epoch_advances=%llu frames_in=%llu frames_out=%llu\n",
                static_cast<unsigned long long>(statistics.connections),
                static_cast<unsigned long long>(statistics.leases_granted),
                static_cast<unsigned long long>(statistics.leases_denied),
                static_cast<unsigned long long>(statistics.decisions_accepted),
                static_cast<unsigned long long>(statistics.decisions_rejected),
                static_cast<unsigned long long>(statistics.epoch_advances),
                static_cast<unsigned long long>(statistics.frames_in),
                static_cast<unsigned long long>(statistics.frames_out));
    std::fflush(stdout);
    if (!ran) {
        std::fprintf(stderr, "coordinator stopped with error: %s\n", ran.error().detail.c_str());
        return 1;
    }
    return 0;
}
