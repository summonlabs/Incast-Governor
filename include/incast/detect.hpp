// Incast Governor - synchronization analysis, classification and contribution ranking.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_DETECT_HPP
#define INCAST_DETECT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "incast/core.hpp"
#include "incast/ids.hpp"
#include "incast/model.hpp"
#include "incast/policy.hpp"

namespace incast {

// ---------------------------------------------------------------------------
// Incident classification.
// ---------------------------------------------------------------------------

enum class IncidentKind : std::uint8_t {
    Unknown = 0,               // evidence insufficient or refused: UNKNOWN stays UNKNOWN
    NoFanIn = 1,               // observed traffic is not many-to-one
    OrdinaryManyToOne = 2,     // many senders, arrivals not synchronized
    SynchronizedIncastRisk = 3,// synchronized fan-in approaching collapse
    ActiveIncast = 4,          // synchronized fan-in actively collapsing service
    Recovery = 5,              // previously active, now inside the recovery band
};

[[nodiscard]] constexpr std::string_view to_string(IncidentKind kind) noexcept {
    switch (kind) {
        case IncidentKind::Unknown: return "Unknown";
        case IncidentKind::NoFanIn: return "NoFanIn";
        case IncidentKind::OrdinaryManyToOne: return "OrdinaryManyToOne";
        case IncidentKind::SynchronizedIncastRisk: return "SynchronizedIncastRisk";
        case IncidentKind::ActiveIncast: return "ActiveIncast";
        case IncidentKind::Recovery: return "Recovery";
    }
    return "Unknown";
}

enum class Severity : std::uint8_t {
    None = 0,
    Advisory = 1,
    Elevated = 2,
    Severe = 3,
    Critical = 4,
};

[[nodiscard]] constexpr std::string_view to_string(Severity severity) noexcept {
    switch (severity) {
        case Severity::None: return "None";
        case Severity::Advisory: return "Advisory";
        case Severity::Elevated: return "Elevated";
        case Severity::Severe: return "Severe";
        case Severity::Critical: return "Critical";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Why an intervention was refused. Refusals are first-class outcomes: the
// governor reports the exact authority that was missing rather than degrading
// silently into an unauthorized action.
// ---------------------------------------------------------------------------

enum class RefusalReason : std::uint8_t {
    None = 0,
    PolicyDisabled,
    NoIncident,
    MissingEvidence,
    StaleEvidence,
    ContradictoryEvidence,
    ReorderedEvidence,
    SupersededEvidence,
    OutOfWindowEvidence,
    EventMismatch,
    DestinationMismatch,
    PolicyGenerationMismatch,
    EpochMismatch,
    BootMismatch,
    LeaseMissing,
    LeaseExpired,
    IdentityCollision,
    ProtectedFloorConflict,
    NoMitigablePopulation,
    BoundsExceeded,
    AwaitingRecoveryPolicy,
    MalformedBundle,
    CapacityNotRevalidated,
};

[[nodiscard]] constexpr std::string_view to_string(RefusalReason reason) noexcept {
    switch (reason) {
        case RefusalReason::None: return "None";
        case RefusalReason::PolicyDisabled: return "PolicyDisabled";
        case RefusalReason::NoIncident: return "NoIncident";
        case RefusalReason::MissingEvidence: return "MissingEvidence";
        case RefusalReason::StaleEvidence: return "StaleEvidence";
        case RefusalReason::ContradictoryEvidence: return "ContradictoryEvidence";
        case RefusalReason::ReorderedEvidence: return "ReorderedEvidence";
        case RefusalReason::SupersededEvidence: return "SupersededEvidence";
        case RefusalReason::OutOfWindowEvidence: return "OutOfWindowEvidence";
        case RefusalReason::EventMismatch: return "EventMismatch";
        case RefusalReason::DestinationMismatch: return "DestinationMismatch";
        case RefusalReason::PolicyGenerationMismatch: return "PolicyGenerationMismatch";
        case RefusalReason::EpochMismatch: return "EpochMismatch";
        case RefusalReason::BootMismatch: return "BootMismatch";
        case RefusalReason::LeaseMissing: return "LeaseMissing";
        case RefusalReason::LeaseExpired: return "LeaseExpired";
        case RefusalReason::IdentityCollision: return "IdentityCollision";
        case RefusalReason::ProtectedFloorConflict: return "ProtectedFloorConflict";
        case RefusalReason::NoMitigablePopulation: return "NoMitigablePopulation";
        case RefusalReason::BoundsExceeded: return "BoundsExceeded";
        case RefusalReason::AwaitingRecoveryPolicy: return "AwaitingRecoveryPolicy";
        case RefusalReason::MalformedBundle: return "MalformedBundle";
        case RefusalReason::CapacityNotRevalidated: return "CapacityNotRevalidated";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Synchronization evidence. Synchronization is demonstrated by arrival timing
// and by the share of aggregate demand carried inside the compressed window,
// never by the sender count alone.
// ---------------------------------------------------------------------------

struct SynchronizationEvidence {
    bool evaluated = false;
    std::uint32_t population = 0;
    std::uint32_t synchronized_senders = 0;
    Duration arrival_spread{};
    Duration window_width{};
    Duration sync_window_bound{};
    BasisPoints arrival_compression_bp{};   // 1 - spread/window_width
    BasisPoints synchronized_share_bp{};    // synchronized offer / aggregate offer
    bool population_floor_met = false;
    bool window_compressed = false;
    bool share_sufficient = false;
    bool synchronized = false;
    bool saturated = false;

    [[nodiscard]] std::string summary() const;
};

// ---------------------------------------------------------------------------
// Per-sender contribution. Ranked deterministically by offered rate, then by
// identity, so that equal inputs always yield equal explanations.
// ---------------------------------------------------------------------------

struct SenderContribution {
    SenderIdentity identity{};
    ServiceClass service = ServiceClass::BestEffort;
    bool protected_obligation = false;
    bool collided = false;
    bool synchronized = false;
    bool mitigable = false;
    Rate offered_rate{};
    BasisPoints offered_share_bp{};
    BasisPoints aggregate_share_bp{};
    Duration arrival_offset{};
    std::string disposition{};  // human-readable, bounded
};

// ---------------------------------------------------------------------------
// Classification result.
// ---------------------------------------------------------------------------

struct ClassificationContext {
    IncidentKind prior_kind = IncidentKind::Unknown;
    std::uint32_t incident_streak = 0;
    std::uint32_t clean_streak = 0;
};

struct Classification {
    IncidentKind kind = IncidentKind::Unknown;
    IncidentKind instantaneous_kind = IncidentKind::Unknown;
    Severity severity = Severity::None;
    bool authoritative = false;
    bool hysteresis_held = false;
    RefusalReason refusal = RefusalReason::MissingEvidence;
    std::string refusal_detail{};

    FanInMetrics metrics{};
    SynchronizationEvidence synchronization{};
    std::vector<SenderContribution> contributions{};
    std::vector<std::string> notes{};
};

struct AuthorityCheck {
    RefusalReason reason = RefusalReason::None;
    std::string detail{};
    std::array<EvidenceState, kEvidenceClassCount> classes{};
};

// Reports the dominant ordering/validity refusal for the bundle. A required
// class that is not Present denies authority; the first denying class in
// evidence-class order wins so the answer is deterministic.
[[nodiscard]] AuthorityCheck check_authority(
    const EvidenceBundle& bundle,
    const GovernorPolicy& policy,
    const std::array<std::uint64_t, kEvidenceClassCount>& last_sequence);

// Detects duplicate/contradictory sender identities. Collided identities are
// never penalized because the governor cannot prove which live sender the
// identifier belongs to.
[[nodiscard]] std::vector<SenderCollision> detect_collisions(const EvidenceBundle& bundle);

// ---------------------------------------------------------------------------
// Detection entry points. Both are pure: identical inputs produce identical
// outputs, and no internal state is consulted or mutated.
// ---------------------------------------------------------------------------

[[nodiscard]] FanInMetrics compute_metrics(const EvidenceBundle& bundle, const GovernorPolicy& policy);

[[nodiscard]] Classification classify(const EvidenceBundle& bundle,
                                      const GovernorPolicy& policy,
                                      std::vector<SenderCollision> collisions,
                                      const ClassificationContext& context = {});

// Instantaneous (hysteresis-free) classification of the metrics.
[[nodiscard]] IncidentKind classify_instantaneous(const FanInMetrics& metrics,
                                                  const SynchronizationEvidence& synchronization,
                                                  const GovernorPolicy& policy);

[[nodiscard]] Severity derive_severity(IncidentKind kind,
                                       const FanInMetrics& metrics,
                                       const GovernorPolicy& policy);

// True when the sender may never be penalized: either it carries an explicit
// protected obligation, or its service class is stronger than the configured
// mitigable class ceiling.
[[nodiscard]] bool is_protected_sender(const SenderObservation& sender, const GovernorPolicy& policy);

// True when the sender's first arrival falls inside the synchronized window
// anchored on the earliest arrival of the population.
[[nodiscard]] bool is_synchronized_sender(const SenderObservation& sender,
                                          Timestamp earliest,
                                          const SynchronizationEvidence& synchronization);

// Ranks contributions deterministically and selects the mitigable subset.
[[nodiscard]] std::vector<SenderContribution> rank_contributions(
    const EvidenceBundle& bundle,
    const GovernorPolicy& policy,
    const FanInMetrics& metrics,
    const SynchronizationEvidence& synchronization,
    const std::vector<SenderCollision>& collisions);

}  // namespace incast

#endif  // INCAST_DETECT_HPP
