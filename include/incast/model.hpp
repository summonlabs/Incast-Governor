// Incast Governor - evidence, capacity, sender population and queue model.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_MODEL_HPP
#define INCAST_MODEL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "incast/core.hpp"
#include "incast/ids.hpp"

namespace incast {

// ---------------------------------------------------------------------------
// Units. Distinct types keep rates, byte counts, durations and timestamps from
// being interchanged by accident.
// ---------------------------------------------------------------------------

class Timestamp {
public:
    constexpr Timestamp() noexcept = default;
    explicit constexpr Timestamp(std::int64_t nanos_since_epoch) noexcept : nanos_(nanos_since_epoch) {}

    [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return nanos_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return nanos_ != 0; }

    friend constexpr bool operator==(Timestamp, Timestamp) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(Timestamp, Timestamp) noexcept = default;

private:
    std::int64_t nanos_{0};
};

class Duration {
public:
    constexpr Duration() noexcept = default;
    explicit constexpr Duration(std::int64_t nanos) noexcept : nanos_(nanos) {}

    [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return nanos_; }
    [[nodiscard]] constexpr bool positive() const noexcept { return nanos_ > 0; }

    friend constexpr bool operator==(Duration, Duration) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(Duration, Duration) noexcept = default;

private:
    std::int64_t nanos_{0};
};

class Rate {
public:
    constexpr Rate() noexcept = default;
    explicit constexpr Rate(std::uint64_t bytes_per_second) noexcept : bps_(bytes_per_second) {}

    [[nodiscard]] constexpr std::uint64_t bytes_per_second() const noexcept { return bps_; }
    [[nodiscard]] constexpr bool positive() const noexcept { return bps_ != 0; }

    friend constexpr bool operator==(Rate, Rate) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(Rate, Rate) noexcept = default;

private:
    std::uint64_t bps_{0};
};

class Bytes {
public:
    constexpr Bytes() noexcept = default;
    explicit constexpr Bytes(std::uint64_t count) noexcept : count_(count) {}

    [[nodiscard]] constexpr std::uint64_t count() const noexcept { return count_; }

    friend constexpr bool operator==(Bytes, Bytes) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(Bytes, Bytes) noexcept = default;

private:
    std::uint64_t count_{0};
};

// Basis points: 10000 == 1.0. All externally supplied ratios are clamped into
// range before use so that no downstream comparison can overflow.
class BasisPoints {
public:
    constexpr BasisPoints() noexcept = default;
    explicit constexpr BasisPoints(std::uint32_t value) noexcept
        : value_(value > kMax ? kMax : value), clamped_(value > kMax) {}

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool clamped() const noexcept { return clamped_; }
    [[nodiscard]] constexpr bool at_least(std::uint32_t threshold) const noexcept { return value_ >= threshold; }
    [[nodiscard]] constexpr bool below(std::uint32_t threshold) const noexcept { return value_ < threshold; }

    friend constexpr bool operator==(BasisPoints, BasisPoints) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(BasisPoints, BasisPoints) noexcept = default;

