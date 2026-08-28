cmake_minimum_required(VERSION 3.23)

project(TsnHub VERSION 1.0.20260826 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TSNHUB_BUILD_TESTS "Build TsnHub tests" ON)
option(TSNHUB_USE_OPEN62541_PUBSUB "Use native open62541 PubSub when available" OFF)
option(TSNHUB_ALLOW_NATIVE_PUBSUB_NON_LINUX "Allow native PubSub API validation outside Linux" OFF)
if(TSNHUB_USE_OPEN62541_PUBSUB AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   NOT TSNHUB_ALLOW_NATIVE_PUBSUB_NON_LINUX)
    message(FATAL_ERROR "Native open62541 PubSub mode is Linux-only")
endif()

find_package(CLI11 REQUIRED)
find_package(open62541 REQUIRED)

add_library(tsnhub_core
    tsn/Scheduler.cpp
    tsn/SimulatorNode.cpp
    transport/UdpSocket.cpp
    transport/PortablePubSub.cpp
)
target_include_directories(tsnhub_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(tsnhub_core PUBLIC cxx_std_17)

add_executable(TsnHub main.cpp)
target_compile_definitions(TsnHub PRIVATE TSNHUB_VERSION="${PROJECT_VERSION}")
target_link_libraries(TsnHub PRIVATE tsnhub_core CLI11::CLI11 open62541::open62541)
if(TSNHUB_USE_OPEN62541_PUBSUB)
    target_sources(TsnHub PRIVATE pubsub/Open62541Bridge.cpp)
    target_compile_definitions(TsnHub PRIVATE TSNHUB_USE_OPEN62541_PUBSUB=1)
    add_executable(tsnhub_pubsub_fixture docker/PubSubFixture.cpp)
    target_link_libraries(tsnhub_pubsub_fixture PRIVATE CLI11::CLI11 open62541::open62541)
    if(TSNHUB_BUILD_TESTS)
        add_test(NAME native_pubsub_integration
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_pubsub_integration.sh
                    ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(native_pubsub_integration PROPERTIES TIMEOUT 30)
    endif()
endif()

if(TSNHUB_BUILD_TESTS)
    enable_testing()
    add_executable(tsnhub_scheduler_test tests/SchedulerTest.cpp)
    target_link_libraries(tsnhub_scheduler_test PRIVATE tsnhub_core)
    add_test(NAME scheduler COMMAND tsnhub_scheduler_test)
    add_executable(tsnhub_pubsub_test tests/PortablePubSubTest.cpp)
    target_link_libraries(tsnhub_pubsub_test PRIVATE tsnhub_core)
    add_test(NAME portable_pubsub COMMAND tsnhub_pubsub_test)
    add_executable(tsnhub_udp_test tests/UdpLoopbackTest.cpp)
    target_link_libraries(tsnhub_udp_test PRIVATE tsnhub_core)
    add_test(NAME udp_loopback COMMAND tsnhub_udp_test)
    add_executable(tsnhub_node_test tests/SimulatorNodeTest.cpp)
    target_link_libraries(tsnhub_node_test PRIVATE tsnhub_core)
    add_test(NAME simulator_node COMMAND tsnhub_node_test)
endif()

if(WIN32)
    target_link_libraries(tsnhub_core PUBLIC ws2_32)
endif()
