// Incast Governor - crash-safe durable store: journal, snapshot and recovery.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_DURABLE_HPP
#define INCAST_DURABLE_HPP

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "incast/core.hpp"
#include "incast/governor.hpp"

namespace incast {

enum class JournalRecordType : std::uint16_t {
    StateCommit = 1,
    InterventionTransition = 2,
    PolicyChange = 3,
    AuthorityBinding = 4,
    Checkpoint = 5,
    Shutdown = 6,
    EpochAdvance = 7,
};

[[nodiscard]] constexpr std::string_view to_string(JournalRecordType type) noexcept {
    switch (type) {
        case JournalRecordType::StateCommit: return "StateCommit";
        case JournalRecordType::InterventionTransition: return "InterventionTransition";
        case JournalRecordType::PolicyChange: return "PolicyChange";
        case JournalRecordType::AuthorityBinding: return "AuthorityBinding";
        case JournalRecordType::Checkpoint: return "Checkpoint";
        case JournalRecordType::Shutdown: return "Shutdown";
        case JournalRecordType::EpochAdvance: return "EpochAdvance";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Recovery report. Recovery distinguishes durable configuration and history,
// committed authoritative state, unfinished attempts, ambiguous outcomes, stale
// live authority and evidence requiring revalidation.
// ---------------------------------------------------------------------------

struct RecoveryReport {
    bool snapshot_present = false;
    bool snapshot_loaded = false;
    bool snapshot_corrupt = false;
    std::uint64_t snapshot_generation = 0;

    bool journal_present = false;
    std::uint64_t journal_records_applied = 0;
    std::uint64_t journal_records_rejected = 0;
    std::uint64_t journal_tail_bytes_discarded = 0;
    bool journal_tail_ambiguous = false;

    bool recovered_committed_state = false;
    bool has_unfinished_attempts = false;
    bool has_ambiguous_outcomes = false;
    bool stale_live_authority_dropped = false;
    std::uint32_t interventions_requiring_revalidation = 0;
    std::uint32_t interventions_dropped_as_stale = 0;

    GovernorState state{};
    std::vector<std::string> notes{};
};

struct DurableConfig {
    std::filesystem::path directory{};
    std::size_t max_journal_records_before_snapshot = kMaxJournalRecordsBeforeSnapshot;
    bool fsync_on_commit = true;
    std::size_t max_state_bytes = 4u << 20;
};

// ---------------------------------------------------------------------------
// DurableStore: validate -> bind authority -> plan -> journal -> verify ->
// commit -> cleanup. A commit is only acknowledged after the record is durable.
// ---------------------------------------------------------------------------

class DurableStore {
public:
    DurableStore() = default;
    ~DurableStore();

    DurableStore(const DurableStore&) = delete;
    DurableStore& operator=(const DurableStore&) = delete;

    [[nodiscard]] Status open(const DurableConfig& config);
    void close();

    [[nodiscard]] bool is_open() const noexcept { return open_; }

    // Recovers durable state. Never restores liveness or authority.
    [[nodiscard]] Outcome<RecoveryReport> recover();

    [[nodiscard]] Status commit(const GovernorState& state);
    [[nodiscard]] Status append(JournalRecordType type, std::span<const std::byte> payload);
    [[nodiscard]] Status checkpoint(const GovernorState& state);
    [[nodiscard]] Status flush();

    [[nodiscard]] std::uint64_t journal_record_count() const noexcept { return records_since_snapshot_; }
    [[nodiscard]] const std::filesystem::path& snapshot_path() const noexcept { return snapshot_path_; }
    [[nodiscard]] const std::filesystem::path& journal_path() const noexcept { return journal_path_; }

private:
    [[nodiscard]] Status write_snapshot_atomic(const GovernorState& state);
    [[nodiscard]] Status rotate_journal();
    [[nodiscard]] Status ensure_directory() const;

    DurableConfig config_{};
    std::filesystem::path snapshot_path_{};
    std::filesystem::path journal_path_{};
    std::string open_journal_{};   // journal file name currently appended to
    std::uint64_t journal_epoch_ = 0;
    std::uint64_t records_since_snapshot_ = 0;
    bool open_ = false;
};

// Low-level helpers, exposed for tests.
[[nodiscard]] std::vector<std::byte> encode_journal_record(JournalRecordType type,
                                                           std::span<const std::byte> payload);
[[nodiscard]] Status decode_journal_record(std::span<const std::byte> bytes,
                                           JournalRecordType& type,
                                           std::span<const std::byte>& payload,
                                           std::size_t& consumed);

[[nodiscard]] Status write_file_atomic(const std::filesystem::path& target,
                                       std::span<const std::byte> content,
                                       bool fsync);

}  // namespace incast

#endif  // INCAST_DURABLE_HPP
