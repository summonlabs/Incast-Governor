// Incast Governor - crash-safe durable store implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "incast/durable.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

#include "incast/state.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace incast {
namespace {

inline constexpr std::uint32_t kJournalRecordMagic = 0x31524749u;  // 'IGR1'
inline constexpr std::size_t kJournalRecordHeaderBytes = 16;
inline constexpr std::string_view kJournalFileHeader = "IGJRNL01";
inline constexpr std::size_t kJournalFileHeaderBytes = 12;

[[nodiscard]] Status sync_file(std::FILE* handle) {
    if (handle == nullptr) return fail(ErrorCode::IoFailure, "no file handle to synchronise");
    if (std::fflush(handle) != 0) return fail(ErrorCode::IoFailure, "failed to flush the file buffer");
#if defined(_WIN32)
    if (_commit(_fileno(handle)) != 0) return fail(ErrorCode::IoFailure, "failed to commit the file to disk");
#else
    if (::fsync(::fileno(handle)) != 0) return fail(ErrorCode::IoFailure, "failed to fsync the file");
#endif
    return ok_status();
}

[[nodiscard]] Outcome<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return Error{ErrorCode::IoFailure, "unable to open file for reading"};
    std::vector<std::byte> content;
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0) return Error{ErrorCode::IoFailure, "unable to determine file size"};
    if (static_cast<std::uint64_t>(size) > (64ull << 20)) {
        return Error{ErrorCode::OversizedInput, "file exceeds the supported read bound"};
    }
    stream.seekg(0, std::ios::beg);
    content.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        stream.read(reinterpret_cast<char*>(content.data()), size);
        if (!stream) return Error{ErrorCode::IoFailure, "short read while loading file"};
    }
    return content;
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + static_cast<std::size_t>(index)]))
                 << (index * 8);
    }
    return value;
}

[[nodiscard]] std::uint16_t read_u16(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint16_t value = 0;
    for (int index = 0; index < 2; ++index) {
        value |= static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[offset + static_cast<std::size_t>(index)]))
                 << (index * 8);
    }
    return value;
}

// The journals of a directory in the order they were written. Journals are
// ordered by their numeric rotation index, not by their file name:
// lexicographic ordering would place "governor.10.journal" before
// "governor.9.journal" and replay records out of order.
[[nodiscard]] std::vector<std::pair<std::uint64_t, std::filesystem::path>> list_journals(
    const std::filesystem::path& directory, std::error_code& error) {
    std::vector<std::pair<std::uint64_t, std::filesystem::path>> journals;
    const std::string prefix("governor.");
    const std::string suffix(".journal");
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) break;
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0 || entry.path().extension() != suffix) continue;
        if (name.size() < prefix.size() + suffix.size()) continue;
        const std::string digits = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        const auto index = parse_u64(digits);
        journals.emplace_back(index.value_or(0), entry.path());
    }
    std::sort(journals.begin(), journals.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    return journals;
}

// What reading one journal found.
struct JournalScan {
    bool readable = false;      // the file could be opened and read to its end
    bool header_valid = false;  // it starts with the journal file header
    bool damaged = false;       // something after the valid prefix did not decode
    ErrorCode damage = ErrorCode::None;
    std::uint64_t valid_bytes = 0;  // length of the prefix that decoded cleanly
    std::uint64_t records = 0;      // records inside that prefix
};

