// Incast Governor - adversarial and malformed input tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "governor_fixture.hpp"
#include "incast/coordinator.hpp"
#include "incast/durable.hpp"
#include "incast/governor.hpp"
#include "incast/state.hpp"
#include "incast/synthetic.hpp"
#include "incast/transport.hpp"
#include "test_framework.hpp"

using namespace incast;

namespace {

synthetic::FanInSpec window_of(const synthetic::FanInSpec& base, std::uint64_t index) {
    synthetic::FanInSpec spec = base;
    spec.window = WindowId{index};
    spec.window_generation = WindowGeneration{index};
    spec.event = EventId{index};
    spec.event_generation = EventGeneration{index};
    spec.sequence = index * 10;
    spec.now = Timestamp{base.now.nanos() + static_cast<std::int64_t>(index) * 1'000'000};
    return spec;
}

}  // namespace

IG_TEST(adversarial, structurally_invalid_bundles_are_refused) {
    const auto active = synthetic::synchronized_burst(64);

    auto missing_destination = synthetic::build(window_of(active, 1));
    missing_destination.destination = DestinationId{};
    auto governor = igfixture::make_governor(active);
    IG_CHECK(governor.evaluate(missing_destination).refusal == RefusalReason::MalformedBundle);

    auto missing_event = synthetic::build(window_of(active, 2));
    missing_event.event = EventId{};
    IG_CHECK(governor.evaluate(missing_event).refusal == RefusalReason::MalformedBundle);

    auto reversed_window = synthetic::build(window_of(active, 3));
    reversed_window.window_start = Timestamp{active.now.nanos() + 1'000};
    reversed_window.window_end = Timestamp{active.now.nanos() - 1'000};
    IG_CHECK(governor.evaluate(reversed_window).refusal == RefusalReason::MalformedBundle);
}

IG_TEST(adversarial, oversized_collections_are_refused_before_processing) {
    const auto active = synthetic::synchronized_burst(64);
    auto bundle = synthetic::build(active);
    bundle.senders.resize(kMaxSendersPerWindow + 1);
    auto governor = igfixture::make_governor(active);
    IG_CHECK(governor.evaluate(bundle).refusal == RefusalReason::MalformedBundle);
}

IG_TEST(adversarial, evidence_from_the_future_is_contradictory) {
    const auto active = synthetic::synchronized_burst(64);
    auto spec = window_of(active, 1);
    spec.evidence_from_future = true;   // observations stamped after the evaluation instant
    const auto bundle = synthetic::build(spec);
    const auto policy = make_default_policy();
    const auto check = check_authority(bundle, policy, {});
    IG_CHECK(check.reason == RefusalReason::ContradictoryEvidence);
}

IG_TEST(adversarial, zero_service_rate_is_contradictory_capacity) {
    const auto active = synthetic::synchronized_burst(64);
    auto spec = window_of(active, 1);
    spec.service_rate = Rate{0};
    const auto bundle = synthetic::build(spec);
    const auto policy = make_default_policy();
    const auto check = check_authority(bundle, policy, {});
    IG_CHECK(check.reason == RefusalReason::ContradictoryEvidence);
    IG_CHECK(check.classes[static_cast<std::size_t>(EvidenceClass::Capacity)] ==
             EvidenceState::Contradictory);
}

IG_TEST(adversarial, huge_fan_in_is_handled_within_bounds) {
    auto spec = synthetic::synchronized_burst(static_cast<std::uint32_t>(kMaxSendersPerWindow));
    spec.per_sender_rate = Rate{1'000'000};
    auto governor = igfixture::make_governor(spec);
    const Decision decision = governor.evaluate(synthetic::build(spec));
    IG_CHECK(decision.kind != IncidentKind::Unknown);
    IG_CHECK(decision.intent.penalized_senders.size() <= kMaxPenalizedSenders);
    IG_CHECK(decision.explanation.within_budget());
}

IG_TEST(adversarial, replayed_windows_cannot_reissue_an_intervention) {
    const auto active = synthetic::synchronized_burst(96);
    auto governor = igfixture::make_governor(active, true);
    const auto bundle = synthetic::build(window_of(active, 1));

    IG_REQUIRE(governor.evaluate(bundle).outcome == DecisionOutcome::InterventionIssued);
    for (int replay = 0; replay < 6; ++replay) {
        const Decision decision = governor.evaluate(bundle);
        IG_CHECK(decision.outcome == DecisionOutcome::Refused);
        IG_CHECK(decision.refusal == RefusalReason::ReorderedEvidence);
    }
    IG_CHECK_EQ(governor.interventions().size(), 1u);
}

IG_TEST(adversarial, truncated_and_corrupt_frames_are_rejected) {
    Frame frame{};
    frame.type = FrameType::DecisionPublish;
    frame.payload = {std::byte{1}, std::byte{2}, std::byte{3}};
    auto encoded_result = encode_frame(frame);
    IG_REQUIRE(encoded_result.has_value());
    const auto& encoded = encoded_result.value();

    auto decoded = decode_frame(encoded);
    IG_REQUIRE(decoded.has_value());
    IG_CHECK(decoded.value().type == FrameType::DecisionPublish);
    IG_CHECK_EQ(decoded.value().payload.size(), 3u);

    auto short_buffer = encoded;
    short_buffer.resize(encoded.size() - 1);
    IG_CHECK(decode_frame(short_buffer).error().code == ErrorCode::TruncatedInput);

    auto header_only = encoded;
    header_only.resize(kFrameHeaderBytes);
    IG_CHECK(decode_frame(header_only).error().code == ErrorCode::TruncatedInput);

    auto bad_magic = encoded;
    bad_magic[0] = std::byte{0x00};
    IG_CHECK(decode_frame(bad_magic).error().code == ErrorCode::ProtocolViolation);

    auto bad_version = encoded;
    bad_version[4] = std::byte{0x7F};
    IG_CHECK(decode_frame(bad_version).error().code == ErrorCode::UnsupportedVersion);

    auto bad_type = encoded;
    bad_type[6] = std::byte{0xFF};
    IG_CHECK(decode_frame(bad_type).error().code == ErrorCode::ProtocolViolation);

    auto reserved = encoded;
    reserved[10] = std::byte{0x01};
    IG_CHECK(decode_frame(reserved).error().code == ErrorCode::ProtocolViolation);

    auto corrupt_payload = encoded;
    corrupt_payload.back() = static_cast<std::byte>(static_cast<unsigned>(corrupt_payload.back()) ^ 0xFFu);
    IG_CHECK(decode_frame(corrupt_payload).error().code == ErrorCode::ChecksumMismatch);

    // An oversized payload is refused at encode time rather than truncated.
    Frame oversized_frame{};
    oversized_frame.type = FrameType::Hello;
    oversized_frame.payload.assign(kMaxFrameBytes + 1, std::byte{0});
    IG_CHECK(encode_frame(oversized_frame).error().code == ErrorCode::OversizedInput);

    // A frame header that declares more than the supported bound is refused
    // before any allocation is attempted.
    auto oversized = encoded;
    oversized[12] = std::byte{0xFF};
    oversized[13] = std::byte{0xFF};
    oversized[14] = std::byte{0xFF};
    oversized[15] = std::byte{0x7F};
    IG_CHECK(decode_frame(oversized).error().code == ErrorCode::OversizedInput);
}

IG_TEST(adversarial, malformed_protocol_messages_are_rejected) {
    const std::vector<std::byte> empty{};
    IG_CHECK(!decode_hello(empty).has_value());
    IG_CHECK(!decode_lease_request(empty).has_value());
    IG_CHECK(!decode_decision_ack(empty).has_value());
    IG_CHECK(!decode_control_request(empty).has_value());

    MessageHello hello{};
    hello.worker = WorkerId{1};
    hello.boot = BootIncarnation{2};
    hello.name.assign("w");
    auto encoded = encode_hello(hello);
    encoded.push_back(std::byte{0x00});
    IG_CHECK(decode_hello(encoded).error().code == ErrorCode::MalformedInput);

    auto oversized_name = encode_hello(hello);
    codec::Writer writer;
    writer.u64(1);
    writer.u64(2);
    writer.u32(0xFFFF);
    const auto crafted = writer.buffer();
    IG_CHECK(decode_hello(crafted).error().code == ErrorCode::OversizedInput);

    ControlRequest request{};
    request.opcode = static_cast<ControlOpcode>(99);
    codec::Writer control_writer;
    control_writer.u32(99);
    control_writer.str("token");
    control_writer.str("arg");
    IG_CHECK(decode_control_request(control_writer.buffer()).error().code == ErrorCode::ProtocolViolation);
}

IG_TEST(adversarial, intent_codec_rejects_unknown_enumerations) {
    MitigationIntent intent{};
    intent.kind = MitigationKind::PacingHint;
    intent.scope = MitigationScope::Destination;
    auto encoded = encode_intent(intent);
    IG_REQUIRE(decode_intent(encoded).has_value());

    auto bad_kind = encoded;
    bad_kind[0] = std::byte{0x7F};
    IG_CHECK(decode_intent(bad_kind).error().code == ErrorCode::CorruptState);

    auto bad_scope = encoded;
    bad_scope[1] = std::byte{0x7F};
    IG_CHECK(decode_intent(bad_scope).error().code == ErrorCode::CorruptState);

    auto truncated = encoded;
    truncated.resize(encoded.size() - 4);
    IG_CHECK(!decode_intent(truncated).has_value());
}

IG_TEST(adversarial, coordinator_denies_malformed_and_unleased_publishes) {
    CoordinatorConfig config{};
    config.port = 0;
    config.control_token = "test-token";
    config.lease_duration = Duration{500'000'000};
    Coordinator coordinator(config);
    IG_REQUIRE(static_cast<bool>(coordinator.start()));

    std::thread server([&coordinator]() { static_cast<void>(coordinator.serve_one()); });

    net::Connection connection;
    IG_REQUIRE(static_cast<bool>(connection.connect_loopback(coordinator.port())));
    FramedChannel channel(std::move(connection));

    MessageDecisionPublish publish{};
    publish.lease = LeaseId{999};
    publish.epoch = coordinator.epoch();
    publish.boot = mint_boot_incarnation();
    publish.worker = WorkerId{1};
    publish.destination = DestinationId{1};
    publish.sequence = 1;
    Frame frame{};
    frame.type = FrameType::DecisionPublish;
    frame.payload = encode_decision_publish(publish);
    IG_REQUIRE(static_cast<bool>(channel.send(frame)));

    auto reply = channel.receive();
    IG_REQUIRE(reply.has_value());
    IG_CHECK(reply.value().type == FrameType::DecisionAck);
    auto ack = decode_decision_ack(reply.value().payload);
    IG_REQUIRE(ack.has_value());
    IG_CHECK(!ack.value().accepted);
    IG_CHECK(ack.value().reason == RefusalReason::LeaseMissing);

    channel.close();
    server.join();
    coordinator.stop();
}

IG_TEST(adversarial, coordinator_control_requires_the_token) {
    CoordinatorConfig config{};
    config.control_token = "secret";
    Coordinator coordinator(config);
    IG_REQUIRE(static_cast<bool>(coordinator.start()));

    std::thread server([&coordinator]() { static_cast<void>(coordinator.serve_one()); });

    ControlRequest request{};
    request.opcode = ControlOpcode::AdvanceEpoch;
    request.token = "wrong";
    auto response = send_control(coordinator.port(), request);
    IG_REQUIRE(response.has_value());
    IG_CHECK(!response.value().ok);
    IG_CHECK(coordinator.epoch() == EpochId{1});

    server.join();
    coordinator.stop();
}

IG_TEST(adversarial, durable_store_refuses_oversized_journal_payloads) {
    std::vector<std::byte> payload(kMaxJournalRecordBytes + 1);
    const auto record = encode_journal_record(JournalRecordType::Checkpoint, payload);
    JournalRecordType type = JournalRecordType::Checkpoint;
    std::span<const std::byte> body{};
    std::size_t consumed = 0;
    IG_CHECK(decode_journal_record(record, type, body, consumed).error().code ==
             ErrorCode::OversizedInput);
}