    static constexpr std::uint32_t kMax = 1000000;  // 100x, guards absurd inputs

private:
    std::uint32_t value_{0};
    bool clamped_{false};
};

// Monotonic process clock. Returns nanoseconds from an unspecified epoch; only
// differences are meaningful, and the value is never persisted as wall time.
[[nodiscard]] Timestamp monotonic_now() noexcept;

[[nodiscard]] inline Duration elapsed_since(Timestamp start) noexcept {
    const Timestamp current = monotonic_now();
    return current >= start ? Duration{current.nanos() - start.nanos()} : Duration{0};
}

// Ratio helper in basis points with saturation; never divides by zero.
[[nodiscard]] inline BasisPoints ratio_bp(std::uint64_t numerator, std::uint64_t denominator) noexcept {
    if (denominator == 0) return BasisPoints{0};
    const std::uint64_t scaled = saturating_mul(numerator, 10000ULL);
    if (scaled == std::numeric_limits<std::uint64_t>::max()) return BasisPoints{BasisPoints::kMax};
    const std::uint64_t result = scaled / denominator;
    const std::uint64_t capped = result > BasisPoints::kMax ? BasisPoints::kMax : result;
    return BasisPoints{static_cast<std::uint32_t>(capped)};
}

// ---------------------------------------------------------------------------
// Service classes. Stronger classes carry stronger obligations: mitigation may
// never push a stronger class below the protected floor of a weaker one.
// ---------------------------------------------------------------------------

enum class ServiceClass : std::uint8_t {
    BestEffort = 0,
    Bulk = 1,
    Interactive = 2,
    LatencySensitive = 3,
    Control = 4,
    Replication = 5,
};

inline constexpr std::size_t kServiceClassCount = 6;

[[nodiscard]] constexpr std::uint8_t service_strength(ServiceClass klass) noexcept {
    return static_cast<std::uint8_t>(klass);
}

[[nodiscard]] constexpr ServiceClass stronger_of(ServiceClass a, ServiceClass b) noexcept {
    return service_strength(a) >= service_strength(b) ? a : b;
}

[[nodiscard]] constexpr bool stronger_than(ServiceClass a, ServiceClass b) noexcept {
    return service_strength(a) > service_strength(b);
}

[[nodiscard]] constexpr std::string_view to_string(ServiceClass klass) noexcept {
    switch (klass) {
        case ServiceClass::BestEffort: return "BestEffort";
        case ServiceClass::Bulk: return "Bulk";
        case ServiceClass::Interactive: return "Interactive";
        case ServiceClass::LatencySensitive: return "LatencySensitive";
        case ServiceClass::Control: return "Control";
        case ServiceClass::Replication: return "Replication";
    }
    return "Unknown";
}

[[nodiscard]] std::optional<ServiceClass> parse_service_class(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Evidence classification. Every externally supplied evidence item is stamped
// so that the governor can distinguish authoritative evidence from UNKNOWN,
// stale, contradictory, reordered or superseded evidence.
// ---------------------------------------------------------------------------

enum class EvidenceClass : std::uint8_t {
    FanIn = 0,
    ArrivalTiming = 1,
    AggregateRate = 2,
    Destination = 3,
    Capacity = 4,
    Queue = 5,
    Service = 6,
    Provenance = 7,
};

inline constexpr std::size_t kEvidenceClassCount = 8;

[[nodiscard]] constexpr std::string_view to_string(EvidenceClass klass) noexcept {
    switch (klass) {
        case EvidenceClass::FanIn: return "FanIn";
        case EvidenceClass::ArrivalTiming: return "ArrivalTiming";
        case EvidenceClass::AggregateRate: return "AggregateRate";
        case EvidenceClass::Destination: return "Destination";
        case EvidenceClass::Capacity: return "Capacity";
        case EvidenceClass::Queue: return "Queue";
        case EvidenceClass::Service: return "Service";
        case EvidenceClass::Provenance: return "Provenance";
    }
    return "Unknown";
}

enum class EvidenceState : std::uint8_t {
    Unknown = 0,
    Present = 1,
    Stale = 2,
    Contradictory = 3,
    Reordered = 4,
    OutOfWindow = 5,
    Superseded = 6,
};

[[nodiscard]] constexpr std::string_view to_string(EvidenceState state) noexcept {
    switch (state) {
        case EvidenceState::Unknown: return "Unknown";
        case EvidenceState::Present: return "Present";
        case EvidenceState::Stale: return "Stale";
        case EvidenceState::Contradictory: return "Contradictory";
        case EvidenceState::Reordered: return "Reordered";
        case EvidenceState::OutOfWindow: return "OutOfWindow";
        case EvidenceState::Superseded: return "Superseded";
    }
    return "Unknown";
}

struct EvidenceStamp {
    EvidenceState state = EvidenceState::Unknown;
    EvidenceClass klass = EvidenceClass::FanIn;
    WindowId window{};
    WindowGeneration window_generation{};
    ProvenanceId provenance{};
    ProvenanceGeneration provenance_generation{};
    Timestamp observed_at{};
    Timestamp valid_until{};
    std::uint64_t sequence = 0;   // per-class monotonic sequence within a source
    std::uint32_t sample_count = 0;
    bool truncated = false;       // true when a bounded collection was clipped

    // Authoritative means: explicitly present, observed at or before the
    // evaluation instant, not past its validity horizon, and bound to the
    // window under evaluation. Anything else is refused.
    [[nodiscard]] bool authoritative_at(Timestamp now, WindowId expected_window) const noexcept {
        if (state != EvidenceState::Present) return false;
        if (expected_window.valid() && window != expected_window) return false;
        if (!observed_at.valid()) return false;
        if (now.valid() && observed_at > now) return false;
        if (valid_until.valid()) {
            if (!now.valid()) return false;
            if (now > valid_until) return false;
        }
        return true;
    }

    [[nodiscard]] bool has_freshness_horizon() const noexcept { return valid_until.valid(); }
};

template <class T>
struct Evidenced {
    T value{};
    EvidenceStamp stamp{};
};

// ---------------------------------------------------------------------------
// Observations supplied by the fabric observation plane.
// ---------------------------------------------------------------------------

struct SenderObservation {
    SenderIdentity identity{};
    ServiceClass service = ServiceClass::BestEffort;
    bool protected_obligation = false;
    Rate offered_rate{};      // bytes/second offered by this sender in the window
    Bytes transferred{};      // bytes completed in the window
    Bytes outstanding{};      // bytes still queued at the destination
    Timestamp first_arrival{};
    Timestamp last_arrival{};
    std::uint32_t attempts = 1;  // retransmission / attempt count
};

struct QueueObservation {
    QueueId queue{};
    ResourceId resource{};
    Bytes occupancy{};
    Bytes capacity{};
    Bytes high_watermark{};
    std::uint64_t drop_events = 0;
    std::uint64_t ecn_marks = 0;
    Timestamp observed_at{};
    Generation<ResourceTag> resource_generation{};
};

struct DestinationCapacity {
    DestinationId destination{};
    ResourceId resource{};
    CapacityGeneration resource_generation{};
    Rate service_rate{};             // sustained drain rate of the destination
    Bytes queue_capacity{};
    Bytes buffer_capacity{};
    std::uint32_t admission_limit = 0;      // configured concurrent sender limit
    Rate protected_floor_rate{};            // aggregate protected rate that must survive
    ServiceClass strongest_protected = ServiceClass::BestEffort;
};

// ---------------------------------------------------------------------------
// The complete, self-describing evidence bundle evaluated by the governor.
// A decision is a pure function of this bundle plus policy and clock.
// ---------------------------------------------------------------------------

struct EvidenceBundle {
    EventId event{};
    EventGeneration event_generation{};
    DestinationId destination{};
    DestinationGeneration destination_generation{};
    WindowId window{};
    WindowGeneration window_generation{};
    Timestamp window_start{};
    Timestamp window_end{};
    Timestamp now{};

    EvidenceStamp fan_in_stamp{};
    std::vector<SenderObservation> senders{};

    EvidenceStamp capacity_stamp{};
    DestinationCapacity capacity{};

    EvidenceStamp queue_stamp{};
    std::vector<QueueObservation> queues{};

    EvidenceStamp timing_stamp{};
    EvidenceStamp rate_stamp{};
    EvidenceStamp service_stamp{};
    EvidenceStamp provenance_stamp{};

    EvidenceStamp policy_stamp{};

    // Authority binding: which epoch, boot, worker and lease this bundle was
    // produced under. Everything here is validated before a decision is made.
    EpochId epoch{};
    BootIncarnation boot{};
    WorkerId worker{};
    LeaseId lease{};
    Timestamp lease_expiry{};

    [[nodiscard]] bool structurally_valid() const noexcept;
};

// ---------------------------------------------------------------------------
// Derived metrics. Purely computed from the bundle; no hidden state.
// ---------------------------------------------------------------------------

struct FanInMetrics {
    std::uint32_t active_senders = 0;
    std::uint32_t distinct_flows = 0;
    bool population_saturated = false;
    std::uint32_t protected_senders = 0;
    std::uint32_t collided_senders = 0;

    Timestamp earliest_arrival{};
    Timestamp latest_arrival{};
    Duration arrival_spread{};
    Duration window_width{};
    std::uint32_t synchronization_bp = 0;  // 10000 == fully synchronized arrival

    Rate aggregate_offered{};
    Rate synchronized_offered{};
    Rate protected_offered{};
    Rate penalizable_offered{};

    Rate service_rate{};
    Bytes queue_capacity{};
    Bytes queue_occupancy{};
    Bytes high_watermark{};
    Bytes buffer_headroom{};
    BasisPoints queue_pressure_bp{};
    BasisPoints oversubscription_bp{};
    std::uint64_t drop_events = 0;
    std::uint64_t ecn_marks = 0;

    std::vector<SenderCollision> collisions{};
};

}  // namespace incast

#endif  // INCAST_MODEL_HPP
