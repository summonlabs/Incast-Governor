// Incast Governor - strongly typed identities, incarnations, generations, epochs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_IDS_HPP
#define INCAST_IDS_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "incast/core.hpp"

namespace incast {

// The zero value of every identity is the reserved "absent" value. No
// authoritative object is ever constructed with an absent identity.
template <class Tag, class Rep = std::uint64_t>
class StrongId {
public:
    using rep_type = Rep;
    using tag_type = Tag;

    constexpr StrongId() noexcept = default;
    explicit constexpr StrongId(Rep value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != Rep{0}; }
    [[nodiscard]] constexpr bool absent() const noexcept { return value_ == Rep{0}; }

    friend constexpr bool operator==(StrongId, StrongId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(StrongId, StrongId) noexcept = default;

private:
    Rep value_{0};
};

template <class Tag, class Rep>
struct StrongIdHash {
    [[nodiscard]] std::size_t operator()(StrongId<Tag, Rep> id) const noexcept {
        return std::hash<Rep>{}(id.value());
    }
};

// Identity tags. Each tag yields a distinct, non-interchangeable type.
struct DestinationTag;
struct SenderTag;
struct FlowTag;
struct ResourceTag;
struct QueueTag;
struct WindowTag;
struct PolicyTag;
struct InterventionTag;
struct EventTag;
struct ProvenanceTag;
struct LeaseTag;
struct WorkerTag;
struct GroupTag;

using DestinationId = StrongId<DestinationTag>;
using SenderId = StrongId<SenderTag>;
using FlowId = StrongId<FlowTag>;
using ResourceId = StrongId<ResourceTag>;
using QueueId = StrongId<QueueTag>;
using WindowId = StrongId<WindowTag>;
using PolicyId = StrongId<PolicyTag>;
using InterventionId = StrongId<InterventionTag>;
using EventId = StrongId<EventTag>;
using ProvenanceId = StrongId<ProvenanceTag>;
using LeaseId = StrongId<LeaseTag>;
using WorkerId = StrongId<WorkerTag>;
using GroupId = StrongId<GroupTag>;

// A generation is a monotonically increasing revision of a specific entity
// family. Using a distinct template parameter keeps a generation from being
// silently interchanged with the entity identifier it belongs to.
template <class Tag>
class Generation {
public:
    using tag_type = Tag;

