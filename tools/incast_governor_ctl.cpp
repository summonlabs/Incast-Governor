// Incast Governor control client: status, epoch advance, revoke and shutdown.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <string>

#include "command_line.hpp"
#include "incast/coordinator.hpp"

int main(int argc, char** argv) {
    using namespace incast;
    const tools::Arguments arguments = tools::parse_arguments(argc, argv);
    const auto port_text = arguments.value("--port");
    if (!port_text) {
        std::puts("incast-governor-ctl --port N [--token T] <status|advance-epoch|revoke-all|shutdown> [reason]");
        return 1;
    }
    const auto port = parse_u64(*port_text);
    if (!port || *port == 0 || *port > 65535) {
        std::fprintf(stderr, "invalid port\n");
        return 1;
    }

    ControlRequest request{};
    request.token = arguments.value("--token").value_or("incast-control");
    request.argument = arguments.value("--reason").value_or("control");

    const std::string command = tools::find_command(arguments, {"--port", "--token", "--reason"});
    if (command.empty()) {
        std::fprintf(stderr, "missing control command\n");
        return 1;
    }
    if (command == "status") request.opcode = ControlOpcode::Status;
    else if (command == "advance-epoch") request.opcode = ControlOpcode::AdvanceEpoch;
    else if (command == "revoke-all") request.opcode = ControlOpcode::RevokeAll;
    else if (command == "shutdown") request.opcode = ControlOpcode::Shutdown;
    else {
        std::fprintf(stderr, "unknown control command: %s\n", command.c_str());
        return 1;
    }

    auto response = send_control(static_cast<std::uint16_t>(*port), request);
    if (!response) {
        std::fprintf(stderr, "control request failed: %s\n", response.error().detail.c_str());
        return 2;
    }
    std::printf("ok=%s epoch=%llu leases=%llu accepted=%llu rejected=%llu detail=%s\n",
                response.value().ok ? "true" : "false",
                static_cast<unsigned long long>(response.value().epoch.value()),
                static_cast<unsigned long long>(response.value().lease_count),
                static_cast<unsigned long long>(response.value().decisions_accepted),
                static_cast<unsigned long long>(response.value().decisions_rejected),
                response.value().detail.c_str());
    return response.value().ok ? 0 : 3;
}
