// Incast Governor - core primitives: versioning, checked arithmetic, CRC, time, outcome.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_CORE_HPP
#define INCAST_CORE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace incast {

// ---------------------------------------------------------------------------
// Product / format versioning
// ---------------------------------------------------------------------------

inline constexpr std::string_view kProductName = "Incast Governor";
inline constexpr std::string_view kProductVersion = "1.0.0";
inline constexpr std::uint32_t kAbiVersion = 1;
inline constexpr std::uint16_t kWireProtocolVersion = 1;
inline constexpr std::uint16_t kSnapshotFormatVersion = 1;
inline constexpr std::uint16_t kJournalFormatVersion = 1;

// Hard structural bounds. Every externally influenced collection, payload and
// durable record is checked against these before allocation or acceptance.
inline constexpr std::size_t kMaxSendersPerWindow = 8192;
inline constexpr std::size_t kMaxQueueObservations = 512;
inline constexpr std::size_t kMaxPenalizedSenders = 1024;
inline constexpr std::size_t kMaxWindowHistory = 256;
inline constexpr std::size_t kMaxInterventions = 512;
inline constexpr std::size_t kMaxExplanationNotes = 128;
inline constexpr std::size_t kMaxExplanationBytes = 64 * 1024;
inline constexpr std::size_t kMaxFrameBytes = 1u << 20;
inline constexpr std::size_t kMaxJournalRecordBytes = 1u << 18;
inline constexpr std::size_t kMaxJournalRecordsBeforeSnapshot = 4096;
inline constexpr std::size_t kMaxScenarioEvents = 1u << 20;
inline constexpr std::size_t kMaxIdentifierChars = 96;

// ---------------------------------------------------------------------------
// Outcome: an explicit success/error carrier. Errors are never signalled by
// sentinel values; a partially constructed authoritative object is never
// returned as success.
// ---------------------------------------------------------------------------

enum class ErrorCode : std::uint16_t {
    None = 0,
    InvalidArgument,
    OutOfRange,
    Overflow,
    Underflow,
    DivideByZero,
    CapacityExceeded,
    MalformedInput,
    TruncatedInput,
    OversizedInput,
    ChecksumMismatch,
    UnsupportedVersion,
    NotFound,
    AlreadyExists,
    IllegalTransition,
    StaleGeneration,
    FencedEpoch,
    StaleBoot,
    NotAuthorized,
    IoFailure,
    CorruptState,
    Closed,
    ProtocolViolation,
    IdentityCollision,
    ProtectedFloorConflict,
    UnknownEvidence,
    Cancelled,
};

[[nodiscard]] constexpr std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::None: return "None";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::OutOfRange: return "OutOfRange";
        case ErrorCode::Overflow: return "Overflow";
        case ErrorCode::Underflow: return "Underflow";
        case ErrorCode::DivideByZero: return "DivideByZero";
        case ErrorCode::CapacityExceeded: return "CapacityExceeded";
        case ErrorCode::MalformedInput: return "MalformedInput";
        case ErrorCode::TruncatedInput: return "TruncatedInput";
        case ErrorCode::OversizedInput: return "OversizedInput";
        case ErrorCode::ChecksumMismatch: return "ChecksumMismatch";
        case ErrorCode::UnsupportedVersion: return "UnsupportedVersion";
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::AlreadyExists: return "AlreadyExists";
        case ErrorCode::IllegalTransition: return "IllegalTransition";
        case ErrorCode::StaleGeneration: return "StaleGeneration";
        case ErrorCode::FencedEpoch: return "FencedEpoch";
        case ErrorCode::StaleBoot: return "StaleBoot";
        case ErrorCode::NotAuthorized: return "NotAuthorized";
        case ErrorCode::IoFailure: return "IoFailure";
        case ErrorCode::CorruptState: return "CorruptState";
        case ErrorCode::Closed: return "Closed";
        case ErrorCode::ProtocolViolation: return "ProtocolViolation";
        case ErrorCode::IdentityCollision: return "IdentityCollision";
        case ErrorCode::ProtectedFloorConflict: return "ProtectedFloorConflict";
        case ErrorCode::UnknownEvidence: return "UnknownEvidence";
        case ErrorCode::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

struct Error {
    ErrorCode code = ErrorCode::None;
    std::string detail{};

    [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::None; }
};

