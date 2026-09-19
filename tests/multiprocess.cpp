// Incast Governor - real multiprocess closure tests.
//
// Every claim here is made against genuine OS processes talking over real
// loopback TCP with length-prefixed, CRC-checked frames. Nothing is simulated
// in-process.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "incast/core.hpp"
#include "incast/ids.hpp"
#include "process_harness.hpp"
#include "test_framework.hpp"

namespace {

std::string g_coordinator;
std::string g_worker;
std::string g_ctl;
std::string g_state_directory;

[[nodiscard]] std::string extract_value(const std::string& line, const std::string& key) {
    const std::size_t position = line.find(key);
    if (position == std::string::npos) return {};
    std::size_t start = position + key.size();
    std::size_t end = line.find(' ', start);
    if (end == std::string::npos) end = line.size();
    return line.substr(start, end - start);
}

struct CoordinatorProcess {
    igprocess::ChildProcess process{};
    std::uint16_t port = 0;
    std::uint64_t boot = 0;
    std::uint64_t epoch = 0;

    bool start(const std::string& state_directory, const std::string& token) {
        std::vector<std::string> arguments{"--port", "0", "--token", token, "--lease-ms", "5000"};
        if (!state_directory.empty()) {
            arguments.push_back("--state-dir");
            arguments.push_back(state_directory);
        }
        if (!process.start(g_coordinator, arguments)) return false;
        std::string line;
        if (process.read_line(line) != igprocess::ReadResult::Line) return false;
        if (line.rfind("ready ", 0) != 0) {
            std::fprintf(stderr, "unexpected coordinator readiness line: %s\n", line.c_str());
            return false;
        }
        const auto port_text = extract_value(line, "port=");
        const auto boot_text = extract_value(line, "boot=");
        const auto epoch_text = extract_value(line, "epoch=");
        const auto port_value = incast::parse_u64(port_text);
        const auto boot_value = incast::parse_u64(boot_text);
        const auto epoch_value = incast::parse_u64(epoch_text);
        if (!port_value || !boot_value || !epoch_value) return false;
        port = static_cast<std::uint16_t>(*port_value);
        boot = *boot_value;
        epoch = *epoch_value;
        return port != 0;
    }
};

}  // namespace

IG_TEST(multiprocess, command_line_surface_is_provided) {
    const auto coordinator = std::filesystem::path(g_coordinator);
    const auto worker = std::filesystem::path(g_worker);
    const auto ctl = std::filesystem::path(g_ctl);
    IG_CHECK(std::filesystem::exists(coordinator));
    IG_CHECK(std::filesystem::exists(worker));
    IG_CHECK(std::filesystem::exists(ctl));
}

IG_TEST(multiprocess, epoch_advance_fences_a_running_worker_process) {
    const std::string token = "mp-token-1";
    CoordinatorProcess coordinator;
    IG_REQUIRE(coordinator.start("", token));

    igprocess::ChildProcess worker;
    IG_REQUIRE(worker.start(g_worker, {"--port", std::to_string(coordinator.port), "--name", "mp-worker-1",
                                       "--destination", "1", "--cycles", "40", "--sleep-ms", "25",
                                       "--senders", "96"}));
    std::string line;
    IG_REQUIRE(worker.read_line(line) == igprocess::ReadResult::Line);
    IG_CHECK(line.rfind("ready ", 0) == 0);
    const auto epoch_text = incast::parse_u64(extract_value(line, "epoch="));
    IG_REQUIRE(epoch_text.has_value());
    IG_CHECK_EQ(*epoch_text, coordinator.epoch);

    // The first decision is authoritative and accepted by the coordinator.
    IG_REQUIRE(worker.read_line(line) == igprocess::ReadResult::Line);
    IG_CHECK(line.find("accepted=true") != std::string::npos);

    // Advance the coordinator epoch out of band; the running worker is fenced.
    igprocess::ChildProcess ctl;
    IG_REQUIRE(ctl.start(g_ctl, {"--port", std::to_string(coordinator.port), "--token", token,
                                 "advance-epoch", "--reason", "multiprocess-test"}));
    IG_REQUIRE(ctl.read_line(line) == igprocess::ReadResult::Line);
    IG_CHECK(line.find("ok=true") != std::string::npos);
    IG_CHECK_EQ(ctl.wait(), 0);

    bool saw_rejection = false;
    bool saw_fence_line = false;
    while (worker.read_line(line) == igprocess::ReadResult::Line) {
        if (line.find("accepted=false") != std::string::npos) saw_rejection = true;
        if (line.rfind("fenced ", 0) == 0) {
            saw_fence_line = true;
            break;
        }
    }
    IG_CHECK(saw_rejection);
    IG_CHECK(saw_fence_line);
    IG_CHECK_EQ(worker.wait(), 3);

    igprocess::ChildProcess shutdown;
    static_cast<void>(shutdown.start(g_ctl, {"--port", std::to_string(coordinator.port), "--token", token,
                                             "shutdown"}));
    static_cast<void>(shutdown.wait());
    static_cast<void>(coordinator.process.wait());
}

