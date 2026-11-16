// Incast Governor - transport and in-process coordinator/worker integration.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "governor_fixture.hpp"
#include "incast/coordinator.hpp"
#include "incast/synthetic.hpp"
#include "incast/transport.hpp"
#include "test_framework.hpp"

using namespace incast;

namespace {

struct RunningCoordinator {
    Coordinator coordinator;
    std::thread thread;

    explicit RunningCoordinator(CoordinatorConfig config) : coordinator(std::move(config)) {}
    ~RunningCoordinator() { shutdown(); }

    bool start() {
        if (!coordinator.start()) return false;
        thread = std::thread([this]() { static_cast<void>(coordinator.run()); });
        return true;
    }
    void shutdown() {
        coordinator.stop();
        if (thread.joinable()) thread.join();
    }
};

}  // namespace

IG_TEST(transport, loopback_round_trip_and_partial_frames) {
    IG_REQUIRE(static_cast<bool>(net::initialize()));
    net::Listener listener;
    IG_REQUIRE(static_cast<bool>(listener.bind_loopback(0)));
    IG_CHECK(listener.bound_port() != 0);

    std::thread server([&listener]() {
        auto connection = listener.accept();
        if (!connection) return;
        FramedChannel channel(std::move(connection.value()));
        // Two frames written back to back in one buffer exercise the reader's
        // partial-read path.
        Frame first{};
        first.type = FrameType::HelloAck;
        first.payload = {std::byte{1}, std::byte{2}};
        Frame second{};
        second.type = FrameType::HeartbeatAck;
        second.payload = std::vector<std::byte>(300, std::byte{7});
        static_cast<void>(channel.send(first));
        static_cast<void>(channel.send(second));
    });

    net::Connection connection;
    IG_REQUIRE(static_cast<bool>(connection.connect_loopback(listener.bound_port())));
    FramedChannel channel(std::move(connection));
    auto first = channel.receive();
    IG_REQUIRE(first.has_value());
    IG_CHECK(first.value().type == FrameType::HelloAck);
    IG_CHECK_EQ(first.value().payload.size(), 2u);
    auto second = channel.receive();
    IG_REQUIRE(second.has_value());
    IG_CHECK(second.value().type == FrameType::HeartbeatAck);
    IG_CHECK_EQ(second.value().payload.size(), 300u);
    IG_CHECK_EQ(channel.frames_received(), 2u);

    channel.close();
    server.join();
    listener.close();
}

IG_TEST(transport, oversized_declared_frame_is_refused) {
    IG_REQUIRE(static_cast<bool>(net::initialize()));
    net::Listener listener;
    IG_REQUIRE(static_cast<bool>(listener.bind_loopback(0)));

    std::thread server([&listener]() {
        auto connection = listener.accept();
        if (!connection) return;
        // Craft a header that declares a payload above the supported bound.
        std::vector<std::byte> header(kFrameHeaderBytes, std::byte{0});
        const auto put_u16 = [&header](std::size_t offset, std::uint16_t value) {
            header[offset] = static_cast<std::byte>(value & 0xFFu);
            header[offset + 1] = static_cast<std::byte>((value >> 8) & 0xFFu);
        };
        const auto put_u32 = [&header](std::size_t offset, std::uint32_t value) {
            for (int index = 0; index < 4; ++index) {
                header[offset + static_cast<std::size_t>(index)] =
                    static_cast<std::byte>((value >> (index * 8)) & 0xFFu);
            }
        };
        put_u32(0, kFrameMagic);
        put_u16(4, kWireProtocolVersion);
        put_u16(6, static_cast<std::uint16_t>(FrameType::Hello));
        put_u32(12, static_cast<std::uint32_t>(kMaxFrameBytes) + 1);
        put_u32(16, 0);
        static_cast<void>(connection.value().send_all(header));
    });

    net::Connection connection;
    IG_REQUIRE(static_cast<bool>(connection.connect_loopback(listener.bound_port())));
    FramedChannel channel(std::move(connection));
    auto frame = channel.receive();
    IG_CHECK(!frame.has_value());
    IG_CHECK(frame.error().code == ErrorCode::OversizedInput);
    IG_CHECK_EQ(channel.protocol_errors(), 1u);

    channel.close();
    server.join();
    listener.close();
}

