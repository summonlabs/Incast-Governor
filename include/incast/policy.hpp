// Incast Governor - policy, thresholds, hysteresis, protection and authority rules.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_POLICY_HPP
#define INCAST_POLICY_HPP

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "incast/core.hpp"
#include "incast/ids.hpp"
#include "incast/model.hpp"

namespace incast {

// ---------------------------------------------------------------------------
// Synchronization thresholds. Sender count alone never proves synchronization:
// both a population floor and an arrival-compression condition must hold.
// ---------------------------------------------------------------------------

struct SynchronizationPolicy {
    std::uint32_t many_to_one_min_senders = 4;
    std::uint32_t min_synchronized_senders = 8;
    Duration sync_window_bound{50'000};          // 50 microseconds
    std::uint32_t sync_index_threshold_bp = 7000;  // arrivals occupy <= 70% of window
    std::uint32_t min_aggregate_share_bp = 6000;   // synchronized senders carry >= 60% of offer
};

// ---------------------------------------------------------------------------
// Pressure thresholds.
// ---------------------------------------------------------------------------

struct PressurePolicy {
    std::uint32_t risk_oversubscription_bp = 12'500;     // 1.25x service rate
    std::uint32_t collapse_oversubscription_bp = 20'000; // 2.00x service rate
    std::uint32_t active_queue_pressure_bp = 8'000;      // 80% occupancy
    std::uint32_t congestion_queue_pressure_bp = 6'000;  // 60% occupancy
    std::uint64_t drop_events_for_active = 1;
    std::uint32_t headroom_warning_bp = 2'500;           // < 25% buffer headroom
};

// ---------------------------------------------------------------------------
// Recovery. Relaxation is never implicit: without an enabled recovery policy an
// intervention holds until it expires, and expiry forces revalidation rather
// than silent relaxation.
// ---------------------------------------------------------------------------

struct RecoveryPolicy {
    bool enabled = false;
    std::uint32_t required_clean_windows = 3;
    Duration dwell{0};
    std::uint32_t relax_oversubscription_bp = 9'000;  // must fall below 0.90x
    std::uint32_t relax_queue_pressure_bp = 5'000;    // must fall below 50%
    std::uint64_t max_drops_during_recovery = 0;
    bool require_capacity_revalidation = true;
};

// ---------------------------------------------------------------------------
// Hysteresis. Entry and exit thresholds are separated by margins and a minimum
// dwell so classification cannot oscillate between adjacent windows.
// ---------------------------------------------------------------------------

struct HysteresisPolicy {
    std::uint32_t enter_margin_bp = 500;
    std::uint32_t exit_margin_bp = 1'500;
    std::uint32_t min_dwell_windows = 2;
    std::uint32_t max_incident_windows = 4'096;
    std::uint32_t max_clean_windows = 4'096;
};

// ---------------------------------------------------------------------------
// Protection. Stronger obligations remain protected: mitigation is applied to
// the weakest classes first and never pushes aggregate protected demand below
// the configured floor.
// ---------------------------------------------------------------------------

struct ProtectionPolicy {
    Rate protected_floor_rate{};                    // 0 == derive from evidence
    ServiceClass strongest_mitigable_class = ServiceClass::Bulk;
    bool never_penalize_explicitly_protected = true;
    bool refuse_sender_scope_when_population_saturated = true;
    bool escalate_instead_of_violating_floor = true;
};

// ---------------------------------------------------------------------------
// Mitigation bounds. Every intent is clamped into these limits before it can be
// authorized, so no policy value can request unbounded intervention.
// ---------------------------------------------------------------------------

struct MitigationBounds {
    std::uint32_t min_reduction_bp = 100;
    std::uint32_t max_reduction_bp = 5'000;
    std::uint32_t max_admission_reduction_bp = 5'000;
    Bytes max_headroom_bytes{1u << 20};
    Duration min_duration{100'000'000};   // 100 ms
    Duration max_duration{5'000'000'000}; // 5 s
    std::size_t max_penalized_senders = kMaxPenalizedSenders;
    std::uint32_t max_stagger_slots = 64;
};

// ---------------------------------------------------------------------------
// Authority. Which evidence classes must be authoritative before any
// intervention may be authorized. A required class that is UNKNOWN, stale,
// contradictory, reordered or out of window denies authority outright.
// ---------------------------------------------------------------------------

struct AuthorityPolicy {
    std::array<bool, kEvidenceClassCount> required_classes{
        true,   // FanIn
        true,   // ArrivalTiming
        true,   // AggregateRate
        true,   // Destination
        true,   // Capacity
        true,   // Queue
        false,  // Service (optional, but validated when present)
        true,   // Provenance
    };
    bool require_provenance = true;
    bool require_valid_lease = true;
    bool require_matching_epoch = true;
    bool require_matching_boot = true;
    bool refuse_on_identity_collision = true;
    bool refuse_on_event_mismatch = true;
    std::size_t max_freshness_horizon_nanos = 2'000'000'000;  // evidence may not be older than 2 s
};

// ---------------------------------------------------------------------------
// The complete policy document. Policies are versioned by generation; a policy
// change is a new generation and forces revalidation of live interventions.
// ---------------------------------------------------------------------------

struct GovernorPolicy {
    PolicyId id{};
    PolicyGeneration generation{};
    std::string name{"default"};
    bool enabled = true;

    SynchronizationPolicy synchronization{};
    PressurePolicy pressure{};
    RecoveryPolicy recovery{};
    HysteresisPolicy hysteresis{};
    ProtectionPolicy protection{};
    MitigationBounds bounds{};
    AuthorityPolicy authority{};

    // Validates internal consistency. A contradictory policy is refused at
    // construction time rather than producing contradictory decisions later.
    [[nodiscard]] Status validate() const;

    // A stable digest of the policy content; two policies with equal digests
    // must produce equal decisions for equal evidence.
    [[nodiscard]] std::uint64_t digest() const noexcept;
};

// Default policy document bound to the well-known default policy identity.
[[nodiscard]] GovernorPolicy make_default_policy();

}  // namespace incast

#endif  // INCAST_POLICY_HPP
