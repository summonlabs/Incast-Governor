# Incast Governor test suite.
#
# No test declares a timeout: a hang is a defect to diagnose, not to hide behind
# a watchdog.

set(INCAST_GOVERNOR_TEST_SOURCES
    test_main.cpp
    unit_core.cpp
    unit_detect.cpp
    unit_mitigate.cpp
    unit_state.cpp
    unit_governor.cpp
    property_governor.cpp
    adversarial.cpp
    integration_transport.cpp)

add_executable(incast_governor_tests ${INCAST_GOVERNOR_TEST_SOURCES})
target_link_libraries(incast_governor_tests
    PRIVATE incast_governor $<BUILD_INTERFACE:incast_governor_warnings>)
target_include_directories(incast_governor_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME incast_governor.tests COMMAND incast_governor_tests)

if(INCAST_GOVERNOR_BUILD_TOOLS)
    add_executable(incast_governor_multiprocess_tests multiprocess.cpp)
    target_link_libraries(incast_governor_multiprocess_tests
        PRIVATE incast_governor $<BUILD_INTERFACE:incast_governor_warnings>)
    target_include_directories(incast_governor_multiprocess_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    add_test(NAME incast_governor.multiprocess
        COMMAND incast_governor_multiprocess_tests
            --coordinator $<TARGET_FILE:incast_governor_coordinator>
            --worker $<TARGET_FILE:incast_governor_worker>
            --ctl $<TARGET_FILE:incast_governor_ctl>)

    add_test(NAME incast_governor.selftest COMMAND incast_governor_cli selftest)
endif()

if(INCAST_GOVERNOR_BUILD_BENCHMARKS)
    add_test(NAME incast_governor.benchmark
        COMMAND incast_governor_bench --events 200 --senders 64 --repeats 1)
endif()
