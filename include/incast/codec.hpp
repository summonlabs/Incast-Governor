// Incast Governor - bounded, bounds-checked binary codec used by the durable
// store and the framed transport. Little-endian, length-prefixed, no padding.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_CODEC_HPP
#define INCAST_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "incast/core.hpp"

namespace incast::codec {

using Bytes = std::byte;

class Writer {
public:
    void u8(std::uint8_t value) { buffer_.push_back(static_cast<Bytes>(value)); }
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i64(std::int64_t value);
    void boolean(bool value) { u8(value ? 1u : 0u); }
    void raw(const void* data, std::size_t size);
    void str(std::string_view text);

    [[nodiscard]] const std::vector<Bytes>& buffer() const noexcept { return buffer_; }
    [[nodiscard]] std::vector<Bytes> take() { return std::move(buffer_); }
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
    void reserve(std::size_t bytes) { buffer_.reserve(bytes); }

private:
    std::vector<Bytes> buffer_{};
};

class Reader {
public:
    explicit Reader(std::span<const Bytes> data) noexcept : data_(data) {}

    [[nodiscard]] Outcome<std::uint8_t> u8();
    [[nodiscard]] Outcome<std::uint16_t> u16();
    [[nodiscard]] Outcome<std::uint32_t> u32();
    [[nodiscard]] Outcome<std::uint64_t> u64();
    [[nodiscard]] Outcome<std::int64_t> i64();
    [[nodiscard]] Outcome<bool> boolean();
    // The default bound matches the writer's cap so that a value written by
    // this codec can always be read back by it.
    [[nodiscard]] Outcome<std::string> str(std::size_t max_chars = kMaxIdentifierChars * 8);
    [[nodiscard]] Status raw(void* destination, std::size_t size);

    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - position_; }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] bool exhausted() const noexcept { return position_ == data_.size(); }

private:
    [[nodiscard]] Status require(std::size_t size) const;

    std::span<const Bytes> data_{};
    std::size_t position_ = 0;
};

}  // namespace incast::codec

#endif  // INCAST_CODEC_HPP