    constexpr Generation() noexcept = default;
    explicit constexpr Generation(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
    [[nodiscard]] constexpr bool newer_than(Generation other) const noexcept { return value_ > other.value_; }

    friend constexpr bool operator==(Generation, Generation) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(Generation, Generation) noexcept = default;

private:
    std::uint64_t value_{0};
};

using EventGeneration = Generation<EventTag>;
using DestinationGeneration = Generation<DestinationTag>;
using SenderGeneration = Generation<SenderTag>;
using CapacityGeneration = Generation<ResourceTag>;
using WindowGeneration = Generation<WindowTag>;
using PolicyGeneration = Generation<PolicyTag>;
using InterventionGeneration = Generation<InterventionTag>;
using ProvenanceGeneration = Generation<ProvenanceTag>;
using StateGeneration = Generation<GroupTag>;

// Coordinator epoch: advances whenever authority over a destination is
// re-acquired. Every decision binds to the epoch that authorized it.
class EpochId {
public:
    constexpr EpochId() noexcept = default;
    explicit constexpr EpochId(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
    [[nodiscard]] constexpr EpochId next() const noexcept { return EpochId(value_ + 1); }

    friend constexpr bool operator==(EpochId, EpochId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(EpochId, EpochId) noexcept = default;

private:
    std::uint64_t value_{0};
};

// Boot incarnation: a per-OS-process-boot nonce. Durable state never restores
// liveness, telemetry freshness, leases or authority from a previous boot.
class BootIncarnation {
public:
    constexpr BootIncarnation() noexcept = default;
    explicit constexpr BootIncarnation(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    friend constexpr bool operator==(BootIncarnation, BootIncarnation) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(BootIncarnation, BootIncarnation) noexcept = default;

private:
    std::uint64_t value_{0};
};

// Composite sender identity. A bare SenderId is not sufficient authority to
// penalize traffic: the same identifier observed from two live incarnations is
// an identity collision and must never be resolved by guessing.
struct SenderIdentity {
    SenderId sender{};
    BootIncarnation incarnation{};
    FlowId flow{};

    [[nodiscard]] bool valid() const noexcept { return sender.valid() && incarnation.valid(); }

    friend constexpr bool operator==(const SenderIdentity&, const SenderIdentity&) noexcept = default;
};

struct SenderIdentityHash {
    [[nodiscard]] std::size_t operator()(const SenderIdentity& identity) const noexcept {
        std::size_t seed = std::hash<std::uint64_t>{}(identity.sender.value());
        hash_combine(seed, std::hash<std::uint64_t>{}(identity.incarnation.value()));
        hash_combine(seed, std::hash<std::uint64_t>{}(identity.flow.value()));
        return seed;
    }
};

struct SenderKeyHash {
    [[nodiscard]] std::size_t operator()(const SenderIdentity& identity) const noexcept {
        std::size_t seed = std::hash<std::uint64_t>{}(identity.sender.value());
        hash_combine(seed, std::hash<std::uint64_t>{}(identity.incarnation.value()));
        return seed;
    }
};

struct SenderKeyEq {
    [[nodiscard]] bool operator()(const SenderIdentity& a, const SenderIdentity& b) const noexcept {
        return a.sender == b.sender && a.incarnation == b.incarnation;
    }
};

// Identity collision classes. Detected collisions are recorded and the affected
// senders are excluded from any per-sender penalization.
enum class CollisionKind : std::uint8_t {
    None = 0,
    DuplicateSenderDifferentIncarnation = 1,
    ContradictoryAttributes = 2,
    FlowReuseAcrossDestinations = 3,
};

[[nodiscard]] constexpr std::string_view to_string(CollisionKind kind) noexcept {
    switch (kind) {
        case CollisionKind::None: return "None";
        case CollisionKind::DuplicateSenderDifferentIncarnation: return "DuplicateSenderDifferentIncarnation";
        case CollisionKind::ContradictoryAttributes: return "ContradictoryAttributes";
        case CollisionKind::FlowReuseAcrossDestinations: return "FlowReuseAcrossDestinations";
    }
    return "Unknown";
}

struct SenderCollision {
    SenderId sender{};
    CollisionKind kind = CollisionKind::None;
    std::uint32_t occurrences = 0;
};

// Deterministic 64-bit mixing used to mint incarnations, provenance ids and
// scenario seeds. SplitMix64 is used because it is tiny, seedable and stable
// across platforms, which keeps test vectors reproducible.
class DeterministicRng {
public:
    explicit constexpr DeterministicRng(std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] constexpr std::uint64_t next() noexcept {
        state_ += 0x9e3779b97f4a7c15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    [[nodiscard]] constexpr std::uint64_t bounded(std::uint64_t upper_exclusive) noexcept {
        if (upper_exclusive == 0) return 0;
        return next() % upper_exclusive;
    }

    [[nodiscard]] constexpr std::uint64_t state() const noexcept { return state_; }

private:
    std::uint64_t state_;
};

// Identifier parsing for text surfaces (CLI, scenarios, wire payloads). Every
// parse is a checked conversion; malformed text yields nullopt rather than 0.
[[nodiscard]] std::optional<std::uint64_t> parse_u64(std::string_view text) noexcept;
[[nodiscard]] std::optional<std::int64_t> parse_i64(std::string_view text) noexcept;
[[nodiscard]] std::optional<double> parse_double(std::string_view text) noexcept;
[[nodiscard]] bool valid_identifier_text(std::string_view text) noexcept;
[[nodiscard]] std::string format_u64(std::uint64_t value);

}  // namespace incast

#endif  // INCAST_IDS_HPP
