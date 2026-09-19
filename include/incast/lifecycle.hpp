// Incast Governor - intervention lifecycle, fencing, epochs and recovery.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_LIFECYCLE_HPP
#define INCAST_LIFECYCLE_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "incast/core.hpp"
#include "incast/detect.hpp"
#include "incast/ids.hpp"
#include "incast/mitigate.hpp"
#include "incast/policy.hpp"

namespace incast {

// ---------------------------------------------------------------------------
// Intervention lifecycle. Relaxation, retirement and revalidation are distinct
// states so that no code path can silently weaken a live intervention.
// ---------------------------------------------------------------------------

enum class InterventionState : std::uint8_t {
    Proposed = 0,
    Authorized = 1,
    Active = 2,
    Relaxing = 3,
    Revalidating = 4,
    Retired = 5,
    Fenced = 6,
    Refused = 7,
    Expired = 8,
};

[[nodiscard]] constexpr std::string_view to_string(InterventionState state) noexcept {
    switch (state) {
        case InterventionState::Proposed: return "Proposed";
        case InterventionState::Authorized: return "Authorized";
        case InterventionState::Active: return "Active";
        case InterventionState::Relaxing: return "Relaxing";
        case InterventionState::Revalidating: return "Revalidating";
        case InterventionState::Retired: return "Retired";
        case InterventionState::Fenced: return "Fenced";
        case InterventionState::Refused: return "Refused";
        case InterventionState::Expired: return "Expired";
    }
    return "Unknown";
}

[[nodiscard]] constexpr bool is_terminal_state(InterventionState state) noexcept {
    return state == InterventionState::Retired || state == InterventionState::Fenced ||
           state == InterventionState::Refused || state == InterventionState::Expired;
}

[[nodiscard]] constexpr bool is_live_state(InterventionState state) noexcept {
    return state == InterventionState::Authorized || state == InterventionState::Active ||
           state == InterventionState::Relaxing || state == InterventionState::Revalidating;
}

struct Intervention {
    InterventionId id{};
    InterventionGeneration generation{};
    DestinationId destination{};
    DestinationGeneration destination_generation{};
    WindowId window{};
    WindowGeneration window_generation{};

    InterventionState state = InterventionState::Proposed;
    MitigationIntent intent{};

    EpochId epoch{};
    BootIncarnation boot{};
    WorkerId worker{};
    LeaseId lease{};
    PolicyId policy{};
    PolicyGeneration policy_generation{};

    Timestamp created_at{};
    Timestamp updated_at{};
    Timestamp expires_at{};
    Timestamp last_relaxed_at{};

    BasisPoints applied_reduction_bp{};
    std::uint32_t relax_steps = 0;
    std::uint32_t continue_steps = 0;

    RefusalReason refusal = RefusalReason::None;
    std::string refusal_detail{};

    [[nodiscard]] bool live() const noexcept { return is_live_state(state); }
};

// Legal transition table. Any transition not listed is rejected with
// IllegalTransition; the table is the single source of truth.
[[nodiscard]] bool is_legal_transition(InterventionState from, InterventionState to) noexcept;

[[nodiscard]] Status transition(Intervention& intervention,
                                InterventionState to,
                                Timestamp now,
                                std::string detail = {});

// ---------------------------------------------------------------------------
// Fencing. Authority is only valid inside the epoch, boot incarnation, worker
// and lease that created it. Anything else is fenced, never re-derived.
// ---------------------------------------------------------------------------

struct FenceContext {
    EpochId epoch{};
    BootIncarnation boot{};
    WorkerId worker{};
    LeaseId lease{};
    Timestamp now{};
    Timestamp lease_expiry{};
};

[[nodiscard]] bool is_fenced(const Intervention& intervention,
                             const FenceContext& context,
                             RefusalReason& reason,
                             std::string& detail);

// ---------------------------------------------------------------------------
// Epoch tracking. The coordinator epoch advances on every authority
// acquisition; work from a previous epoch is rejected as stale.
// ---------------------------------------------------------------------------

class EpochTracker {
public:
    EpochTracker() = default;

    void reset(EpochId epoch, BootIncarnation boot) noexcept {
        epoch_ = epoch;
        boot_ = boot;
    }

    [[nodiscard]] EpochId current() const noexcept { return epoch_; }
    [[nodiscard]] BootIncarnation boot() const noexcept { return boot_; }

    // Advances the epoch. Monotonic by construction: a lower epoch is refused.
    [[nodiscard]] Status advance(EpochId requested);
    [[nodiscard]] bool accepts(EpochId epoch) const noexcept { return epoch.valid() && epoch == epoch_; }
    [[nodiscard]] bool accepts_boot(BootIncarnation boot) const noexcept {
        return boot.valid() && boot == boot_;
    }

private:
    EpochId epoch_{};
    BootIncarnation boot_{};
};

// ---------------------------------------------------------------------------
// Recovery accounting. Relaxation requires an explicit recovery policy and a
// sustained clean streak; a single clean window never relaxes an intervention.
// ---------------------------------------------------------------------------

struct RecoveryTracker {
    bool tracking = false;
    Timestamp since{};
    std::uint32_t clean_windows = 0;
    std::uint32_t incident_windows = 0;
    std::uint64_t drops_during_recovery = 0;
    bool capacity_revalidated = false;

    void reset() noexcept {
        tracking = false;
        since = Timestamp{};
        clean_windows = 0;
        incident_windows = 0;
        drops_during_recovery = 0;
        capacity_revalidated = false;
    }
};

struct RecoveryVerdict {
    bool relax = false;
    RefusalReason reason = RefusalReason::None;
    std::string detail{};
    BasisPoints step_bp{};
};

// Evaluates whether an intervention may relax during this window. Requires the
// policy to be explicitly enabled; otherwise the answer is always "hold".
[[nodiscard]] RecoveryVerdict evaluate_recovery(const RecoveryPolicy& policy,
                                                const RecoveryTracker& tracker,
                                                const FanInMetrics& metrics,
                                                bool policy_enabled);

}  // namespace incast

#endif  // INCAST_LIFECYCLE_HPP