// Reads one journal record by record and applies its committed states to the
// report. Only one record is held in memory at a time, so a journal is bounded
// by the records it may hold and not by what could be loaded at once: a journal
// the store was allowed to write is always one recovery can read.
[[nodiscard]] JournalScan scan_journal(const std::filesystem::path& path, RecoveryReport& report) {
    JournalScan scan{};
    const std::string name = path.filename().string();

    std::error_code size_error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, size_error);
    std::ifstream stream(path, std::ios::binary);
    if (size_error || !stream) {
        report.notes.emplace_back("journal could not be read: " + name);
        return scan;
    }
    const std::uint64_t total = static_cast<std::uint64_t>(file_size);

    std::array<char, kJournalFileHeaderBytes> file_header{};
    stream.read(file_header.data(), static_cast<std::streamsize>(file_header.size()));
    if (static_cast<std::size_t>(stream.gcount()) != file_header.size() ||
        std::memcmp(file_header.data(), kJournalFileHeader.data(), kJournalFileHeader.size()) != 0) {
        scan.readable = !stream.bad();
        report.notes.emplace_back("journal header invalid: " + name);
        return scan;
    }
    scan.readable = true;
    scan.header_valid = true;
    scan.valid_bytes = kJournalFileHeaderBytes;

    const auto read_exact = [&stream](std::byte* destination, std::size_t count) {
        stream.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(count));
        return static_cast<std::size_t>(stream.gcount()) == count;
    };

    std::vector<std::byte> record;
    while (scan.valid_bytes < total) {
        const std::uint64_t remaining = total - scan.valid_bytes;
        const std::size_t header_bytes =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kJournalRecordHeaderBytes));
        record.resize(header_bytes);
        bool complete = read_exact(record.data(), header_bytes);
        if (complete && header_bytes == kJournalRecordHeaderBytes &&
            read_u32(record, 0) == kJournalRecordMagic && read_u16(record, 4) == kJournalFormatVersion &&
            read_u32(record, 8) <= kMaxJournalPayloadBytes) {
            // The header is believed about its payload length only once it has
            // passed the checks the decoder makes before it looks at a payload.
            const std::size_t payload_bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
                read_u32(record, 8), remaining - kJournalRecordHeaderBytes));
            record.resize(kJournalRecordHeaderBytes + payload_bytes);
            complete = read_exact(record.data() + kJournalRecordHeaderBytes, payload_bytes);
        }
        if (!complete) {
            scan.readable = false;
            report.notes.emplace_back("journal could not be read to its end: " + name);
            return scan;
        }

        JournalRecordType type = JournalRecordType::StateCommit;
        std::span<const std::byte> payload{};
        std::size_t consumed = 0;
        const Status decoded = decode_journal_record(record, type, payload, consumed);
        if (!decoded) {
            if (decoded.error().code == ErrorCode::TruncatedInput && remaining < kJournalRecordHeaderBytes) {
                report.journal_tail_bytes_discarded += remaining;
                report.notes.emplace_back("journal tail truncated mid-header; discarded");
            } else if (decoded.error().code == ErrorCode::TruncatedInput ||
                       decoded.error().code == ErrorCode::ChecksumMismatch) {
                report.journal_tail_bytes_discarded += remaining;
                report.journal_tail_ambiguous = true;
                report.notes.emplace_back("journal tail is ambiguous; discarded rather than trusted");
            } else {
                report.journal_records_rejected += 1;
                report.notes.emplace_back(std::string("journal record rejected: ") +
                                          std::string(to_string(decoded.error().code)));
            }
            scan.damaged = true;
            scan.damage = decoded.error().code;
            return scan;
        }
        scan.valid_bytes += consumed;
        scan.records += 1;
        report.journal_records_applied += 1;
        if (type == JournalRecordType::StateCommit) {
            auto state = decode_state(payload);
            if (!state) {
                report.journal_records_rejected += 1;
                continue;
            }
            report.state = state.value();
            report.recovered_committed_state = true;
        }
    }
    return scan;
}

}  // namespace

Status write_file_atomic(const std::filesystem::path& target,
                         std::span<const std::byte> content,
                         bool fsync) {
    const std::filesystem::path temporary = target.string() + ".tmp";
    std::FILE* handle = std::fopen(temporary.string().c_str(), "wb");
    if (handle == nullptr) {
        return fail(ErrorCode::IoFailure, "unable to create the temporary file");
    }
    bool written = true;
    if (!content.empty()) {
        written = std::fwrite(content.data(), 1, content.size(), handle) == content.size();
    }
    if (!written) {
        std::fclose(handle);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return fail(ErrorCode::IoFailure, "short write to the temporary file");
    }
    if (fsync) {
        const Status synced = sync_file(handle);
        if (!synced) {
            std::fclose(handle);
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return synced;
        }
    }
    if (std::fclose(handle) != 0) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return fail(ErrorCode::IoFailure, "failed to close the temporary file");
    }
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return fail(ErrorCode::IoFailure, "failed to publish the file atomically");
    }
    return ok_status();
}