IG_TEST(multiprocess, hard_kill_then_new_incarnation_advances_the_epoch) {
    const std::string token = "mp-token-2";
    CoordinatorProcess coordinator;
    IG_REQUIRE(coordinator.start("", token));

    igprocess::ChildProcess victim;
    IG_REQUIRE(victim.start(g_worker, {"--port", std::to_string(coordinator.port), "--name", "victim",
                                       "--destination", "7", "--cycles", "400", "--sleep-ms", "20"}));
    std::string line;
    IG_REQUIRE(victim.read_line(line) == igprocess::ReadResult::Line);
    const auto victim_epoch = incast::parse_u64(extract_value(line, "epoch="));
    IG_REQUIRE(victim_epoch.has_value());

    // Hard, uncatchable kill: the worker never gets to clean up.
    victim.terminate();
    const int victim_code = victim.wait();
    IG_CHECK(victim_code != 0);
    victim.close();

    igprocess::ChildProcess successor;
    IG_REQUIRE(successor.start(g_worker, {"--port", std::to_string(coordinator.port), "--name", "successor",
                                          "--destination", "7", "--cycles", "2", "--sleep-ms", "0"}));
    IG_REQUIRE(successor.read_line(line) == igprocess::ReadResult::Line);
    IG_CHECK(line.rfind("ready ", 0) == 0);
    const auto successor_epoch = incast::parse_u64(extract_value(line, "epoch="));
    IG_REQUIRE(successor_epoch.has_value());
    // A different live incarnation claiming the same destination supersedes the
    // previous holder, so the epoch strictly advances.
    IG_CHECK(*successor_epoch > *victim_epoch);
    IG_REQUIRE(successor.read_line(line) == igprocess::ReadResult::Line);
    IG_CHECK(line.find("accepted=true") != std::string::npos);
    IG_CHECK_EQ(successor.wait(), 0);

    igprocess::ChildProcess shutdown;
    static_cast<void>(shutdown.start(g_ctl, {"--port", std::to_string(coordinator.port), "--token", token,
                                             "shutdown"}));
    static_cast<void>(shutdown.wait());
    static_cast<void>(coordinator.process.wait());
}

IG_TEST(multiprocess, coordinator_restart_never_reuses_an_epoch) {
    const std::string token = "mp-token-3";
    const std::string state_directory = g_state_directory;

    CoordinatorProcess first;
    IG_REQUIRE(first.start(state_directory, token));
    const std::uint64_t first_boot = first.boot;
    const std::uint64_t first_epoch = first.epoch;

    // Run a worker so at least one grant and decision is durably accounted for.
    igprocess::ChildProcess worker;
    IG_REQUIRE(worker.start(g_worker, {"--port", std::to_string(first.port), "--name", "restart-worker",
                                       "--destination", "11", "--cycles", "2", "--sleep-ms", "0"}));
    std::string line;
    IG_REQUIRE(worker.read_line(line) == igprocess::ReadResult::Line);
    IG_REQUIRE(worker.read_line(line) == igprocess::ReadResult::Line);
    IG_CHECK(line.find("accepted=true") != std::string::npos);
    IG_CHECK_EQ(worker.wait(), 0);

    igprocess::ChildProcess status;
    IG_REQUIRE(status.start(g_ctl, {"--port", std::to_string(first.port), "--token", token, "status"}));
    IG_REQUIRE(status.read_line(line) == igprocess::ReadResult::Line);
    const auto accepted = incast::parse_u64(extract_value(line, "accepted="));
    IG_REQUIRE(accepted.has_value());
    IG_CHECK(*accepted >= 1);
    IG_CHECK_EQ(status.wait(), 0);

    // Hard kill: no clean shutdown, no chance to write anything on the way out.
    first.process.terminate();
    static_cast<void>(first.process.wait());
    first.process.close();

    CoordinatorProcess second;
    IG_REQUIRE(second.start(state_directory, token));
    IG_CHECK(second.boot != first_boot);
    IG_CHECK(second.epoch > first_epoch);

    // A worker connecting to the restarted coordinator receives a strictly
    // newer epoch than anything issued before the crash.
    igprocess::ChildProcess after;
    IG_REQUIRE(after.start(g_worker, {"--port", std::to_string(second.port), "--name", "after-restart",
                                      "--destination", "11", "--cycles", "2", "--sleep-ms", "0"}));
    IG_REQUIRE(after.read_line(line) == igprocess::ReadResult::Line);
    const auto after_epoch = incast::parse_u64(extract_value(line, "epoch="));
    IG_REQUIRE(after_epoch.has_value());
    IG_CHECK(*after_epoch > first_epoch);
    IG_REQUIRE(after.read_line(line) == igprocess::ReadResult::Line);
    IG_CHECK(line.find("accepted=true") != std::string::npos);
    IG_CHECK_EQ(after.wait(), 0);

    igprocess::ChildProcess final_shutdown;
    static_cast<void>(final_shutdown.start(
        g_ctl, {"--port", std::to_string(second.port), "--token", token, "shutdown"}));
    static_cast<void>(final_shutdown.wait());
    static_cast<void>(second.process.wait());
}

