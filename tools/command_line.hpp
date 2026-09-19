// Incast Governor tools - shared command line parsing helpers.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_TOOLS_COMMAND_LINE_HPP
#define INCAST_TOOLS_COMMAND_LINE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "incast/core.hpp"
#include "incast/ids.hpp"

namespace incast::tools {

// Split command line flags of the form --name value and --flag.
struct Arguments {
    std::vector<std::string> tokens{};

    [[nodiscard]] bool has(std::string_view name) const {
        for (const auto& token : tokens) {
            if (token == name) return true;
        }
        return false;
    }

    [[nodiscard]] std::optional<std::string> value(std::string_view name) const {
        for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
            if (tokens[index] == name) return tokens[index + 1];
        }
        return std::nullopt;
    }

    [[nodiscard]] std::uint64_t u64(std::string_view name, std::uint64_t fallback) const {
        const auto raw = value(name);
        if (!raw) return fallback;
        const auto parsed = parse_u64(*raw);
        return parsed ? *parsed : fallback;
    }

    [[nodiscard]] std::int64_t i64(std::string_view name, std::int64_t fallback) const {
        const auto raw = value(name);
        if (!raw) return fallback;
        const auto parsed = parse_i64(*raw);
        return parsed ? *parsed : fallback;
    }
};

// Locates the sub-command token: the first token that is neither a flag nor the
// value of one of the supplied flags. Flags may therefore appear in any order.
[[nodiscard]] inline std::string find_command(const Arguments& arguments,
                                              const std::vector<std::string_view>& value_flags) {
    for (std::size_t index = 0; index < arguments.tokens.size(); ++index) {
        const std::string& token = arguments.tokens[index];
        bool is_value_flag = false;
        for (const auto flag : value_flags) {
            if (token == flag) {
                is_value_flag = true;
                break;
            }
        }
        if (is_value_flag) {
            index += 1;
            continue;
        }
        if (token.rfind("--", 0) == 0) continue;
        return token;
    }
    return {};
}

[[nodiscard]] inline Arguments parse_arguments(int argc, char** argv, int first_index = 1) {
    Arguments arguments{};
    for (int index = first_index; index < argc; ++index) {
        arguments.tokens.emplace_back(argv[index]);
    }
    return arguments;
}

}  // namespace incast::tools

#endif  // INCAST_TOOLS_COMMAND_LINE_HPP
