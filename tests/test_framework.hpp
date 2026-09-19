// Incast Governor - minimal deterministic test framework.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef INCAST_TEST_FRAMEWORK_HPP
#define INCAST_TEST_FRAMEWORK_HPP

#include <cstdio>
#include <exception>
#include <string>
#include <type_traits>
#include <vector>

namespace igtest {

struct Failure {
    std::string message;
};

struct Case {
    std::string suite;
    std::string name;
    void (*function)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline long& check_count() {
    static long count = 0;
    return count;
}

inline long& failure_count() {
    static long count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* suite, const char* name, void (*function)()) {
        registry().push_back(Case{suite, name, function});
    }
};

inline void report_failure(const char* file, int line, const std::string& text) {
    failure_count() += 1;
    std::fprintf(stderr, "    FAIL %s:%d: %s\n", file, line, text.c_str());
    std::fflush(stderr);
}

inline int run_all(int argc, char** argv) {
    std::string filter;
    for (int index = 1; index < argc; ++index) {
        const std::string token = argv[index];
        const std::string prefix = "--filter=";
        if (token.rfind(prefix, 0) == 0) filter = token.substr(prefix.size());
    }

    int ran = 0;
    int failed_cases = 0;
    for (const auto& test : registry()) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos &&
            test.suite.find(filter) == std::string::npos) {
            continue;
        }
        ran += 1;
        const long before = failure_count();
        std::printf("[ RUN  ] %s.%s\n", test.suite.c_str(), test.name.c_str());
        std::fflush(stdout);
        try {
            test.function();
        } catch (const Failure& failure) {
            report_failure("require", 0, failure.message);
        } catch (const std::exception& error) {
            report_failure("exception", 0, error.what());
        } catch (...) {
            report_failure("exception", 0, "non-standard exception escaped the test case");
        }
        if (failure_count() != before) {
            failed_cases += 1;
            std::printf("[ FAIL ] %s.%s\n", test.suite.c_str(), test.name.c_str());
        } else {
            std::printf("[  OK  ] %s.%s\n", test.suite.c_str(), test.name.c_str());
        }
        std::fflush(stdout);
    }
    std::printf("\n%d test cases, %ld checks, %ld failures\n", ran, check_count(), failure_count());
    std::fflush(stdout);
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace igtest

#define IG_TEST(suite, name)                                                                     \
    static void ig_test_##suite##_##name();                                                      \
    static const ::igtest::Registrar ig_registrar_##suite##_##name(#suite, #name,                \
                                                                   &ig_test_##suite##_##name);   \
    static void ig_test_##suite##_##name()

#define IG_CHECK(condition)                                                                      \
    do {                                                                                         \
        ::igtest::check_count() += 1;                                                            \
        if (!(condition)) {                                                                      \
            ::igtest::report_failure(__FILE__, __LINE__, #condition);                            \
        }                                                                                        \
    } while (false)

#define IG_CHECK_EQ(actual, expected)                                                            \
    do {                                                                                         \
        ::igtest::check_count() += 1;                                                            \
        if (!((actual) == (expected))) {                                                         \
            ::igtest::report_failure(__FILE__, __LINE__,                                         \
                                     std::string(#actual " == " #expected " (actual=") +         \
                                         ::igtest::describe(actual) + ", expected=" +            \
                                         ::igtest::describe(expected) + ")");                    \
        }                                                                                        \
    } while (false)

#define IG_REQUIRE(condition)                                                                    \
    do {                                                                                         \
        ::igtest::check_count() += 1;                                                            \
        if (!(condition)) {                                                                      \
            throw ::igtest::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) +     \
                                    ": requirement failed: " #condition};                        \
        }                                                                                        \
    } while (false)

namespace igtest {

template <class T>
inline std::string describe(const T& value) {
    if constexpr (std::is_convertible_v<T, long long>) {
        return std::to_string(static_cast<long long>(value));
    } else if constexpr (std::is_convertible_v<T, std::string>) {
        return std::string(value);
    } else {
        return "<value>";
    }
}

}  // namespace igtest

#endif  // INCAST_TEST_FRAMEWORK_HPP
