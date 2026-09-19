// Incast Governor - bounded decision explanation and rendering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_EXPLAIN_HPP
#define INCAST_EXPLAIN_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "incast/core.hpp"
#include "incast/detect.hpp"
#include "incast/ids.hpp"
#include "incast/mitigate.hpp"
#include "incast/model.hpp"

namespace incast {

// ---------------------------------------------------------------------------
// A bounded, self-contained explanation of one authoritative decision.
//
// The structure is deliberately flat and size-capped: an explanation is a
// diagnostic artifact, never an unbounded data channel.
// ---------------------------------------------------------------------------

struct Explanation {
    EventId event{};
    EventGeneration event_generation{};
    DestinationId destination{};
    WindowId window{};
    WindowGeneration window_generation{};
    EpochId epoch{};
    BootIncarnation boot{};
    WorkerId worker{};
    LeaseId lease{};
    PolicyId policy{};
    PolicyGeneration policy_generation{};
    Timestamp decided_at{};

    IncidentKind kind = IncidentKind::Unknown;
    Severity severity = Severity::None;
    bool authoritative = false;
    RefusalReason refusal = RefusalReason::None;
    std::string refusal_detail{};

    FanInMetrics metrics{};
    SynchronizationEvidence synchronization{};

    // Affected queue / resource surface.
    QueueId affected_queue{};
    ResourceId affected_resource{};
    Bytes queue_occupancy{};
    Bytes queue_capacity{};
    Bytes buffer_headroom{};

    std::vector<SenderContribution> top_contributors{};
    std::vector<std::string> protected_exceptions{};
    std::vector<std::string> notes{};

    AuthorityVector authority{};
    MitigationIntent intent{};

    std::string headline{};

    [[nodiscard]] std::size_t approximate_bytes() const noexcept;
    [[nodiscard]] bool within_budget() const noexcept;

    // Deterministic text rendering. Stable field order, no locale dependence.
    [[nodiscard]] std::string render_text() const;
    [[nodiscard]] std::string render_json() const;
};

// Escapes a string for the JSON renderer, replacing control characters and
// truncating to the identifier bound.
[[nodiscard]] std::string json_escape(std::string_view text);

}  // namespace incast

#endif  // INCAST_EXPLAIN_HPP
