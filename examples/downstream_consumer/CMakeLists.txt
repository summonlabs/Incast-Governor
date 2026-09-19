cmake_minimum_required(VERSION 3.20)
project(IncastGovernorDownstreamConsumer LANGUAGES CXX)

# Consumes an installed IncastGovernor package exactly as an unrelated project
# would: find_package, link the exported target, build and run.
find_package(IncastGovernor 1.0.0 REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE IncastGovernor::incast_governor)
target_compile_features(downstream_consumer PRIVATE cxx_std_20)
