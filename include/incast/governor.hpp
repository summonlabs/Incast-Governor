// Incast Governor - the authoritative, generation-bound governance engine.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_GOVERNOR_HPP
#define INCAST_GOVERNOR_HPP

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "incast/core.hpp"
#include "incast/detect.hpp"
#include "incast/explain.hpp"
#include "incast/ids.hpp"
#include "incast/lifecycle.hpp"
#include "incast/mitigate.hpp"
#include "incast/model.hpp"
#include "incast/policy.hpp"

namespace incast {

// ---------------------------------------------------------------------------
// Per-window record retained for hysteresis and dwell accounting. The history
// is bounded; the oldest record is dropped when the bound is reached.
// ---------------------------------------------------------------------------

struct WindowRecord {
    WindowId window{};
    WindowGeneration window_generation{};
    Timestamp start{};
    Timestamp end{};
    IncidentKind kind = IncidentKind::Unknown;
    Severity severity = Severity::None;
    BasisPoints oversubscription_bp{};
    BasisPoints queue_pressure_bp{};
    std::uint32_t active_senders = 0;
    bool authoritative = false;
};

// ---------------------------------------------------------------------------
// Durable governor state. Serialisable, versioned and integrity checked.
// Restoring this state must never restore liveness, telemetry freshness,
// leases, epochs or authority: everything live is re-established explicitly.
// ---------------------------------------------------------------------------

struct GovernorState {
    StateGeneration generation{};
    DestinationId destination{};
    DestinationGeneration destination_generation{};
    BootIncarnation boot{};
    EpochId epoch{};
    PolicyId policy{};
    PolicyGeneration policy_generation{};

    Timestamp last_evaluation{};
    std::uint64_t evaluation_count = 0;
    std::uint64_t refusal_count = 0;
    std::uint64_t intervention_count = 0;

    std::uint32_t incident_streak = 0;
    std::uint32_t clean_streak = 0;

    RecoveryTracker recovery{};
    std::vector<WindowRecord> history{};
    std::vector<Intervention> interventions{};
    // Accepted evidence sequences are only comparable inside the incarnation
    // that produced them. A restart mints a new boot incarnation whose sequence
    // space starts again, so the recorded sequences are scoped to their source.
    std::array<std::uint64_t, kEvidenceClassCount> last_sequence{};
    BootIncarnation last_sequence_boot{};

    InterventionGeneration next_intervention_generation{1};

    // A restored state is never live: it must pass revalidation before any
    // intervention can become authoritative again.
    bool requires_revalidation = true;
    bool restored_from_durable = false;
    std::uint32_t restore_count = 0;

    [[nodiscard]] std::vector<const Intervention*> live_interventions() const;
    [[nodiscard]] Status validate() const;
};

// ---------------------------------------------------------------------------
// Decision outcome kinds.
// ---------------------------------------------------------------------------

enum class DecisionOutcome : std::uint8_t {
    Refused = 0,
    NoAction = 1,
    InterventionIssued = 2,
    InterventionContinued = 3,
    InterventionRelaxed = 4,
    InterventionRetired = 5,
    InterventionFenced = 6,
};

[[nodiscard]] constexpr std::string_view to_string(DecisionOutcome outcome) noexcept {
    switch (outcome) {
        case DecisionOutcome::Refused: return "Refused";
        case DecisionOutcome::NoAction: return "NoAction";
        case DecisionOutcome::InterventionIssued: return "InterventionIssued";
        case DecisionOutcome::InterventionContinued: return "InterventionContinued";
        case DecisionOutcome::InterventionRelaxed: return "InterventionRelaxed";
        case DecisionOutcome::InterventionRetired: return "InterventionRetired";
        case DecisionOutcome::InterventionFenced: return "InterventionFenced";
    }
    return "Unknown";
}

struct Decision {
    DecisionOutcome outcome = DecisionOutcome::Refused;

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

    Timestamp decided_at{};
    StateGeneration state_generation{};

    IncidentKind kind = IncidentKind::Unknown;
    Severity severity = Severity::None;
    RefusalReason refusal = RefusalReason::None;
    std::string refusal_detail{};

