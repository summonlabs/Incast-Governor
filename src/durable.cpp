// Incast Governor - crash-safe durable store implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "incast/durable.hpp"

#include <algorithm>
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
    if (length > kMaxJournalRecordBytes) {
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
    if (payload.size() > kMaxJournalRecordBytes) {
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
    const std::filesystem::path previous = journal_path_;
    const Status rotated = rotate_journal();
    if (!rotated) return rotated;
    if (!previous.empty() && previous != journal_path_) {
        std::error_code ignored;
        std::filesystem::remove(previous, ignored);
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

    // Journals are ordered by their numeric rotation index, not by their file
    // name: lexicographic ordering would place "governor.10.journal" before
    // "governor.9.journal" and replay records out of order.
    std::vector<std::pair<std::uint64_t, std::filesystem::path>> journals;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(config_.directory, error)) {
        if (error) break;
        const std::string name = entry.path().filename().string();
        if (name.rfind("governor.", 0) != 0 || entry.path().extension() != ".journal") continue;
        const std::string digits = name.substr(std::string("governor.").size(),
                                               name.size() - std::string("governor.").size() -
                                                   std::string(".journal").size());
        const auto index = parse_u64(digits);
        journals.emplace_back(index.value_or(0), entry.path());
    }
    if (error) return Error{ErrorCode::IoFailure, "unable to enumerate the durable state directory"};
    std::sort(journals.begin(), journals.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });

    if (!journals.empty()) report.journal_present = true;
    if (!journals.empty()) {
        journal_path_ = journals.back().second;
        journal_epoch_ = journals.back().first;
    }

    for (const auto& [rotation, path] : journals) {
        static_cast<void>(rotation);
        auto content = read_file(path);
        if (!content) {
            report.notes.emplace_back("journal could not be read: " + path.filename().string());
            continue;
        }
        const auto bytes = content.value();
        if (bytes.size() < kJournalFileHeaderBytes ||
            std::memcmp(bytes.data(), kJournalFileHeader.data(), kJournalFileHeader.size()) != 0) {
            report.notes.emplace_back("journal header invalid: " + path.filename().string());
            continue;
        }
        std::size_t offset = kJournalFileHeaderBytes;
        while (offset < bytes.size()) {
            JournalRecordType type = JournalRecordType::StateCommit;
            std::span<const std::byte> payload{};
            std::size_t consumed = 0;
            const Status decoded = decode_journal_record(
                std::span<const std::byte>(bytes.data() + offset, bytes.size() - offset), type, payload, consumed);
            if (!decoded) {
                const std::size_t remaining = bytes.size() - offset;
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
                break;
            }
            offset += consumed;
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