IG_TEST(integration, worker_acquires_a_lease_and_publishes_an_accepted_decision) {
    CoordinatorConfig config{};
    config.control_token = "integration";
    config.lease_duration = Duration{5'000'000'000};
    RunningCoordinator running(config);
    IG_REQUIRE(running.start());

    WorkerConfig worker_config{};
    worker_config.coordinator_port = running.coordinator.port();
    worker_config.name = "integration-worker";
    worker_config.boot = mint_boot_incarnation();
    worker_config.worker = mint_worker_id(worker_config.name);
    worker_config.destination = DestinationId{1};
    worker_config.destination_generation = DestinationGeneration{1};

    GovernorWorker worker(worker_config);
    IG_REQUIRE(static_cast<bool>(worker.connect_and_handshake()));
    auto grant = worker.acquire_lease();
    IG_REQUIRE(grant.has_value());
    IG_CHECK(grant.value().epoch == running.coordinator.epoch());
    IG_CHECK(grant.value().boot == worker_config.boot);
    IG_CHECK(grant.value().worker == worker_config.worker);

    auto spec = synthetic::synchronized_burst(96);
    spec.epoch = grant.value().epoch;
    spec.boot = grant.value().boot;
    spec.worker = grant.value().worker;
    spec.lease = grant.value().lease;
    auto governor = igfixture::make_governor(spec, true);
    const Decision decision = governor.evaluate(synthetic::build(spec));
    IG_REQUIRE(decision.outcome == DecisionOutcome::InterventionIssued);

    auto ack = worker.publish(decision);
    IG_REQUIRE(ack.has_value());
    IG_CHECK(ack.value().accepted);
    IG_CHECK(ack.value().reason == RefusalReason::None);
    IG_CHECK_EQ(running.coordinator.statistics().decisions_accepted, 1u);
    IG_CHECK(!worker.fenced());

    worker.close();
    running.shutdown();
}

IG_TEST(integration, epoch_advance_fences_a_live_worker) {
    CoordinatorConfig config{};
    config.lease_duration = Duration{5'000'000'000};
    RunningCoordinator running(config);
    IG_REQUIRE(running.start());

    WorkerConfig worker_config{};
    worker_config.coordinator_port = running.coordinator.port();
    worker_config.name = "fenced-worker";
    worker_config.boot = mint_boot_incarnation();
    worker_config.worker = mint_worker_id(worker_config.name);
    worker_config.destination = DestinationId{2};

    GovernorWorker worker(worker_config);
    IG_REQUIRE(static_cast<bool>(worker.connect_and_handshake()));
    auto grant = worker.acquire_lease();
    IG_REQUIRE(grant.has_value());
    const EpochId original = grant.value().epoch;

    auto spec = synthetic::synchronized_burst(64);
    spec.epoch = grant.value().epoch;
    spec.boot = grant.value().boot;
    spec.worker = grant.value().worker;
    spec.lease = grant.value().lease;
    auto governor = igfixture::make_governor(spec, true);
    auto ack = worker.publish(governor.evaluate(synthetic::build(spec)));
    IG_REQUIRE(ack.has_value());
    IG_CHECK(ack.value().accepted);

    IG_REQUIRE(static_cast<bool>(running.coordinator.advance_epoch("test advance")));
    IG_CHECK(running.coordinator.epoch() > original);

    auto next = spec;
    next.window = WindowId{2};
    next.window_generation = WindowGeneration{2};
    next.event = EventId{2};
    next.event_generation = EventGeneration{2};
    next.sequence = 20;
    next.now = Timestamp{spec.now.nanos() + 1'000'000};
    auto second_ack = worker.publish(governor.evaluate(synthetic::build(next)));
    IG_REQUIRE(second_ack.has_value());
    IG_CHECK(!second_ack.value().accepted);
    IG_CHECK(second_ack.value().reason == RefusalReason::EpochMismatch);
    IG_CHECK(worker.fenced());
    IG_CHECK(worker.epoch() == running.coordinator.epoch());

    worker.close();
    running.shutdown();
}

IG_TEST(integration, revoke_all_invalidates_every_lease) {
    CoordinatorConfig config{};
    config.control_token = "revoke";
    config.lease_duration = Duration{5'000'000'000};
    RunningCoordinator running(config);
    IG_REQUIRE(running.start());

    WorkerConfig worker_config{};
    worker_config.coordinator_port = running.coordinator.port();
    worker_config.name = "revoked-worker";
    worker_config.boot = mint_boot_incarnation();
    worker_config.worker = mint_worker_id(worker_config.name);
    worker_config.destination = DestinationId{3};

    GovernorWorker worker(worker_config);
    IG_REQUIRE(static_cast<bool>(worker.connect_and_handshake()));
    auto grant = worker.acquire_lease();
    IG_REQUIRE(grant.has_value());

    ControlRequest control{};
    control.opcode = ControlOpcode::RevokeAll;
    control.token = "revoke";
    auto response = send_control(running.coordinator.port(), control);
    IG_REQUIRE(response.has_value());
    IG_CHECK(response.value().ok);

    auto spec = synthetic::synchronized_burst(32);
    spec.epoch = grant.value().epoch;
    spec.boot = grant.value().boot;
    spec.worker = grant.value().worker;
    spec.lease = grant.value().lease;
    auto governor = igfixture::make_governor(spec, true);
    auto ack = worker.publish(governor.evaluate(synthetic::build(spec)));
    IG_REQUIRE(ack.has_value());
    IG_CHECK(!ack.value().accepted);
    IG_CHECK(ack.value().reason == RefusalReason::EpochMismatch ||
             ack.value().reason == RefusalReason::LeaseMissing);

    worker.close();
    running.shutdown();
}

IG_TEST(integration, a_new_incarnation_supersedes_the_previous_lease) {
    CoordinatorConfig config{};
    config.lease_duration = Duration{5'000'000'000};
    RunningCoordinator running(config);
    IG_REQUIRE(running.start());

    const DestinationId destination{4};
    WorkerConfig first_config{};
    first_config.coordinator_port = running.coordinator.port();
    first_config.name = "first";
    first_config.boot = mint_boot_incarnation();
    first_config.worker = mint_worker_id(first_config.name);
    first_config.destination = destination;
    GovernorWorker first(first_config);
    IG_REQUIRE(static_cast<bool>(first.connect_and_handshake()));
    auto first_grant = first.acquire_lease();
    IG_REQUIRE(first_grant.has_value());

    WorkerConfig second_config = first_config;
    second_config.name = "second";
    second_config.boot = mint_boot_incarnation();
    second_config.worker = mint_worker_id(second_config.name);
    GovernorWorker second(second_config);
    IG_REQUIRE(static_cast<bool>(second.connect_and_handshake()));
    auto second_grant = second.acquire_lease();
    IG_REQUIRE(second_grant.has_value());

    IG_CHECK(second_grant.value().epoch > first_grant.value().epoch);
    IG_CHECK(second_grant.value().lease != first_grant.value().lease);

    // The superseded worker can no longer publish authoritative decisions.
    auto spec = synthetic::synchronized_burst(32);
    spec.epoch = first_grant.value().epoch;
    spec.boot = first_grant.value().boot;
    spec.worker = first_grant.value().worker;
    spec.lease = first_grant.value().lease;
    auto governor = igfixture::make_governor(spec, true);
    auto ack = first.publish(governor.evaluate(synthetic::build(spec)));
    IG_REQUIRE(ack.has_value());
    IG_CHECK(!ack.value().accepted);

    first.close();
    second.close();
    running.shutdown();
}

IG_TEST(integration, heartbeat_reports_epoch_advances_to_the_worker) {
    CoordinatorConfig config{};
    config.lease_duration = Duration{5'000'000'000};
    RunningCoordinator running(config);
    IG_REQUIRE(running.start());

    WorkerConfig worker_config{};
    worker_config.coordinator_port = running.coordinator.port();
    worker_config.name = "heartbeat";
    worker_config.boot = mint_boot_incarnation();
    worker_config.worker = mint_worker_id(worker_config.name);
    worker_config.destination = DestinationId{5};
    GovernorWorker worker(worker_config);
    IG_REQUIRE(static_cast<bool>(worker.connect_and_handshake()));
    IG_REQUIRE(worker.acquire_lease().has_value());
    IG_REQUIRE(static_cast<bool>(worker.send_heartbeat()));
    IG_CHECK(!worker.fenced());

    IG_REQUIRE(static_cast<bool>(running.coordinator.advance_epoch("heartbeat advance")));
    IG_REQUIRE(static_cast<bool>(worker.send_heartbeat()));
    IG_CHECK(worker.fenced());
    IG_CHECK(worker.epoch() == running.coordinator.epoch());

    worker.close();
    running.shutdown();
}

namespace {

class LedgerDirectory {
public:
    explicit LedgerDirectory(const std::string& label) {
        static std::uint64_t counter = 0;
        counter += 1;
        path_ = std::filesystem::temp_directory_path() /
                ("incast-governor-ledger-" + label + "-" + std::to_string(counter));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
    }
    ~LedgerDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    LedgerDirectory(const LedgerDirectory&) = delete;
    LedgerDirectory& operator=(const LedgerDirectory&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_{};
};

WorkerConfig worker_config_for(const Coordinator& coordinator, const std::string& name,
                               std::uint64_t destination) {
    WorkerConfig config{};
    config.coordinator_port = coordinator.port();
    config.name = name;
    config.boot = mint_boot_incarnation();
    config.worker = mint_worker_id(name);
    config.destination = DestinationId{destination};
    config.destination_generation = DestinationGeneration{1};
    return config;
}

// Publishes one authoritative decision under the worker's own grant.
Outcome<MessageDecisionAck> publish_one(GovernorWorker& worker, const MessageLeaseGrant& grant) {
    auto spec = synthetic::synchronized_burst(48);
    spec.destination = grant.destination;
    spec.epoch = grant.epoch;
    spec.boot = grant.boot;
    spec.worker = grant.worker;
    spec.lease = grant.lease;
    auto governor = igfixture::make_governor(spec, true);
    return worker.publish(governor.evaluate(synthetic::build(spec)));
}

EpochId persisted_epoch(const std::filesystem::path& directory) {
    EpochLedger ledger;
    IG_REQUIRE(static_cast<bool>(ledger.open(directory)));
    auto loaded = ledger.load();
    IG_REQUIRE(loaded.has_value());
    return loaded.value();
}

}  // namespace

IG_TEST(integration, revoke_all_makes_the_new_epoch_durable) {
    LedgerDirectory directory("revoke");
    CoordinatorConfig config{};
    config.control_token = "revoke-durable";
    config.directory = directory.path();
    RunningCoordinator running(config);
    IG_REQUIRE(running.start());
    const EpochId before = running.coordinator.epoch();
    IG_CHECK_EQ(persisted_epoch(directory.path()).value(), before.value());

    ControlRequest control{};
    control.opcode = ControlOpcode::RevokeAll;
    control.token = "revoke-durable";
    auto response = send_control(running.coordinator.port(), control);
    IG_REQUIRE(response.has_value());
    IG_CHECK(response.value().ok);
    const EpochId after = running.coordinator.epoch();
    IG_CHECK(after > before);

    // Leases are granted under the new epoch from here on. A coordinator that
    // is killed now has to restart strictly above it, so it must be on disk.
    IG_CHECK_EQ(persisted_epoch(directory.path()).value(), after.value());
    running.shutdown();
}

IG_TEST(integration, an_epoch_that_cannot_be_made_durable_is_never_issued) {
    LedgerDirectory directory("unwritable");
    CoordinatorConfig config{};
    config.control_token = "ledger-failure";
    config.directory = directory.path();
    config.lease_duration = Duration{5'000'000'000};
    RunningCoordinator running(config);
    IG_REQUIRE(running.start());

    GovernorWorker holder(worker_config_for(running.coordinator, "holder", 31));
    IG_REQUIRE(static_cast<bool>(holder.connect_and_handshake()));
    auto grant = holder.acquire_lease();
    IG_REQUIRE(grant.has_value());
    const EpochId before = running.coordinator.epoch();

    // From here the ledger cannot be rewritten: the name of its temporary file
    // is taken by a directory.
    std::error_code error;
    std::filesystem::create_directories(directory.path() / "coordinator.epoch.tmp", error);
    IG_REQUIRE(!error);

    const Status advanced = running.coordinator.advance_epoch("ledger is unwritable");
    IG_CHECK(!advanced);
    IG_CHECK_EQ(running.coordinator.epoch().value(), before.value());

    ControlRequest control{};
    control.opcode = ControlOpcode::RevokeAll;
    control.token = "ledger-failure";
    auto response = send_control(running.coordinator.port(), control);
    IG_REQUIRE(response.has_value());
    IG_CHECK(!response.value().ok);
    IG_CHECK_EQ(running.coordinator.epoch().value(), before.value());

    // A second incarnation claiming the destination would need a new epoch to
    // supersede the holder. It cannot have one, so it is refused and the holder
    // keeps the authority it was granted.
    GovernorWorker claimant(worker_config_for(running.coordinator, "claimant", 31));
    IG_REQUIRE(static_cast<bool>(claimant.connect_and_handshake()));
    IG_CHECK(!claimant.acquire_lease().has_value());
    IG_CHECK_EQ(running.coordinator.epoch().value(), before.value());
    IG_CHECK_EQ(persisted_epoch(directory.path()).value(), before.value());

    auto ack = publish_one(holder, grant.value());
    IG_REQUIRE(ack.has_value());
    IG_CHECK(ack.value().accepted);

    claimant.close();
    holder.close();
    running.shutdown();
}

IG_TEST(integration, a_publish_from_another_incarnation_cannot_revoke_the_holders_lease) {
    CoordinatorConfig config{};
    config.lease_duration = Duration{5'000'000'000};
    RunningCoordinator running(config);
    IG_REQUIRE(running.start());

    GovernorWorker holder(worker_config_for(running.coordinator, "lease-holder", 41));
    IG_REQUIRE(static_cast<bool>(holder.connect_and_handshake()));
    auto grant = holder.acquire_lease();
    IG_REQUIRE(grant.has_value());

    // A process that does not hold the lease names it, under an epoch that is
    // not the coordinator's: a worker left over from an earlier incarnation
    // whose lease number has since been issued to someone else.
    {
        net::Connection connection;
        IG_REQUIRE(static_cast<bool>(connection.connect_loopback(running.coordinator.port())));
        FramedChannel channel(std::move(connection));
        MessageDecisionPublish stale{};
        stale.lease = grant.value().lease;
        stale.epoch = EpochId{grant.value().epoch.value() + 7};
        stale.boot = mint_boot_incarnation();
        stale.worker = mint_worker_id("left-over");
        stale.destination = DestinationId{41};
        stale.window = WindowId{1};
        stale.window_generation = WindowGeneration{1};
        stale.sequence = 1;
        Frame frame{};
        frame.type = FrameType::DecisionPublish;
        frame.payload = encode_decision_publish(stale);
        IG_REQUIRE(static_cast<bool>(channel.send(frame)));
        auto reply = channel.receive();
        IG_REQUIRE(reply.has_value());
        IG_REQUIRE(reply.value().type == FrameType::DecisionAck);
        auto ack = decode_decision_ack(reply.value().payload);
        IG_REQUIRE(ack.has_value());
        IG_CHECK(!ack.value().accepted);
        channel.close();
    }

    // The holder's authority is what the coordinator granted, untouched.
    auto ack = publish_one(holder, grant.value());
    IG_REQUIRE(ack.has_value());
    IG_CHECK(ack.value().accepted);
    IG_CHECK(ack.value().reason == RefusalReason::None);
    IG_CHECK(!holder.fenced());
    IG_CHECK_EQ(running.coordinator.live_lease_count(), 1u);

    holder.close();
    running.shutdown();
}

IG_TEST(integration, connections_beyond_the_configured_bound_are_refused) {
    CoordinatorConfig config{};
    config.max_connections = 2;
    RunningCoordinator running(config);
    IG_REQUIRE(running.start());

    GovernorWorker first(worker_config_for(running.coordinator, "bound-first", 51));
    GovernorWorker second(worker_config_for(running.coordinator, "bound-second", 52));
    IG_REQUIRE(static_cast<bool>(first.connect_and_handshake()));
    IG_REQUIRE(static_cast<bool>(second.connect_and_handshake()));

    // Both sessions are open and idle, so a third peer is not served.
    GovernorWorker third(worker_config_for(running.coordinator, "bound-third", 53));
    IG_CHECK(!static_cast<bool>(third.connect_and_handshake()));
    third.close();

    // Ending one session frees its place.
    first.close();
    bool admitted = false;
    for (int attempt = 0; attempt < 500 && !admitted; ++attempt) {
        GovernorWorker next(worker_config_for(running.coordinator, "bound-next", 54));
        admitted = static_cast<bool>(next.connect_and_handshake());
        if (admitted) {
            IG_CHECK(next.acquire_lease().has_value());
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        next.close();
    }
    IG_CHECK(admitted);

    second.close();
    running.shutdown();
}

IG_TEST(integration, stopping_ends_sessions_that_are_connected_and_silent) {
    CoordinatorConfig config{};
    RunningCoordinator running(config);
    IG_REQUIRE(running.start());

    GovernorWorker worker(worker_config_for(running.coordinator, "silent", 61));
    IG_REQUIRE(static_cast<bool>(worker.connect_and_handshake()));
    IG_REQUIRE(worker.acquire_lease().has_value());

    // The worker stays connected and sends nothing. Stopping joins the session
    // that serves it, so the session has to be ended, not waited for.
    const auto started = std::chrono::steady_clock::now();
    running.shutdown();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);
    IG_CHECK(elapsed.count() < 30);

    // The coordinator closed the session: the worker's next exchange fails.
    IG_CHECK(!static_cast<bool>(worker.send_heartbeat()));
    worker.close();
}