    FanInMetrics metrics{};
    SynchronizationEvidence synchronization{};
    std::vector<SenderContribution> contributions{};

    InterventionId intervention{};
    InterventionState intervention_state = InterventionState::Retired;
    MitigationIntent intent{};

    AuthorityVector authority{};
    Explanation explanation{};

    [[nodiscard]] bool authorized() const noexcept { return authority.intervention_authorized; }
};

// ---------------------------------------------------------------------------
// Engine configuration.
// ---------------------------------------------------------------------------

struct GovernorConfig {
    GovernorPolicy policy = make_default_policy();
    DestinationId destination{};
    DestinationGeneration destination_generation{};
    WorkerId worker{};
    LeaseId lease{};
    EpochId epoch{};
    BootIncarnation boot{};
    Timestamp lease_expiry{};

    std::size_t max_history = 64;
    std::size_t max_contributions_in_decision = 64;
    std::size_t max_notes = 32;
};

// ---------------------------------------------------------------------------
// The governor. All public entry points are serialised by an internal mutex.
//
// Lock-reentrancy contract (verified by the deadlock audit):
//   * No public method calls any other public method.
//   * No callback, allocator hook or user code executes while the mutex is
//     held; the decision observer is invoked after the lock is released.
//   * No read-lock is ever upgraded to a write-lock: a single non-recursive
//     mutex is used and never re-acquired.
//   * evaluate() performs no I/O; durability is handled by the caller via the
//     durable store using the returned state, outside the lock.
// ---------------------------------------------------------------------------

class Governor {
public:
    explicit Governor(GovernorConfig config);

    Governor(const Governor&) = delete;
    Governor& operator=(const Governor&) = delete;

    // Replaces the policy document. A generation change invalidates live
    // interventions, which are moved to Revalidating rather than re-derived.
    [[nodiscard]] Status configure_policy(const GovernorPolicy& policy, Timestamp now);

    [[nodiscard]] GovernorPolicy policy() const;
    [[nodiscard]] DestinationId destination() const;

    // Binds the current authority. All subsequent evaluations must present this
    // epoch, boot, worker and lease, or they are refused as fenced.
    [[nodiscard]] Status bind_authority(EpochId epoch,
                                        BootIncarnation boot,
                                        WorkerId worker,
                                        LeaseId lease,
                                        Timestamp lease_expiry);

    [[nodiscard]] Decision evaluate(const EvidenceBundle& bundle);

    [[nodiscard]] GovernorState snapshot_state() const;
    [[nodiscard]] Status restore_state(const GovernorState& state);

    void reset();

    [[nodiscard]] std::vector<Intervention> interventions() const;

    // Explicitly retires a live intervention. Used by shutdown and by tests.
    [[nodiscard]] Status retire(InterventionId id, Timestamp now, std::string reason);

    // Shutdown: stops accepting new interventions and moves every live
    // intervention to Retired. Returns the number of interventions retired.
    [[nodiscard]] Status shutdown(Timestamp now, std::size_t& retired_count);

    // Observer invoked after the internal lock is released.
    void set_decision_observer(std::function<void(const Decision&)> observer);

    [[nodiscard]] std::size_t max_history() const;

private:
    [[nodiscard]] Decision evaluate_locked(const EvidenceBundle& bundle);

    mutable std::mutex mutex_{};
    GovernorConfig config_{};
    GovernorState state_{};
    bool accepting_ = true;
    std::function<void(const Decision&)> observer_{};
};

// Builds the explanation structure for a decision. Pure function.
//
// The full contributor list is passed separately from decision.contributions so
// that protected/collided exceptions outside the decision's reporting slice are
// still explained; the rendered contributor list remains bounded.
[[nodiscard]] Explanation build_explanation(const Decision& decision,
                                            const std::vector<SenderContribution>& all_contributions,
                                            std::size_t max_contributors);

[[nodiscard]] inline Explanation build_explanation(const Decision& decision, std::size_t max_contributors) {
    return build_explanation(decision, decision.contributions, max_contributors);
}

}  // namespace incast

#endif  // INCAST_GOVERNOR_HPP
