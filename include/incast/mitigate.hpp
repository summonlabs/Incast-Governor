// Incast Governor - bounded mitigation intent synthesis and authority vector.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_MITIGATE_HPP
#define INCAST_MITIGATE_HPP

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "incast/core.hpp"
#include "incast/detect.hpp"
#include "incast/ids.hpp"
#include "incast/model.hpp"
#include "incast/policy.hpp"

namespace incast {

// ---------------------------------------------------------------------------
// Mitigation intent kinds. The governor never enforces: it publishes bounded
// intent that an external enforcement plane may execute or ignore.
// ---------------------------------------------------------------------------

enum class MitigationKind : std::uint8_t {
    None = 0,
    PacingHint = 1,
    SenderStaggering = 2,
    TemporaryAdmissionReduction = 3,
    PriorityAwareRateReduction = 4,
    TemporaryHeadroomAdjustment = 5,
    CongestionEscalation = 6,
};

[[nodiscard]] constexpr std::string_view to_string(MitigationKind kind) noexcept {
    switch (kind) {
        case MitigationKind::None: return "None";
        case MitigationKind::PacingHint: return "PacingHint";
        case MitigationKind::SenderStaggering: return "SenderStaggering";
        case MitigationKind::TemporaryAdmissionReduction: return "TemporaryAdmissionReduction";
        case MitigationKind::PriorityAwareRateReduction: return "PriorityAwareRateReduction";
        case MitigationKind::TemporaryHeadroomAdjustment: return "TemporaryHeadroomAdjustment";
        case MitigationKind::CongestionEscalation: return "CongestionEscalation";
    }
    return "Unknown";
}

enum class MitigationScope : std::uint8_t {
    None = 0,
    Destination = 1,
    Queue = 2,
    SenderSet = 3,
    Resource = 4,
};

[[nodiscard]] constexpr std::string_view to_string(MitigationScope scope) noexcept {
    switch (scope) {
        case MitigationScope::None: return "None";
        case MitigationScope::Destination: return "Destination";
        case MitigationScope::Queue: return "Queue";
        case MitigationScope::SenderSet: return "SenderSet";
        case MitigationScope::Resource: return "Resource";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Authority vector: the complete, auditable justification for a decision. It
// records the state of every evidence class and every binding that was checked.
// ---------------------------------------------------------------------------

struct AuthorityVector {
    std::array<EvidenceState, kEvidenceClassCount> classes{};
    bool intervention_authorized = false;
    RefusalReason refusal = RefusalReason::MissingEvidence;
    std::string refusal_detail{};

    bool policy_enabled = false;
    bool policy_generation_bound = false;
    bool event_bound = false;
    bool destination_bound = false;
    bool window_bound = false;
    bool epoch_bound = false;
    bool boot_bound = false;
    bool lease_bound = false;
    bool provenance_bound = false;

    bool population_saturated = false;
    bool identity_collision_present = false;
    bool protected_floor_preserved = true;
    bool recovery_policy_present = false;
    bool bounds_respected = true;
    bool clamped = false;

    [[nodiscard]] std::size_t authoritative_class_count() const noexcept;
    [[nodiscard]] std::string summary() const;
};

// ---------------------------------------------------------------------------
// Mitigation intent: the bounded, generation-bound request handed to the
// enforcement plane.
// ---------------------------------------------------------------------------

struct MitigationIntent {
    MitigationKind kind = MitigationKind::None;
    MitigationScope scope = MitigationScope::None;

    InterventionId intervention{};
    InterventionGeneration generation{};
    EventId event{};
    EventGeneration event_generation{};
    DestinationId destination{};
    DestinationGeneration destination_generation{};
    WindowId window{};
    WindowGeneration window_generation{};
    PolicyId policy{};
    PolicyGeneration policy_generation{};

    EpochId epoch{};
    BootIncarnation boot{};
    WorkerId worker{};
    LeaseId lease{};

    Timestamp issued_at{};
    Timestamp expires_at{};
    Duration duration{};

    BasisPoints reduction_bp{};
    BasisPoints admission_reduction_bp{};
    BasisPoints aggregate_reduction_bp{};
    Bytes headroom_bytes{};
    std::uint32_t stagger_slots = 0;

    std::vector<SenderIdentity> penalized_senders{};
    std::vector<ServiceClass> affected_classes{};
    QueueId queue{};
    ResourceId resource{};

    Rate protected_offered{};
    Rate protected_floor{};
    Rate resulting_offered{};
    Rate required_reduction{};
    Rate achievable_reduction{};

    bool bounded = true;
    bool clamped = false;
    bool insufficient = false;
    bool escalating = false;
    bool relaxes_prior = false;
    bool continues_prior = false;

    [[nodiscard]] bool active() const noexcept { return kind != MitigationKind::None; }
    [[nodiscard]] bool expired_at(Timestamp now) const noexcept {
        return expires_at.valid() && now.valid() && now > expires_at;
    }
};

// ---------------------------------------------------------------------------
// Synthesis request. Everything the synthesizer is allowed to see is passed in
// explicitly; the synthesizer itself is stateless and deterministic.
// ---------------------------------------------------------------------------

struct SynthesisRequest {
    const EvidenceBundle* bundle = nullptr;
    const GovernorPolicy* policy = nullptr;
    const Classification* classification = nullptr;
    AuthorityCheck check{};

    InterventionId intervention{};
    InterventionGeneration generation{};
    Timestamp issued_at{};

    // When continuing an already-live intervention the synthesizer keeps the
    // identity stable and only adjusts magnitude.
    const MitigationIntent* prior = nullptr;

    // Set when the policy authorizes relaxation for this evaluation.
    bool recovery_relaxation_permitted = false;
    BasisPoints recovery_step_bp{};
};

struct SynthesisResult {
    MitigationIntent intent{};
    AuthorityVector authority{};
    std::vector<std::string> notes{};
};

[[nodiscard]] SynthesisResult synthesize(const SynthesisRequest& request);

// Clamps an intent into the policy bounds. Exposed so that callers can verify
// bound enforcement independently of synthesis.
[[nodiscard]] bool clamp_to_bounds(MitigationIntent& intent, const MitigationBounds& bounds) noexcept;

}  // namespace incast

#endif  // INCAST_MITIGATE_HPP
