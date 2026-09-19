// Incast Governor - core primitive tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <limits>
#include <string>
#include <vector>

#include "incast/codec.hpp"
#include "incast/core.hpp"
#include "incast/ids.hpp"
#include "incast/model.hpp"
#include "incast/policy.hpp"
#include "test_framework.hpp"

using namespace incast;

IG_TEST(core, crc32c_known_vector) {
    // The standard CRC-32C check value for the ASCII string "123456789".
    IG_CHECK_EQ(crc32c(std::string_view("123456789")), 0xE3069283u);
    IG_CHECK_EQ(crc32c(std::string_view("")), 0u);
}

IG_TEST(core, checked_arithmetic_boundaries) {
    IG_CHECK(!checked_add(std::numeric_limits<std::int64_t>::max(), std::int64_t{1}).has_value());
    IG_CHECK(checked_add(std::numeric_limits<std::int64_t>::max(), std::int64_t{0}).has_value());
    IG_CHECK(!checked_add(std::numeric_limits<std::uint64_t>::max(), std::uint64_t{1}).has_value());
    IG_CHECK(checked_mul(std::numeric_limits<std::uint64_t>::max(), 2u).has_value() == false);
    IG_CHECK_EQ(checked_mul(std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()).value(), 0u);
    IG_CHECK(checked_div(1, 0).has_value() == false);
    IG_CHECK(checked_mul(std::numeric_limits<std::int64_t>::min(), -1).has_value() == false);
    IG_CHECK_EQ(saturating_add(std::numeric_limits<std::uint64_t>::max(), 5u),
                std::numeric_limits<std::uint64_t>::max());
    IG_CHECK_EQ(saturating_mul(std::numeric_limits<std::uint64_t>::max(), 3u),
                std::numeric_limits<std::uint64_t>::max());
    IG_CHECK_EQ(narrow<std::uint32_t>(std::uint64_t{7}).value(), 7u);
    IG_CHECK(narrow<std::uint32_t>(std::uint64_t{1} << 40).has_value() == false);
    IG_CHECK(narrow<std::uint32_t>(std::int64_t{-1}).has_value() == false);
}

IG_TEST(core, strong_identities_are_distinct_types) {
    const DestinationId destination{7};
    const SenderId sender{7};
    IG_CHECK(destination.valid());
    IG_CHECK(sender.valid());
    IG_CHECK(destination != DestinationId{8});
    IG_CHECK(DestinationId{}.absent());
    IG_CHECK(DestinationId{} == DestinationId{});

    const EventGeneration first{1};
    const EventGeneration second{2};
    IG_CHECK(second.newer_than(first));
    IG_CHECK(!first.newer_than(second));

    const EpochId epoch{3};
    IG_CHECK_EQ(epoch.next().value(), 4u);
}

IG_TEST(core, basis_points_clamp_and_ratio) {
    IG_CHECK_EQ(BasisPoints{1234}.value(), 1234u);
    IG_CHECK_EQ(BasisPoints{BasisPoints::kMax + 10u}.value(), BasisPoints::kMax);
    IG_CHECK(BasisPoints{BasisPoints::kMax + 10u}.clamped());
    IG_CHECK_EQ(ratio_bp(1, 0).value(), 0u);
    IG_CHECK_EQ(ratio_bp(1, 2).value(), 5000u);
    IG_CHECK_EQ(ratio_bp(std::numeric_limits<std::uint64_t>::max(), 1).value(), BasisPoints::kMax);
}

IG_TEST(core, identifier_parsing_rejects_malformed_text) {
    IG_CHECK_EQ(parse_u64("42").value(), 42u);
    IG_CHECK(!parse_u64("").has_value());
    IG_CHECK(!parse_u64("-1").has_value());
    IG_CHECK(!parse_u64("4x").has_value());
    IG_CHECK(!parse_u64("999999999999999999999999").has_value());
    IG_CHECK_EQ(parse_i64("-17").value(), -17);
    IG_CHECK(valid_identifier_text("queue-7.a"));
    IG_CHECK(!valid_identifier_text("bad name"));
    IG_CHECK(!valid_identifier_text(""));
    IG_CHECK(!valid_identifier_text(std::string(kMaxIdentifierChars + 1, 'a')));
}

