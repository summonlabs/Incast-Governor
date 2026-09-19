// Incast Governor - bounded binary codec implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "incast/codec.hpp"

#include <cstring>

namespace incast::codec {

void Writer::u16(std::uint16_t value) {
    buffer_.push_back(static_cast<Bytes>(value & 0xFFu));
    buffer_.push_back(static_cast<Bytes>((value >> 8) & 0xFFu));
}

void Writer::u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        buffer_.push_back(static_cast<Bytes>((value >> shift) & 0xFFu));
    }
}

void Writer::u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        buffer_.push_back(static_cast<Bytes>((value >> shift) & 0xFFu));
    }
}

void Writer::i64(std::int64_t value) {
    u64(static_cast<std::uint64_t>(value));
}

void Writer::raw(const void* data, std::size_t size) {
    if (size == 0 || data == nullptr) return;
    const auto* bytes = static_cast<const Bytes*>(data);
    buffer_.insert(buffer_.end(), bytes, bytes + size);
}

void Writer::str(std::string_view text) {
    const std::size_t capped = text.size() > kMaxIdentifierChars * 8 ? kMaxIdentifierChars * 8 : text.size();
    u32(static_cast<std::uint32_t>(capped));
    raw(text.data(), capped);
}

Status Reader::require(std::size_t size) const {
    if (size > remaining()) return fail(ErrorCode::TruncatedInput, "codec ran past the end of the buffer");
    return ok_status();
}

Outcome<std::uint8_t> Reader::u8() {
    if (!require(1)) return Error{ErrorCode::TruncatedInput, "u8 beyond end"};
    return static_cast<std::uint8_t>(data_[position_++]);
}

Outcome<std::uint16_t> Reader::u16() {
    if (!require(2)) return Error{ErrorCode::TruncatedInput, "u16 beyond end"};
    std::uint16_t value = 0;
    for (int index = 0; index < 2; ++index) {
        value |= static_cast<std::uint16_t>(static_cast<std::uint8_t>(data_[position_++])) << (index * 8);
    }
    return value;
}

Outcome<std::uint32_t> Reader::u32() {
    if (!require(4)) return Error{ErrorCode::TruncatedInput, "u32 beyond end"};
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data_[position_++])) << (index * 8);
    }
    return value;
}

Outcome<std::uint64_t> Reader::u64() {
    if (!require(8)) return Error{ErrorCode::TruncatedInput, "u64 beyond end"};
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data_[position_++])) << (index * 8);
    }
    return value;
}

Outcome<std::int64_t> Reader::i64() {
    auto value = u64();
    if (!value) return value.error();
    return static_cast<std::int64_t>(value.value());
}

Outcome<bool> Reader::boolean() {
    auto value = u8();
    if (!value) return value.error();
    return value.value() != 0;
}

Outcome<std::string> Reader::str(std::size_t max_chars) {
    auto length = u32();
    if (!length) return length.error();
    const std::size_t declared = length.value();
    if (declared > max_chars) {
        return Error{ErrorCode::OversizedInput, "string exceeds the declared bound"};
    }
    if (!require(declared)) return Error{ErrorCode::TruncatedInput, "string payload truncated"};
    std::string text(reinterpret_cast<const char*>(data_.data() + position_), declared);
    position_ += declared;
    return text;
}

Status Reader::raw(void* destination, std::size_t size) {
    const Status check = require(size);
    if (!check) return check;
    if (size != 0) std::memcpy(destination, data_.data() + position_, size);
    position_ += size;
    return ok_status();
}

}  // namespace incast::codec
