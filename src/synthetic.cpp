// Incast Governor - synthetic evidence construction.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "incast/synthetic.hpp"

#include <algorithm>

namespace incast::synthetic {
namespace {

[[nodiscard]] EvidenceStamp make_stamp(EvidenceClass klass,
                                       EvidenceState state,
                                       const FanInSpec& spec,
                                       std::uint64_t sequence,
                                       Timestamp observed_at,
                                       Timestamp valid_until) {
    EvidenceStamp stamp{};
    stamp.state = state;
    stamp.klass = klass;
    stamp.window = spec.window;
    stamp.window_generation = spec.window_generation;
    stamp.provenance = state == EvidenceState::Present ? spec.provenance : ProvenanceId{};
    stamp.provenance_generation = state == EvidenceState::Present ? spec.provenance_generation
                                                                  : ProvenanceGeneration{};
    stamp.observed_at = observed_at;
    stamp.valid_until = valid_until;
    stamp.sequence = sequence;
    return stamp;
}

}  // namespace

EvidenceBundle build(const FanInSpec& spec) {
    EvidenceBundle bundle{};
    bundle.event = spec.event;
    bundle.event_generation = spec.event_generation;
    bundle.destination = spec.destination;
    bundle.destination_generation = spec.destination_generation;
    bundle.window = spec.window;
    bundle.window_generation = spec.window_generation;
    const auto computed_window_start = checked_add(spec.now.nanos(), -spec.window_width.nanos());
    const auto computed_lease_expiry = checked_add(spec.now.nanos(), spec.lease_horizon.nanos());
    bundle.window_start = Timestamp{computed_window_start.value_or(spec.now.nanos())};
    bundle.window_end = spec.now;
    bundle.now = spec.now;
    bundle.epoch = spec.epoch;
    bundle.boot = spec.boot;
    bundle.worker = spec.worker;
    bundle.lease = spec.lease;
    bundle.lease_expiry = Timestamp{computed_lease_expiry.value_or(spec.now.nanos())};

    const Timestamp observed = spec.evidence_from_future
                                   ? Timestamp{spec.now.nanos() + spec.window_width.nanos()}
                                   : Timestamp{spec.now.nanos() - spec.window_width.nanos() / 2};
    const Timestamp horizon = Timestamp{spec.now.nanos() + spec.window_width.nanos()};

    std::uint64_t sequence = spec.sequence;
    const auto next_sequence = [&sequence, &spec]() {
        const std::uint64_t value = spec.reorder_sequences ? 1 : sequence;
        sequence += 1;
        return value;
    };

    bundle.fan_in_stamp = make_stamp(EvidenceClass::FanIn, spec.fan_in_state, spec, next_sequence(), observed, horizon);
    bundle.timing_stamp = make_stamp(EvidenceClass::ArrivalTiming, spec.timing_state, spec, next_sequence(), observed, horizon);
    bundle.rate_stamp = make_stamp(EvidenceClass::AggregateRate, spec.rate_state, spec, next_sequence(), observed, horizon);
    bundle.capacity_stamp = make_stamp(EvidenceClass::Capacity,
                                       spec.stale_capacity ? EvidenceState::Stale : spec.capacity_state,
                                       spec, next_sequence(), observed, horizon);
    bundle.queue_stamp = make_stamp(EvidenceClass::Queue, spec.queue_state, spec, next_sequence(), observed, horizon);
    bundle.service_stamp = make_stamp(EvidenceClass::Service, spec.service_state, spec, next_sequence(), observed, horizon);
    bundle.provenance_stamp =
        make_stamp(EvidenceClass::Provenance, spec.provenance_state, spec, next_sequence(), observed, horizon);
    bundle.policy_stamp = make_stamp(EvidenceClass::Service, EvidenceState::Present, spec, next_sequence(), observed, horizon);

    bundle.capacity.destination = spec.destination;
    bundle.capacity.resource = spec.resource;
    bundle.capacity.resource_generation = CapacityGeneration{spec.destination_generation.value()};
    bundle.capacity.service_rate = spec.service_rate;
    bundle.capacity.queue_capacity = spec.queue_capacity;
    bundle.capacity.buffer_capacity = spec.buffer_capacity;
    bundle.capacity.admission_limit = spec.senders;

    QueueObservation queue{};
    queue.queue = spec.queue;
    queue.resource = spec.resource;
    queue.capacity = spec.queue_capacity;
    queue.occupancy = Bytes{saturating_mul(spec.queue_capacity.count(), spec.queue_pressure_bp) / 10000ULL};
    queue.high_watermark = queue.occupancy;
    queue.drop_events = spec.drops;
    queue.ecn_marks = spec.ecn_marks;
    queue.observed_at = observed;
    queue.resource_generation = CapacityGeneration{spec.destination_generation.value()};
    if (!spec.omit_queue) bundle.queues.push_back(queue);

    const std::uint32_t total = spec.senders;
    if (total == 0) return bundle;

    DeterministicRng rng(spec.seed);
    const std::int64_t window_start = bundle.window_start.nanos();
    const std::int64_t spread = spec.arrival_spread.nanos();
    const std::int64_t divisor = total > 1 ? static_cast<std::int64_t>(total - 1) : 1;

    const std::uint32_t omitted = std::min(spec.omit_senders, total);
    const std::uint32_t emitted = total - omitted;
    bundle.fan_in_stamp.truncated = spec.mark_population_saturated || omitted > 0;
    bundle.fan_in_stamp.sample_count = emitted;

    bundle.senders.reserve(static_cast<std::size_t>(emitted) + spec.duplicate_senders);
    for (std::uint32_t index = 0; index < emitted; ++index) {
        SenderObservation sender{};
        sender.identity.sender = SenderId{static_cast<std::uint64_t>(index) + 1};
        const std::uint64_t incarnation_base = spec.churn_incarnations ? (rng.next() | 1ULL) : spec.boot.value();
        sender.identity.incarnation = BootIncarnation{incarnation_base == 0 ? 1 : incarnation_base};
        sender.identity.flow = FlowId{static_cast<std::uint64_t>(index) + 1000};

        const bool is_protected = index >= (emitted - std::min(spec.protected_senders, emitted));
        sender.protected_obligation = is_protected && spec.protected_senders > 0;
        sender.service = sender.protected_obligation ? spec.protected_class : spec.sender_class;
        sender.offered_rate = spec.per_sender_rate;
        sender.transferred = Bytes{saturating_mul(spec.per_sender_rate.bytes_per_second(), 1) / 1000};
        sender.outstanding = spec.outstanding_per_sender;
        sender.attempts = 1;

        std::int64_t offset = (spread * static_cast<std::int64_t>(index)) / divisor;
        if (!spec.arrivals_ascending) offset = spread - offset;
        const std::int64_t jitter =
            divisor > 1 ? static_cast<std::int64_t>(rng.bounded(static_cast<std::uint64_t>(std::max<std::int64_t>(1, spread / divisor)))) : 0;
        sender.first_arrival = Timestamp{window_start + offset + jitter};
        sender.last_arrival = sender.first_arrival;
        bundle.senders.push_back(sender);
    }

    // Duplicate rows reuse sender 1 with a fresh incarnation and a contradictory
    // service class, which is exactly the identity collision the governor must
    // refuse to resolve by guessing.
    for (std::uint32_t index = 0; index < spec.duplicate_senders; ++index) {
        SenderObservation sender{};
        sender.identity.sender = SenderId{1};
        sender.identity.incarnation = BootIncarnation{(rng.next() | 1ULL) + 1};
        sender.identity.flow = FlowId{static_cast<std::uint64_t>(9000 + index)};
        sender.service = ServiceClass::Replication;
        sender.protected_obligation = false;
        sender.offered_rate = spec.per_sender_rate;
        sender.outstanding = spec.outstanding_per_sender;
        sender.first_arrival = Timestamp{window_start};
        sender.last_arrival = sender.first_arrival;
        bundle.senders.push_back(sender);
    }
    return bundle;
}

FanInSpec synchronized_burst(std::uint32_t senders) {
    FanInSpec spec{};
    spec.label.assign("synchronized-burst");
    spec.senders = senders;
    spec.per_sender_rate = Rate{1'000'000'000};
    spec.service_rate = Rate{40'000'000'000};
    spec.arrival_spread = Duration{20'000};
    spec.window_width = Duration{1'000'000};
    spec.queue_pressure_bp = 9'200;
    spec.drops = 16;
    return spec;
}

FanInSpec unsynchronized_many_to_one(std::uint32_t senders) {
    FanInSpec spec = synchronized_burst(senders);
    spec.label.assign("unsynchronized-many-to-one");
    spec.arrival_spread = Duration{900'000};   // arrivals fill almost the whole window
    spec.queue_pressure_bp = 4'000;
    spec.drops = 0;
    return spec;
}

FanInSpec capacity_collapse(std::uint32_t senders) {
    FanInSpec spec = synchronized_burst(senders);
    spec.label.assign("capacity-collapse");
    spec.service_rate = Rate{4'000'000'000};
    spec.queue_pressure_bp = 9'900;
    spec.drops = 4'096;
    spec.protected_senders = senders / 8;
    return spec;
}

FanInSpec telemetry_gap(std::uint32_t senders) {
    FanInSpec spec = synchronized_burst(senders);
    spec.label.assign("telemetry-gap");
    spec.rate_state = EvidenceState::Unknown;
    spec.queue_state = EvidenceState::Stale;
    return spec;
}

}  // namespace incast::synthetic
