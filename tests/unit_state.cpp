// Incast Governor - state serialisation and durable store tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "governor_fixture.hpp"
#include "incast/durable.hpp"
#include "incast/state.hpp"
#include "incast/synthetic.hpp"
#include "test_framework.hpp"

using namespace incast;

namespace {

class TempDirectory {
public:
    explicit TempDirectory(const std::string& label) {
        static std::uint64_t counter = 0;
        counter += 1;
        path_ = std::filesystem::temp_directory_path() /
                ("incast-governor-tests-" + label + "-" + std::to_string(counter));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_{};
};

GovernorState make_state_with_intervention() {
    const auto spec = synthetic::synchronized_burst(64);
    auto governor = igfixture::make_governor(spec);
    const Decision decision = governor.evaluate(synthetic::build(spec));
    IG_REQUIRE(decision.outcome == DecisionOutcome::InterventionIssued);
    return governor.snapshot_state();
}

}  // namespace

IG_TEST(state, round_trip_preserves_every_authoritative_field) {
    const GovernorState original = make_state_with_intervention();
    const auto encoded = encode_state(original);
    auto decoded = decode_state(encoded);
    IG_REQUIRE(decoded.has_value());

    const GovernorState& restored = decoded.value();
    IG_CHECK_EQ(restored.generation.value(), original.generation.value());
    IG_CHECK_EQ(restored.destination.value(), original.destination.value());
    IG_CHECK_EQ(restored.evaluation_count, original.evaluation_count);
    IG_CHECK_EQ(restored.history.size(), original.history.size());
    IG_CHECK_EQ(restored.interventions.size(), original.interventions.size());
    IG_CHECK_EQ(restored.next_intervention_generation.value(),
                original.next_intervention_generation.value());
    IG_REQUIRE(!restored.interventions.empty());
    IG_CHECK(restored.interventions.front().state == original.interventions.front().state);
    IG_CHECK_EQ(restored.interventions.front().intent.reduction_bp.value(),
                original.interventions.front().intent.reduction_bp.value());
    IG_CHECK_EQ(restored.interventions.front().intent.penalized_senders.size(),
                original.interventions.front().intent.penalized_senders.size());
}

IG_TEST(state, every_corruption_class_is_rejected) {
    const GovernorState original = make_state_with_intervention();
    const auto encoded = encode_state(original);

    // Magic.
    auto magic = encoded;
    magic[0] = std::byte{0x00};
    IG_CHECK(decode_state(magic).error().code == ErrorCode::CorruptState);

    // Version.
    auto version = encoded;
    version[4] = std::byte{0x7F};
    IG_CHECK(decode_state(version).error().code == ErrorCode::UnsupportedVersion);

    // Truncated payload.
    auto truncated = encoded;
    truncated.resize(encoded.size() - 8);
    IG_CHECK(decode_state(truncated).error().code == ErrorCode::TruncatedInput);

    // Payload checksum.
    auto corrupted = encoded;
    corrupted.back() = static_cast<std::byte>(static_cast<unsigned>(corrupted.back()) ^ 0xFFu);
    IG_CHECK(decode_state(corrupted).error().code == ErrorCode::ChecksumMismatch);

    // Declared length beyond the physical buffer.
    auto overlength = encoded;
    overlength[16] = std::byte{0xFF};
    overlength[17] = std::byte{0xFF};
    IG_CHECK(!decode_state(overlength).has_value());
}

IG_TEST(state, intervention_codec_rejects_trailing_and_oversized_input) {
    const GovernorState state = make_state_with_intervention();
    IG_REQUIRE(!state.interventions.empty());
    const auto encoded = encode_intervention(state.interventions.front());
    auto decoded = decode_intervention(encoded);
    IG_REQUIRE(decoded.has_value());
    IG_CHECK(decoded.value().id == state.interventions.front().id);

    auto padded = encoded;
    padded.push_back(std::byte{0x00});
    const auto padded_result = decode_intervention(padded);
    IG_REQUIRE(!padded_result.has_value());
    IG_CHECK(padded_result.error().code == ErrorCode::MalformedInput);

    auto truncated = encoded;
    truncated.resize(encoded.size() / 2);
    IG_CHECK(!decode_intervention(truncated).has_value());
}

IG_TEST(durable, commit_and_recover_across_restart) {
    TempDirectory directory("durable");
    DurableConfig config{};
    config.directory = directory.path();

    GovernorState first{};
    {
        DurableStore store;
        IG_REQUIRE(static_cast<bool>(store.open(config)));
        first = make_state_with_intervention();
        IG_REQUIRE(static_cast<bool>(store.commit(first)));
        GovernorState second = first;
        second.generation = StateGeneration{first.generation.value() + 1};
        second.evaluation_count += 1;
        IG_REQUIRE(static_cast<bool>(store.commit(second)));
        first = second;
    }

    DurableStore reopened;
    IG_REQUIRE(static_cast<bool>(reopened.open(config)));
    auto report = reopened.recover();
    IG_REQUIRE(report.has_value());
    IG_CHECK(report.value().journal_records_applied >= 2);
    IG_CHECK_EQ(report.value().state.generation.value(), first.generation.value());
    IG_CHECK(report.value().recovered_committed_state);
    // Durable state never restores live authority.
    IG_CHECK(report.value().state.requires_revalidation);
    IG_CHECK(!report.value().state.epoch.valid());
    IG_CHECK(!report.value().state.boot.valid());
    // Every recovered intervention is a revalidation candidate: it holds no
    // enforcement authority until authority is explicitly re-bound.
    for (const auto& intervention : report.value().state.interventions) {
        IG_CHECK(intervention.state == InterventionState::Revalidating);
        IG_CHECK(intervention.state != InterventionState::Active);
    }
}

IG_TEST(durable, torn_journal_tail_is_discarded_not_trusted) {
    TempDirectory directory("torn");
    DurableConfig config{};
    config.directory = directory.path();
    config.max_journal_records_before_snapshot = 1'000'000;

    GovernorState committed{};
    {
        DurableStore store;
        IG_REQUIRE(static_cast<bool>(store.open(config)));
        committed = make_state_with_intervention();
        IG_REQUIRE(static_cast<bool>(store.commit(committed)));
    }

    // Simulate a crash during a later append: a partial record lands at the end.
    {
        std::ofstream stream(directory.path() / "governor.1.journal", std::ios::binary | std::ios::app);
        IG_REQUIRE(stream.good());
        const char partial[] = "IGR1\x01\x00\x01";
        stream.write(partial, sizeof(partial) - 1);
    }

    DurableStore reopened;
    IG_REQUIRE(static_cast<bool>(reopened.open(config)));
    auto report = reopened.recover();
    IG_REQUIRE(report.has_value());
    IG_CHECK(report.value().journal_tail_bytes_discarded > 0);
    IG_CHECK_EQ(report.value().state.generation.value(), committed.generation.value());
}

IG_TEST(durable, a_corrupt_complete_tail_record_is_reported_as_ambiguous) {
    TempDirectory directory("ambiguous");
    DurableConfig config{};
    config.directory = directory.path();
    config.max_journal_records_before_snapshot = 1'000'000;

    GovernorState committed{};
    {
        DurableStore store;
        IG_REQUIRE(static_cast<bool>(store.open(config)));
        committed = make_state_with_intervention();
        IG_REQUIRE(static_cast<bool>(store.commit(committed)));
    }

    // Append a structurally complete record whose payload no longer matches its
    // checksum: the outcome is genuinely ambiguous and must be reported as such.
    {
        const std::vector<std::byte> payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        auto record = encode_journal_record(JournalRecordType::StateCommit, payload);
        record.back() = std::byte{0xFF};
        std::ofstream stream(directory.path() / "governor.1.journal", std::ios::binary | std::ios::app);
        IG_REQUIRE(stream.good());
        stream.write(reinterpret_cast<const char*>(record.data()),
                     static_cast<std::streamsize>(record.size()));
    }

    DurableStore reopened;
    IG_REQUIRE(static_cast<bool>(reopened.open(config)));
    auto report = reopened.recover();
    IG_REQUIRE(report.has_value());
    IG_CHECK(report.value().journal_tail_ambiguous);
    IG_CHECK(report.value().has_ambiguous_outcomes);
    IG_CHECK_EQ(report.value().state.generation.value(), committed.generation.value());
}

IG_TEST(durable, corrupt_snapshot_falls_back_to_the_journal) {
    TempDirectory directory("snapshot");
    DurableConfig config{};
    config.directory = directory.path();

    GovernorState committed{};
    {
        DurableStore store;
        IG_REQUIRE(static_cast<bool>(store.open(config)));
        committed = make_state_with_intervention();
        IG_REQUIRE(static_cast<bool>(store.commit(committed)));
        IG_REQUIRE(static_cast<bool>(store.checkpoint(committed)));
    }

    // Corrupt the snapshot body but keep the journal intact.
    {
        std::fstream stream(directory.path() / "governor.snapshot", std::ios::binary | std::ios::in | std::ios::out);
        IG_REQUIRE(stream.good());
        stream.seekp(static_cast<std::streamoff>(kStateHeaderBytes + 4));
        const char junk[] = "XXXX";
        stream.write(junk, 4);
    }

    DurableStore reopened;
    IG_REQUIRE(static_cast<bool>(reopened.open(config)));
    auto report = reopened.recover();
    IG_REQUIRE(report.has_value());
    IG_CHECK(report.value().snapshot_corrupt);
    IG_CHECK(report.value().recovered_committed_state || report.value().journal_records_applied == 0);
}

IG_TEST(durable, checkpointing_rotates_the_journal_without_losing_state) {
    TempDirectory directory("rotate");
    DurableConfig config{};
    config.directory = directory.path();
    config.max_journal_records_before_snapshot = 3;

    GovernorState latest{};
    {
        DurableStore store;
        IG_REQUIRE(static_cast<bool>(store.open(config)));
        for (int index = 0; index < 12; ++index) {
            latest = make_state_with_intervention();
            latest.generation = StateGeneration{static_cast<std::uint64_t>(index) + 1};
            latest.evaluation_count = static_cast<std::uint64_t>(index);
            IG_REQUIRE(static_cast<bool>(store.commit(latest)));
        }
    }
    IG_CHECK(std::filesystem::exists(directory.path() / "governor.snapshot"));

    DurableStore reopened;
    IG_REQUIRE(static_cast<bool>(reopened.open(config)));
    auto report = reopened.recover();
    IG_REQUIRE(report.has_value());
    IG_CHECK_EQ(report.value().state.generation.value(), latest.generation.value());
}

IG_TEST(durable, oversized_payload_is_refused_before_it_reaches_disk) {
    TempDirectory directory("oversize");
    DurableConfig config{};
    config.directory = directory.path();
    config.max_state_bytes = 64;

    DurableStore store;
    IG_REQUIRE(static_cast<bool>(store.open(config)));
    const GovernorState state = make_state_with_intervention();
    const Status committed = store.commit(state);
    IG_CHECK(!committed);
    IG_CHECK(committed.error().code == ErrorCode::OversizedInput);
}

IG_TEST(durable, atomic_write_leaves_no_temporary_file_behind) {
    TempDirectory directory("atomic");
    const auto target = directory.path() / "payload.bin";
    const std::vector<std::byte> content{std::byte{1}, std::byte{2}, std::byte{3}};
    IG_REQUIRE(static_cast<bool>(write_file_atomic(target, content, true)));
    IG_CHECK(std::filesystem::exists(target));
    IG_CHECK(!std::filesystem::exists(directory.path() / "payload.bin.tmp"));
    IG_CHECK(static_cast<bool>(write_file_atomic(target, std::vector<std::byte>{}, true)));
    IG_CHECK_EQ(std::filesystem::file_size(target), 0u);
}

IG_TEST(durable, journal_record_codec_rejects_damage) {
    const std::vector<std::byte> payload{std::byte{9}, std::byte{8}, std::byte{7}};
    const auto record = encode_journal_record(JournalRecordType::StateCommit, payload);

    JournalRecordType type = JournalRecordType::Checkpoint;
    std::span<const std::byte> body{};
    std::size_t consumed = 0;
    IG_REQUIRE(static_cast<bool>(decode_journal_record(record, type, body, consumed)));
    IG_CHECK(type == JournalRecordType::StateCommit);
    IG_CHECK_EQ(consumed, record.size());
    IG_CHECK_EQ(body.size(), payload.size());

    auto corrupted = record;
    corrupted.back() = std::byte{0x00};
    IG_CHECK(decode_journal_record(corrupted, type, body, consumed).error().code ==
             ErrorCode::ChecksumMismatch);

    auto truncated = record;
    truncated.resize(record.size() - 1);
    IG_CHECK(decode_journal_record(truncated, type, body, consumed).error().code ==
             ErrorCode::TruncatedInput);

    std::vector<std::byte> empty{};
    IG_CHECK(decode_journal_record(empty, type, body, consumed).error().code ==
             ErrorCode::TruncatedInput);
}
