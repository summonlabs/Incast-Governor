cmake_minimum_required(VERSION 3.20)

project(IncastGovernor
    VERSION 1.0.0
    DESCRIPTION "Generation-bound detection and prevention of synchronized many-to-one traffic collapse"
    HOMEPAGE_URL "https://github.com/summonlabs/Incast-Governor"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(INCAST_GOVERNOR_BUILD_TOOLS "Build the coordinator, worker and CLI tools" ON)
option(INCAST_GOVERNOR_BUILD_TESTS "Build the test suite" ON)
option(INCAST_GOVERNOR_BUILD_BENCHMARKS "Build the synthetic governance benchmark" ON)
option(INCAST_GOVERNOR_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(INCAST_GOVERNOR_ENABLE_ASAN "Build with AddressSanitizer where supported" OFF)
option(INCAST_GOVERNOR_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer where supported" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_VISIBILITY_INLINES_HIDDEN OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Warning configuration
# ---------------------------------------------------------------------------
add_library(incast_governor_warnings INTERFACE)
if(MSVC)
    target_compile_options(incast_governor_warnings INTERFACE
        /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc /MP)
    if(INCAST_GOVERNOR_WARNINGS_AS_ERRORS)
        target_compile_options(incast_governor_warnings INTERFACE /WX)
    endif()
    target_compile_definitions(incast_governor_warnings INTERFACE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(incast_governor_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast)
    if(INCAST_GOVERNOR_WARNINGS_AS_ERRORS)
        target_compile_options(incast_governor_warnings INTERFACE -Werror)
    endif()
endif()

if(INCAST_GOVERNOR_ENABLE_ASAN)
    if(MSVC)
        target_compile_options(incast_governor_warnings INTERFACE /fsanitize=address /Zi)
        target_link_options(incast_governor_warnings INTERFACE /INCREMENTAL:NO /DEBUG)
    else()
        target_compile_options(incast_governor_warnings INTERFACE -fsanitize=address -fno-omit-frame-pointer -g)
        target_link_options(incast_governor_warnings INTERFACE -fsanitize=address)
    endif()
endif()

if(INCAST_GOVERNOR_ENABLE_UBSAN AND NOT MSVC)
    target_compile_options(incast_governor_warnings INTERFACE -fsanitize=undefined -fno-omit-frame-pointer -g)
    target_link_options(incast_governor_warnings INTERFACE -fsanitize=undefined)
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(INCAST_GOVERNOR_SOURCES
    src/codec.cpp
    src/coordinator.cpp
    src/core.cpp
    src/detect.cpp
    src/durable.cpp
    src/explain.cpp
    src/governor.cpp
    src/lifecycle.cpp
    src/mitigate.cpp
    src/model.cpp
    src/policy.cpp
    src/state.cpp
    src/synthetic.cpp
    src/transport.cpp)

set(INCAST_GOVERNOR_PUBLIC_HEADERS
    include/incast/codec.hpp
    include/incast/coordinator.hpp
    include/incast/core.hpp
    include/incast/detect.hpp
    include/incast/durable.hpp
    include/incast/explain.hpp
    include/incast/governor.hpp
    include/incast/ids.hpp
    include/incast/lifecycle.hpp
    include/incast/mitigate.hpp
    include/incast/model.hpp
    include/incast/policy.hpp
    include/incast/state.hpp
    include/incast/synthetic.hpp
    include/incast/transport.hpp)

add_library(incast_governor STATIC ${INCAST_GOVERNOR_SOURCES} ${INCAST_GOVERNOR_PUBLIC_HEADERS})
add_library(IncastGovernor::incast_governor ALIAS incast_governor)

target_include_directories(incast_governor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The warning interface target is a build-tree-only concern and must not leak
# into the installed package export.
target_link_libraries(incast_governor
    PRIVATE $<BUILD_INTERFACE:incast_governor_warnings>
    PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(incast_governor PUBLIC ws2_32)
endif()

set_target_properties(incast_governor PROPERTIES
    OUTPUT_NAME incast_governor
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET default)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(INCAST_GOVERNOR_BUILD_TOOLS)
    add_executable(incast_governor_cli tools/incast_governor_cli.cpp)
    target_link_libraries(incast_governor_cli
        PRIVATE incast_governor $<BUILD_INTERFACE:incast_governor_warnings>)
    set_target_properties(incast_governor_cli PROPERTIES OUTPUT_NAME incast-governor)

    add_executable(incast_governor_coordinator tools/incast_governor_coordinator.cpp)
    target_link_libraries(incast_governor_coordinator
        PRIVATE incast_governor $<BUILD_INTERFACE:incast_governor_warnings>)
    set_target_properties(incast_governor_coordinator PROPERTIES OUTPUT_NAME incast-governor-coordinator)

    add_executable(incast_governor_worker tools/incast_governor_worker.cpp)
    target_link_libraries(incast_governor_worker
        PRIVATE incast_governor $<BUILD_INTERFACE:incast_governor_warnings>)
    set_target_properties(incast_governor_worker PROPERTIES OUTPUT_NAME incast-governor-worker)

    add_executable(incast_governor_ctl tools/incast_governor_ctl.cpp)
    target_link_libraries(incast_governor_ctl
        PRIVATE incast_governor $<BUILD_INTERFACE:incast_governor_warnings>)
    set_target_properties(incast_governor_ctl PROPERTIES OUTPUT_NAME incast-governor-ctl)
endif()

# ---------------------------------------------------------------------------
# Benchmark
# ---------------------------------------------------------------------------
if(INCAST_GOVERNOR_BUILD_BENCHMARKS)
    add_executable(incast_governor_bench bench/governance_benchmark.cpp)
    target_link_libraries(incast_governor_bench
        PRIVATE incast_governor $<BUILD_INTERFACE:incast_governor_warnings>)
    set_target_properties(incast_governor_bench PROPERTIES OUTPUT_NAME incast-governor-bench)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(INCAST_GOVERNOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install and package export
# ---------------------------------------------------------------------------
install(TARGETS incast_governor
    EXPORT IncastGovernorTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(INCAST_GOVERNOR_BUILD_TOOLS)
    install(TARGETS incast_governor_cli incast_governor_coordinator incast_governor_worker incast_governor_ctl
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(INCAST_GOVERNOR_BUILD_BENCHMARKS)
    install(TARGETS incast_governor_bench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/incast DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT IncastGovernorTargets
    FILE IncastGovernorTargets.cmake
    NAMESPACE IncastGovernor::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/IncastGovernor)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/IncastGovernorConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/IncastGovernorConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/IncastGovernor)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/IncastGovernorConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/IncastGovernorConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/IncastGovernorConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/IncastGovernor)

install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/IncastGovernor)
