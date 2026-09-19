// Incast Governor - versioned, integrity-checked serialisation of governor state.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_STATE_HPP
#define INCAST_STATE_HPP

#include <cstdint>
#include <span>
#include <vector>

#include "incast/codec.hpp"
#include "incast/core.hpp"
#include "incast/governor.hpp"

namespace incast {

// Binary state encoding. The container is:
//   magic        "IGST"         4 bytes
//   version      u16            state format version
//   generation   u64            state generation at encode time
//   length       u32            payload length
//   crc32c       u32            CRC of the payload
//   payload      bytes
inline constexpr std::uint32_t kStateMagic = 0x54534749u;  // 'IGST' little-endian
inline constexpr std::size_t kStateHeaderBytes = 4 + 2 + 8 + 4 + 4;

[[nodiscard]] std::vector<std::byte> encode_state(const GovernorState& state);
[[nodiscard]] Outcome<GovernorState> decode_state(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_intervention(const Intervention& intervention);
[[nodiscard]] Outcome<Intervention> decode_intervention(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_intent(const MitigationIntent& intent);
[[nodiscard]] Outcome<MitigationIntent> decode_intent(std::span<const std::byte> bytes);

}  // namespace incast

#endif  // INCAST_STATE_HPP