IG_TEST(multiprocess, worker_durable_state_is_recovered_but_never_resumes_authority) {
    const std::string token = "mp-token-5";
    const std::string state_directory = g_state_directory + "/worker-state";
    std::error_code error;
    std::filesystem::remove_all(state_directory, error);

    CoordinatorProcess coordinator;
    IG_REQUIRE(coordinator.start("", token));

    igprocess::ChildProcess first;
    IG_REQUIRE(first.start(g_worker,
                           {"--port", std::to_string(coordinator.port), "--name", "durable-1",
                            "--destination", "21", "--cycles", "2", "--sleep-ms", "0",
                            "--state-dir", state_directory}));
    std::string line;
    IG_REQUIRE(first.read_line(line) == igprocess::ReadResult::Line);
    IG_CHECK(line.find("restored=false") != std::string::npos);
    IG_CHECK(line.find("revalidating=0") != std::string::npos);
    IG_REQUIRE(first.read_line(line) == igprocess::ReadResult::Line);
    IG_CHECK(line.find("outcome=InterventionIssued") != std::string::npos);
    IG_CHECK(line.find("accepted=true") != std::string::npos);
    IG_CHECK_EQ(first.wait(), 0);

    // A brand-new process recovers the committed state: the intervention is a
    // revalidation candidate, never a live enforcement authority.
    igprocess::ChildProcess second;
    IG_REQUIRE(second.start(g_worker,
                            {"--port", std::to_string(coordinator.port), "--name", "durable-2",
                             "--destination", "21", "--cycles", "2", "--sleep-ms", "0",
                             "--state-dir", state_directory}));
    IG_REQUIRE(second.read_line(line) == igprocess::ReadResult::Line);
    IG_CHECK(line.find("restored=true") != std::string::npos);
    IG_CHECK(line.find("revalidating=1") != std::string::npos);

    // The recovered intervention belongs to the superseded incarnation and is
    // fenced; a fresh intervention is only issued from fresh evidence.
    IG_REQUIRE(second.read_line(line) == igprocess::ReadResult::Line);
    IG_CHECK(line.find("outcome=InterventionFenced") != std::string::npos);
    IG_REQUIRE(second.read_line(line) == igprocess::ReadResult::Line);
    IG_CHECK(line.find("outcome=InterventionIssued") != std::string::npos);
    IG_CHECK_EQ(second.wait(), 0);

    igprocess::ChildProcess shutdown;
    static_cast<void>(shutdown.start(g_ctl, {"--port", std::to_string(coordinator.port), "--token", token,
                                             "shutdown"}));
    static_cast<void>(shutdown.wait());
    static_cast<void>(coordinator.process.wait());
    std::filesystem::remove_all(state_directory, error);
}

IG_TEST(multiprocess, control_plane_rejects_a_bad_token_across_processes) {
    const std::string token = "mp-token-4";
    CoordinatorProcess coordinator;
    IG_REQUIRE(coordinator.start("", token));

    igprocess::ChildProcess ctl;
    IG_REQUIRE(ctl.start(g_ctl, {"--port", std::to_string(coordinator.port), "--token", "not-the-token",
                                 "advance-epoch"}));
    std::string line;
    IG_REQUIRE(ctl.read_line(line) == igprocess::ReadResult::Line);
    IG_CHECK(line.find("ok=false") != std::string::npos);
    IG_CHECK_EQ(ctl.wait(), 3);

    igprocess::ChildProcess shutdown;
    static_cast<void>(shutdown.start(g_ctl, {"--port", std::to_string(coordinator.port), "--token", token,
                                             "shutdown"}));
    static_cast<void>(shutdown.wait());
    static_cast<void>(coordinator.process.wait());
}

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        const std::string token = argv[index];
        if (token == "--coordinator" && index + 1 < argc) g_coordinator = argv[++index];
        else if (token == "--worker" && index + 1 < argc) g_worker = argv[++index];
        else if (token == "--ctl" && index + 1 < argc) g_ctl = argv[++index];
        else if (token == "--state-dir" && index + 1 < argc) g_state_directory = argv[++index];
    }
    if (g_state_directory.empty()) {
        g_state_directory =
            (std::filesystem::temp_directory_path() / "incast-governor-multiprocess").string();
    }
    std::error_code error;
    std::filesystem::remove_all(g_state_directory, error);
    std::filesystem::create_directories(g_state_directory, error);

    if (g_coordinator.empty() || g_worker.empty() || g_ctl.empty()) {
        std::fprintf(stderr,
                     "usage: incast_governor_multiprocess_tests --coordinator PATH --worker PATH --ctl PATH "
                     "[--state-dir DIR]\n");
        return 1;
    }
    const int result = ::igtest::run_all(argc, argv);
    std::filesystem::remove_all(g_state_directory, error);
    return result;
}