template <class T>
class Outcome {
public:
    Outcome(T value) : storage_(std::move(value)) {}
    Outcome(Error error) : storage_(std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

    // Querying the error of a successful outcome is a diagnostic convenience:
    // it yields ErrorCode::None instead of throwing, so a logging or
    // explanation path can never terminate the process.
    [[nodiscard]] const Error& error() const& {
        static const Error no_error{ErrorCode::None, "outcome holds a value, not an error"};
        return storage_.index() == 1 ? std::get<1>(storage_) : no_error;
    }

private:
    std::variant<T, Error> storage_;
};

using Status = Outcome<std::monostate>;

[[nodiscard]] inline Status ok_status() { return Status(std::monostate{}); }
[[nodiscard]] inline Status fail(ErrorCode code, std::string detail = {}) {
    return Status(Error{code, std::move(detail)});
}

// ---------------------------------------------------------------------------
// Checked arithmetic for externally influenced sizes, capacities, rates,
// counters and time units.
// ---------------------------------------------------------------------------

namespace detail {
template <class T>
[[nodiscard]] constexpr bool add_would_overflow(T a, T b) noexcept {
    if constexpr (std::is_signed_v<T>) {
        if (b > 0 && a > std::numeric_limits<T>::max() - b) return true;
        if (b < 0 && a < std::numeric_limits<T>::min() - b) return true;
        return false;
    } else {
        return a > static_cast<T>(std::numeric_limits<T>::max() - b);
    }
}
}  // namespace detail

[[nodiscard]] inline std::optional<std::int64_t> checked_add(std::int64_t a, std::int64_t b) noexcept {
    if (detail::add_would_overflow(a, b)) return std::nullopt;
    return a + b;
}

[[nodiscard]] inline std::optional<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) noexcept {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) return std::nullopt;
    return a + b;
}

[[nodiscard]] inline std::optional<std::uint64_t> checked_add(std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint64_t sum = static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b);
    if (sum > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return sum;
}

[[nodiscard]] inline std::optional<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b) noexcept {
    if (a == 0 || b == 0) return std::uint64_t{0};
    if (a > std::numeric_limits<std::uint64_t>::max() / b) return std::nullopt;
    return a * b;
}

[[nodiscard]] inline std::optional<std::int64_t> checked_mul(std::int64_t a, std::int64_t b) noexcept {
    if (a == 0 || b == 0) return std::int64_t{0};
    const std::int64_t max = std::numeric_limits<std::int64_t>::max();
    const std::int64_t min = std::numeric_limits<std::int64_t>::min();
    if (a > 0) {
        if (b > 0) { if (a > max / b) return std::nullopt; }
        else { if (b < min / a) return std::nullopt; }
    } else {
        if (b > 0) { if (a < min / b) return std::nullopt; }
        else { if (a != 0 && b < max / a) return std::nullopt; }
    }
    return a * b;
}

[[nodiscard]] inline std::optional<std::int64_t> checked_div(std::int64_t a, std::int64_t b) noexcept {
    if (b == 0) return std::nullopt;
    if (a == std::numeric_limits<std::int64_t>::min() && b == -1) return std::nullopt;
    return a / b;
}

// Widening helpers keep externally influenced conversions explicit.
template <class To, class From>
[[nodiscard]] inline std::optional<To> narrow(From value) noexcept {
    static_assert(std::is_integral_v<To> && std::is_integral_v<From>, "narrow requires integral types");
    if constexpr (std::is_signed_v<From> == std::is_signed_v<To>) {
        if (value < static_cast<From>(std::numeric_limits<To>::min()) ||
            value > static_cast<From>(std::numeric_limits<To>::max())) {
            return std::nullopt;
        }
    } else if constexpr (std::is_signed_v<From>) {
        if (value < 0) return std::nullopt;
        using UFrom = std::make_unsigned_t<From>;
        if (static_cast<UFrom>(value) > static_cast<UFrom>(std::numeric_limits<To>::max())) return std::nullopt;
    } else {
        if (value > static_cast<From>(std::numeric_limits<To>::max())) return std::nullopt;
    }
    return static_cast<To>(value);
}

[[nodiscard]] constexpr std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) noexcept {
    const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    return a > max - b ? max : a + b;
}

[[nodiscard]] constexpr std::uint64_t saturating_mul(std::uint64_t a, std::uint64_t b) noexcept {
    if (a == 0 || b == 0) return 0;
    const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    return a > max / b ? max : a * b;
}

// ---------------------------------------------------------------------------
// CRC-32C (Castagnoli). Used for frame integrity and durable record integrity.
// ---------------------------------------------------------------------------

[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t size) noexcept;
[[nodiscard]] inline std::uint32_t crc32c(std::string_view text) noexcept {
    return crc32c(text.data(), text.size());
}

// ---------------------------------------------------------------------------
// Hashing support for strong identifiers used as map keys.
// ---------------------------------------------------------------------------

inline void hash_combine(std::size_t& seed, std::size_t value) noexcept {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

}  // namespace incast

#endif  // INCAST_CORE_HPP