std::vector<std::byte> encode_journal_record(JournalRecordType type, std::span<const std::byte> payload) {
    std::vector<std::byte> record;
    record.reserve(kJournalRecordHeaderBytes + payload.size());
    const auto push_u16 = [&record](std::uint16_t value) {
        for (int shift = 0; shift < 16; shift += 8) {
            record.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
        }
    };
    const auto push_u32 = [&record](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            record.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
        }
    };
    push_u32(kJournalRecordMagic);
    push_u16(kJournalFormatVersion);
    push_u16(static_cast<std::uint16_t>(type));
    push_u32(static_cast<std::uint32_t>(payload.size()));
    push_u32(crc32c(payload.data(), payload.size()));
    record.insert(record.end(), payload.begin(), payload.end());
    return record;
}

Status decode_journal_record(std::span<const std::byte> bytes,
                             JournalRecordType& type,
                             std::span<const std::byte>& payload,
                             std::size_t& consumed) {
    consumed = 0;
    if (bytes.size() < kJournalRecordHeaderBytes) {
        return fail(ErrorCode::TruncatedInput, "journal record header is incomplete");
    }
    if (read_u32(bytes, 0) != kJournalRecordMagic) {
        return fail(ErrorCode::CorruptState, "journal record magic mismatch");
    }
    if (read_u16(bytes, 4) != kJournalFormatVersion) {
        return fail(ErrorCode::UnsupportedVersion, "unsupported journal record version");
    }
    const std::uint16_t raw_type = read_u16(bytes, 6);
    if (raw_type == 0 || raw_type > static_cast<std::uint16_t>(JournalRecordType::EpochAdvance)) {
        return fail(ErrorCode::CorruptState, "journal record type is outside the known set");
    }
    type = static_cast<JournalRecordType>(raw_type);
    const std::uint32_t length = read_u32(bytes, 8);
    const std::uint32_t checksum = read_u32(bytes, 12);
    if (length > kMaxJournalPayloadBytes) {
        return fail(ErrorCode::OversizedInput, "journal record exceeds the supported bound");
    }
    if (bytes.size() < kJournalRecordHeaderBytes + length) {
        return fail(ErrorCode::TruncatedInput, "journal record payload is incomplete");
    }
    const auto body = bytes.subspan(kJournalRecordHeaderBytes, length);
    if (crc32c(body.data(), body.size()) != checksum) {
        return fail(ErrorCode::ChecksumMismatch, "journal record failed its integrity check");
    }
    payload = body;
    consumed = kJournalRecordHeaderBytes + length;
    return ok_status();
}

DurableStore::~DurableStore() { close(); }

Status DurableStore::ensure_directory() const {
    std::error_code error;
    std::filesystem::create_directories(config_.directory, error);
    if (error) return fail(ErrorCode::IoFailure, "unable to create the durable state directory");
    return ok_status();
}

Status DurableStore::open(const DurableConfig& config) {
    if (open_) return fail(ErrorCode::AlreadyExists, "durable store is already open");
    if (config.directory.empty()) return fail(ErrorCode::InvalidArgument, "durable store directory is empty");
    config_ = config;
    const Status directory = ensure_directory();
    if (!directory) return directory;

    snapshot_path_ = config_.directory / "governor.snapshot";
    // The journal path is established by the first rotation so that every
    // journal file carries the file header; open() never fabricates a path.
    journal_path_.clear();
    journal_epoch_ = 0;
    records_since_snapshot_ = 0;
    open_ = true;
    return ok_status();
}

void DurableStore::close() {
    if (!open_) return;
    open_ = false;
    open_journal_.clear();
}