IG_TEST(core, service_class_strength_ordering) {
    IG_CHECK(stronger_than(ServiceClass::Replication, ServiceClass::BestEffort));
    IG_CHECK(stronger_than(ServiceClass::Control, ServiceClass::Interactive));
    IG_CHECK(!stronger_than(ServiceClass::BestEffort, ServiceClass::Bulk));
    IG_CHECK(stronger_of(ServiceClass::Bulk, ServiceClass::Control) == ServiceClass::Control);
    IG_CHECK(parse_service_class("LatencySensitive").has_value());
    IG_CHECK(!parse_service_class("latency").has_value());
}

IG_TEST(policy, default_policy_is_valid_and_stable) {
    const GovernorPolicy policy = make_default_policy();
    IG_REQUIRE(static_cast<bool>(policy.validate()));
    const std::uint64_t digest = policy.digest();
    IG_CHECK_EQ(policy.digest(), digest);

    GovernorPolicy changed = policy;
    changed.generation = PolicyGeneration{2};
    IG_CHECK(changed.digest() != digest);
}

IG_TEST(policy, contradictory_policies_are_refused) {
    GovernorPolicy policy = make_default_policy();

    GovernorPolicy bad_sync = policy;
    bad_sync.synchronization.min_synchronized_senders = 1;
    IG_CHECK(!bad_sync.validate());

    GovernorPolicy bad_band = policy;
    bad_band.recovery.enabled = true;
    bad_band.recovery.relax_oversubscription_bp = 30'000;
    IG_CHECK(!bad_band.validate());

    GovernorPolicy bad_bounds = policy;
    bad_bounds.bounds.min_reduction_bp = 6'000;
    bad_bounds.bounds.max_reduction_bp = 5'000;
    IG_CHECK(!bad_bounds.validate());

    GovernorPolicy bad_duration = policy;
    bad_duration.bounds.max_duration = Duration{1'000};
    IG_CHECK(!bad_duration.validate());

    GovernorPolicy bad_name = policy;
    bad_name.name.clear();
    IG_CHECK(!bad_name.validate());

    GovernorPolicy bad_authority = policy;
    bad_authority.authority.require_provenance = false;
    IG_CHECK(!bad_authority.validate());

    GovernorPolicy bad_penalized = policy;
    bad_penalized.bounds.max_penalized_senders = kMaxPenalizedSenders + 1;
    IG_CHECK(!bad_penalized.validate());
}

IG_TEST(codec, round_trip_and_truncation) {
    codec::Writer writer;
    writer.u8(0x7F);
    writer.u16(0xBEEF);
    writer.u32(0xDEADBEEF);
    writer.u64(0x0123456789ABCDEFull);
    writer.i64(-123456789);
    writer.boolean(true);
    writer.str("queue-7");
    const auto buffer = writer.buffer();

    codec::Reader reader(buffer);
    IG_CHECK_EQ(reader.u8().value(), 0x7Fu);
    IG_CHECK_EQ(reader.u16().value(), 0xBEEFu);
    IG_CHECK_EQ(reader.u32().value(), 0xDEADBEEFu);
    IG_CHECK_EQ(reader.u64().value(), 0x0123456789ABCDEFull);
    IG_CHECK_EQ(reader.i64().value(), -123456789);
    IG_CHECK(reader.boolean().value());
    IG_CHECK_EQ(reader.str().value(), std::string("queue-7"));
    IG_CHECK(reader.exhausted());

    codec::Reader truncated(std::span<const std::byte>(buffer.data(), 2));
    IG_CHECK_EQ(truncated.u8().value(), 0x7Fu);
    IG_CHECK(!truncated.u16().has_value());

    codec::Writer big;
    big.str(std::string(kMaxIdentifierChars * 8 + 64, 'x'));
    codec::Reader big_reader(big.buffer());
    auto text = big_reader.str();
    IG_REQUIRE(text.has_value());
    IG_CHECK_EQ(text.value().size(), kMaxIdentifierChars * 8);
}
