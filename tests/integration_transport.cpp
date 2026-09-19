// Incast Governor - transport and in-process coordinator/worker integration.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>
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