Status DurableStore::rotate_journal() {
    journal_epoch_ += 1;
    const std::filesystem::path next =
        config_.directory / ("governor." + std::to_string(journal_epoch_) + ".journal");
    std::vector<std::byte> header;
    header.reserve(kJournalFileHeaderBytes);
    for (const char character : kJournalFileHeader) {
        header.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    header.push_back(static_cast<std::byte>(kJournalFormatVersion & 0xFFu));
    header.push_back(static_cast<std::byte>((kJournalFormatVersion >> 8) & 0xFFu));
    header.push_back(std::byte{0});
    header.push_back(std::byte{0});
    const Status written = write_file_atomic(next, header, config_.fsync_on_commit);
    if (!written) return written;
    journal_path_ = next;
    records_since_snapshot_ = 0;
    return ok_status();
}

Status DurableStore::append(JournalRecordType type, std::span<const std::byte> payload) {
    if (!open_) return fail(ErrorCode::Closed, "durable store is not open");
    // The bound is the one recovery reads a record back under. A smaller one
    // here would refuse every state that outgrew it, for good, while the state
    // bound still called that state acceptable.
    if (payload.size() > kMaxJournalPayloadBytes) {
        return fail(ErrorCode::OversizedInput, "journal payload exceeds the supported bound");
    }
    if (journal_path_.empty()) {
        const Status rotated = rotate_journal();
        if (!rotated) return rotated;
    }
    const auto record = encode_journal_record(type, payload);
    std::FILE* handle = std::fopen(journal_path_.string().c_str(), "ab");
    if (handle == nullptr) return fail(ErrorCode::IoFailure, "unable to open the journal for append");
    const bool written = std::fwrite(record.data(), 1, record.size(), handle) == record.size();
    if (!written) {
        std::fclose(handle);
        return fail(ErrorCode::IoFailure, "short write to the journal");
    }
    if (config_.fsync_on_commit) {
        const Status synced = sync_file(handle);
        if (!synced) {
            std::fclose(handle);
            return synced;
        }
    } else if (std::fflush(handle) != 0) {
        std::fclose(handle);
        return fail(ErrorCode::IoFailure, "failed to flush the journal");
    }
    if (std::fclose(handle) != 0) return fail(ErrorCode::IoFailure, "failed to close the journal");
    records_since_snapshot_ += 1;
    return ok_status();
}

Status DurableStore::write_snapshot_atomic(const GovernorState& state) {
    const auto encoded = encode_state(state);
    if (encoded.size() > config_.max_state_bytes) {
        return fail(ErrorCode::OversizedInput, "encoded state exceeds the configured bound");
    }
    return write_file_atomic(snapshot_path_, encoded, config_.fsync_on_commit);
}

Status DurableStore::commit(const GovernorState& state) {
    if (!open_) return fail(ErrorCode::Closed, "durable store is not open");
    const auto encoded = encode_state(state);
    if (encoded.size() > config_.max_state_bytes) {
        return fail(ErrorCode::OversizedInput, "encoded state exceeds the configured bound");
    }
    const Status appended = append(JournalRecordType::StateCommit, encoded);
    if (!appended) return appended;
    if (records_since_snapshot_ >= config_.max_journal_records_before_snapshot) {
        return checkpoint(state);
    }
    return ok_status();
}

Status DurableStore::checkpoint(const GovernorState& state) {
    if (!open_) return fail(ErrorCode::Closed, "durable store is not open");
    // Order matters: the snapshot must be durable before the journal that
    // preceded it is retired, otherwise a crash could lose committed state.
    const Status snapshot = write_snapshot_atomic(state);
    if (!snapshot) return snapshot;
    const Status rotated = rotate_journal();
    if (!rotated) return rotated;
    // Every journal below the new one is superseded by the snapshot. All of
    // them are removed, not only the one that was open: a journal whose removal
    // failed at an earlier checkpoint, or was cut short by a crash, is retried
    // here instead of being left behind for good.
    std::error_code error;
    for (const auto& [rotation, path] : list_journals(config_.directory, error)) {
        if (rotation >= journal_epoch_ || path == journal_path_) continue;
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    return ok_status();
}

Status DurableStore::flush() { return open_ ? ok_status() : fail(ErrorCode::Closed, "durable store is not open"); }

Outcome<RecoveryReport> DurableStore::recover() {
    if (!open_) return Error{ErrorCode::Closed, "durable store is not open"};
    RecoveryReport report{};

    if (std::filesystem::exists(snapshot_path_)) {
        report.snapshot_present = true;
        auto content = read_file(snapshot_path_);
        if (!content) {
            report.snapshot_corrupt = true;
            report.notes.emplace_back("snapshot could not be read; recovery continues from the journal");
        } else {
            auto state = decode_state(content.value());
            if (!state) {
                report.snapshot_corrupt = true;
                report.notes.emplace_back(std::string("snapshot rejected: ") + std::string(to_string(state.error().code)));
            } else {
                report.snapshot_loaded = true;
                report.snapshot_generation = state.value().generation.value();
                report.state = state.value();
                report.recovered_committed_state = true;
            }
        }
    }

    std::error_code error;
    const auto journals = list_journals(config_.directory, error);
    if (error) return Error{ErrorCode::IoFailure, "unable to enumerate the durable state directory"};
    if (!journals.empty()) report.journal_present = true;

    // A snapshot is written by a checkpoint, and the checkpoint starts a new
    // journal right after it. Every journal below the newest one was therefore
    // already complete when the snapshot was taken: the snapshot supersedes it,
    // and it is only still here because its removal failed or was cut short.
    // Replaying one would put a state back that is older than the snapshot, and
    // it would stay in force whenever the newest journal holds no state yet.
    std::size_t first = 0;
    if (report.snapshot_loaded && journals.size() > 1) {
        first = journals.size() - 1;
        report.notes.emplace_back("journals superseded by the snapshot were not replayed");
    }

    JournalScan newest{};
    for (std::size_t index = first; index < journals.size(); ++index) {
        const JournalScan scan = scan_journal(journals[index].second, report);
        if (index + 1 == journals.size()) newest = scan;
    }

    if (!journals.empty()) {
        journal_epoch_ = journals.back().first;
        journal_path_ = journals.back().second;
        records_since_snapshot_ = newest.records;

        // The newest journal is the one later commits are appended to, and
        // recovery stops at the first record it cannot read. Whatever follows
        // such a record is never reached again, so nothing may be appended
        // behind one.
        if (!newest.readable) {
            return Error{ErrorCode::IoFailure,
                         "the newest journal could not be read; the state it holds cannot be recovered"};
        }
        if (!newest.header_valid) {
            // Not a journal this store can append to: the next commit starts a
            // new one above it.
            journal_path_.clear();
            records_since_snapshot_ = 0;
        } else if (newest.damaged) {
            if (newest.damage == ErrorCode::UnsupportedVersion) {
                return Error{ErrorCode::UnsupportedVersion,
                             "the newest journal holds records in a format this build does not read"};
            }
            // The discarded tail is removed, so the journal ends with its last
            // good record and the next commit follows it directly.
            std::error_code resize_error;
            std::filesystem::resize_file(journal_path_, newest.valid_bytes, resize_error);
            if (resize_error) {
                return Error{ErrorCode::IoFailure, "unable to remove the discarded journal tail"};
            }
        }
    }

    // Durable state never restores liveness, authority or freshness. Live
    // interventions become revalidation candidates instead.
    std::uint32_t live = 0;
    for (auto& intervention : report.state.interventions) {
        if (!intervention.live()) continue;
        live += 1;
        intervention.state = InterventionState::Revalidating;
    }
    report.interventions_requiring_revalidation = live;
    report.interventions_dropped_as_stale = live;
    report.stale_live_authority_dropped = live > 0;
    report.has_unfinished_attempts = live > 0;
    report.has_ambiguous_outcomes = report.journal_tail_ambiguous || report.snapshot_corrupt;
    report.state.requires_revalidation = true;
    report.state.epoch = EpochId{};
    report.state.boot = BootIncarnation{};
    report.state.restored_from_durable = true;
    report.state.restore_count += 1;
    report.state.recovery.reset();
    report.state.incident_streak = 0;
    report.state.clean_streak = 0;
    return report;
}

}  // namespace incast
