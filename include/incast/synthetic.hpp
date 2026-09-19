// Incast Governor - synthetic evidence construction for tests, benchmarks and
// downstream validation harnesses.
//
// This utility fabricates governance evidence. It never measures a physical
// network and must never be presented as a physical-network result.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_SYNTHETIC_HPP
#define INCAST_SYNTHETIC_HPP

#include <cstdint>
#include <string>

#include "incast/core.hpp"
#include "incast/ids.hpp"
#include "incast/model.hpp"

namespace incast::synthetic {

// A fully explicit description of one synthetic observation window. Every field
// that influences a decision is named here, so a scenario is reproducible from
// its specification alone.
struct FanInSpec {
    // Identity and generation binding.
    EventId event{1};
    EventGeneration event_generation{1};
    DestinationId destination{1};
    DestinationGeneration destination_generation{1};
    WindowId window{1};
    WindowGeneration window_generation{1};
    ResourceId resource{1};
    QueueId queue{1};
    ProvenanceId provenance{1};
    ProvenanceGeneration provenance_generation{1};
    PolicyId policy{1};
    PolicyGeneration policy_generation{1};
    EpochId epoch{1};
    BootIncarnation boot{1};
    WorkerId worker{1};
    LeaseId lease{1};

    // Timing.
    Timestamp now{1'000'000'000'000};
    Duration window_width{1'000'000};   // 1 ms observation window
    Duration arrival_spread{20'000};    // 20 us observed fan-in spread
    Duration lease_horizon{10'000'000'000};
    bool arrivals_ascending = true;
    bool evidence_from_future = false;   // stamp observations after the evaluation instant

    // Sender population.
    std::uint32_t senders = 64;
    Rate per_sender_rate{1'000'000'000};   // 1 GB/s per sender
    std::uint32_t protected_senders = 0;
    ServiceClass sender_class = ServiceClass::BestEffort;
    ServiceClass protected_class = ServiceClass::Replication;
    std::uint32_t duplicate_senders = 0;   // extra rows reusing sender 1 with a new incarnation
    std::uint32_t omit_senders = 0;        // rows removed while the population is marked saturated
    bool mark_population_saturated = false;
    bool churn_incarnations = false;       // every sender gets a distinct incarnation

    // Destination and queue.
    Rate service_rate{40'000'000'000};
    Bytes queue_capacity{4u << 20};
    Bytes buffer_capacity{8u << 20};
    std::uint32_t queue_pressure_bp = 5'000;   // occupancy / capacity
    Bytes outstanding_per_sender{64u << 10};
    std::uint64_t drops = 0;
    std::uint64_t ecn_marks = 0;

    // Evidence health. Every class defaults to authoritative.
    EvidenceState fan_in_state = EvidenceState::Present;
    EvidenceState timing_state = EvidenceState::Present;
    EvidenceState rate_state = EvidenceState::Present;
    EvidenceState capacity_state = EvidenceState::Present;
    EvidenceState queue_state = EvidenceState::Present;
    EvidenceState service_state = EvidenceState::Present;
    EvidenceState provenance_state = EvidenceState::Present;
    bool omit_queue = false;
    bool stale_capacity = false;
    std::uint64_t sequence = 1;
    bool reorder_sequences = false;

    std::uint64_t seed = 0x5EEDC0DEULL;
    std::string label{"synthetic"};
};

// Builds the evidence bundle described by the specification. Deterministic for
// a fixed seed.
[[nodiscard]] EvidenceBundle build(const FanInSpec& spec);

// Convenience constructors for the recurring hardening scenarios.
[[nodiscard]] FanInSpec synchronized_burst(std::uint32_t senders = 128);
[[nodiscard]] FanInSpec unsynchronized_many_to_one(std::uint32_t senders = 128);
[[nodiscard]] FanInSpec capacity_collapse(std::uint32_t senders = 256);
[[nodiscard]] FanInSpec telemetry_gap(std::uint32_t senders = 128);

}  // namespace incast::synthetic

#endif  // INCAST_SYNTHETIC_HPP
