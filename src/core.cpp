// Incast Governor - core primitives implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "incast/core.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "incast/ids.hpp"
#include "incast/model.hpp"

namespace incast {
namespace {

// Reflected CRC-32C (Castagnoli) table, computed once at first use.
struct Crc32cTable {
    std::array<std::uint32_t, 256> entries{};

    constexpr Crc32cTable() noexcept {
        constexpr std::uint32_t polynomial = 0x82F63B78u;
        std::uint32_t position = 0;
        for (auto& entry : entries) {
            std::uint32_t crc = position;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1u) != 0u ? (crc >> 1) ^ polynomial : (crc >> 1);
            }
            entry = crc;
            position += 1;
        }
    }
};

constexpr Crc32cTable kCrcTable{};

}  // namespace

std::uint32_t crc32c(const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t index = 0; index < size; ++index) {
        crc = kCrcTable.entries[(crc ^ bytes[index]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::optional<std::uint64_t> parse_u64(std::string_view text) noexcept {
    if (text.empty() || text.size() > 20) return std::nullopt;
    std::uint64_t value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value, 10);
    if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
    return value;
}

std::optional<std::int64_t> parse_i64(std::string_view text) noexcept {
    if (text.empty() || text.size() > 20) return std::nullopt;
    std::int64_t value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value, 10);
    if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
    return value;
}

std::optional<double> parse_double(std::string_view text) noexcept {
    if (text.empty() || text.size() > 64) return std::nullopt;
    const std::string buffer(text);
    char* end = nullptr;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end == nullptr || end != buffer.c_str() + buffer.size()) return std::nullopt;
    if (!std::isfinite(value)) return std::nullopt;
    return value;
}

bool valid_identifier_text(std::string_view text) noexcept {
    if (text.empty() || text.size() > kMaxIdentifierChars) return false;
    for (const char character : text) {
        const bool acceptable = (character >= 'a' && character <= 'z') ||
                                (character >= 'A' && character <= 'Z') ||
                                (character >= '0' && character <= '9') || character == '_' ||
                                character == '-' || character == '.' || character == ':';
        if (!acceptable) return false;
    }
    return true;
}

std::string format_u64(std::uint64_t value) {
    std::array<char, 24> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 10);
    return std::string(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
}

}  // namespace incast
