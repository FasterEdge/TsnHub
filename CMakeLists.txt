cmake_minimum_required(VERSION 3.23)

project(TsnHub VERSION 1.0.20261116 LANGUAGES CXX)

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

# 严格编译检查与测试目标共享的告警集; -Werror 由调用方通过 CMAKE_CXX_FLAGS 注入,
# 这里只追加告警选项本身, 避免 README 里的 "严格编译检查" 一节与实际情况脱节。
list(APPEND TSNHUB_WARNING_FLAGS -Wall -Wextra -Wpedantic)

# 测试断言不依赖 <cassert>, 本就不受 NDEBUG 影响; 这里仍显式移除 NDEBUG,
# 让 Release 构建(-O3 -DNDEBUG)下的 ctest 也能真实验证, 避免假绿灯。
list(APPEND TSNHUB_TEST_COMPILE_OPTIONS
    "-UNDEBUG"
)

# open62541 / CLI11 只能从 Linux/Docker 侧获得(本机无 conan 时也会缺失),
# 因此这里按"可用则启用, 缺失则跳过并说明"处理: 核心 tsnhub_core 与四个单元测试
# 不依赖这两个包, 始终可构建; 只有 CLI 与原生 PubSub 模式会因此被跳过。
find_package(CLI11 QUIET)
if(CLI11_FOUND)
    message(STATUS "CLI11 found: ${CLI11_VERSION}")
else()
    message(STATUS "CLI11 not found: TsnHub CLI target skipped "
                   "(core library and unit tests are unaffected)")
endif()

find_package(open62541 QUIET)
if(open62541_FOUND)
    message(STATUS "open62541 found: ${open62541_VERSION}")
else()
    message(STATUS "open62541 not found: native PubSub targets skipped "
                   "(portable mode remains available)")
endif()

if(TSNHUB_USE_OPEN62541_PUBSUB AND NOT open62541_FOUND)
    message(FATAL_ERROR
        "TSNHUB_USE_OPEN62541_PUBSUB=ON requires open62541; "
        "configure with -DTSNHUB_USE_OPEN62541_PUBSUB=OFF for the portable build")
endif()

add_library(tsnhub_core
    tsn/GateClock.cpp
    tsn/LatencyStats.cpp
    tsn/PriorityMap.cpp
    tsn/Scheduler.cpp
    tsn/SimulatorNode.cpp
    tsn/StatsReport.cpp
    transport/UdpSocket.cpp
    transport/PortablePubSub.cpp
)
target_include_directories(tsnhub_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(tsnhub_core PUBLIC cxx_std_17)
target_compile_options(tsnhub_core PRIVATE ${TSNHUB_WARNING_FLAGS})

# CLI 需要 CLI11, 原生 PubSub 需要 open62541, 两者都可从 Docker 构建中获得。
if(CLI11_FOUND AND open62541_FOUND)
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
else()
    message(STATUS "TsnHub executable skipped (missing dependencies listed above); "
                   "run 'ctest' for the portable unit tests")
endif()

if(TSNHUB_BUILD_TESTS)
    enable_testing()
    add_executable(tsnhub_scheduler_test tests/SchedulerTest.cpp)
    target_link_libraries(tsnhub_scheduler_test PRIVATE tsnhub_core)
    target_compile_options(tsnhub_scheduler_test PRIVATE ${TSNHUB_WARNING_FLAGS} ${TSNHUB_TEST_COMPILE_OPTIONS})
    add_test(NAME scheduler COMMAND tsnhub_scheduler_test)
    add_executable(tsnhub_pubsub_test tests/PortablePubSubTest.cpp)
    target_link_libraries(tsnhub_pubsub_test PRIVATE tsnhub_core)
    target_compile_options(tsnhub_pubsub_test PRIVATE ${TSNHUB_WARNING_FLAGS} ${TSNHUB_TEST_COMPILE_OPTIONS})
    add_test(NAME portable_pubsub COMMAND tsnhub_pubsub_test)
    add_executable(tsnhub_udp_test tests/UdpLoopbackTest.cpp)
    target_link_libraries(tsnhub_udp_test PRIVATE tsnhub_core)
    target_compile_options(tsnhub_udp_test PRIVATE ${TSNHUB_WARNING_FLAGS} ${TSNHUB_TEST_COMPILE_OPTIONS})
    add_test(NAME udp_loopback COMMAND tsnhub_udp_test)
    add_executable(tsnhub_node_test tests/SimulatorNodeTest.cpp)
    target_link_libraries(tsnhub_node_test PRIVATE tsnhub_core)
    target_compile_options(tsnhub_node_test PRIVATE ${TSNHUB_WARNING_FLAGS} ${TSNHUB_TEST_COMPILE_OPTIONS})
    add_test(NAME simulator_node COMMAND tsnhub_node_test)
    # 优先级映射层不依赖 open62541, 因此原生 PubSub 关闭时也能验证
    # PublisherId/WriterGroupId/DataSetWriterId -> priority 的解析与匹配规则。
    add_executable(tsnhub_priority_map_test tests/PriorityMapTest.cpp)
    target_link_libraries(tsnhub_priority_map_test PRIVATE tsnhub_core)
    target_compile_options(tsnhub_priority_map_test PRIVATE ${TSNHUB_WARNING_FLAGS} ${TSNHUB_TEST_COMPILE_OPTIONS})
    add_test(NAME priority_map COMMAND tsnhub_priority_map_test)
    # 时延/抖动/乱序统计与令牌桶整形不依赖 open62541, 与 CLI 同理始终可构建,
    # 因此即使本机缺依赖也能用确定性时间源验证统计器正确性。
    add_executable(tsnhub_latency_stats_test tests/LatencyStatsTest.cpp)
    target_link_libraries(tsnhub_latency_stats_test PRIVATE tsnhub_core)
    target_compile_options(tsnhub_latency_stats_test PRIVATE ${TSNHUB_WARNING_FLAGS} ${TSNHUB_TEST_COMPILE_OPTIONS})
    add_test(NAME latency_stats COMMAND tsnhub_latency_stats_test)
    add_executable(tsnhub_scheduler_latency_test tests/SchedulerLatencyTest.cpp)
    target_link_libraries(tsnhub_scheduler_latency_test PRIVATE tsnhub_core)
    target_compile_options(tsnhub_scheduler_latency_test PRIVATE ${TSNHUB_WARNING_FLAGS} ${TSNHUB_TEST_COMPILE_OPTIONS})
    add_test(NAME scheduler_latency COMMAND tsnhub_scheduler_latency_test)
    # 门控相位与绝对时刻唤醒: 不依赖 open62541, 也不依赖 UDP 回环,
    # 因此在任何构建模式下都能验证时隙边界与唤醒精度。
    add_executable(tsnhub_gate_clock_test tests/GateClockTest.cpp)
    target_link_libraries(tsnhub_gate_clock_test PRIVATE tsnhub_core)
    target_compile_options(tsnhub_gate_clock_test PRIVATE ${TSNHUB_WARNING_FLAGS} ${TSNHUB_TEST_COMPILE_OPTIONS})
    add_test(NAME gate_clock COMMAND tsnhub_gate_clock_test)
    # 信用机制(paced 出队): 同一 GCL 窗口内下发帧数不得超过上限,
    # 跨窗口后配额复位, 关闭时隙不消耗配额。
    add_executable(tsnhub_scheduler_pacing_test tests/SchedulerPacingTest.cpp)
    target_link_libraries(tsnhub_scheduler_pacing_test PRIVATE tsnhub_core)
    target_compile_options(tsnhub_scheduler_pacing_test PRIVATE ${TSNHUB_WARNING_FLAGS} ${TSNHUB_TEST_COMPILE_OPTIONS})
    add_test(NAME scheduler_pacing COMMAND tsnhub_scheduler_pacing_test)
    # 端到端 pacing: 真实 UDP 回环上验证"任一 GCL 窗口内下发帧数不超过信用上限"。
    add_executable(tsnhub_node_pacing_test tests/SimulatorNodePacingTest.cpp)
    target_link_libraries(tsnhub_node_pacing_test PRIVATE tsnhub_core)
    target_compile_options(tsnhub_node_pacing_test PRIVATE ${TSNHUB_WARNING_FLAGS} ${TSNHUB_TEST_COMPILE_OPTIONS})
    add_test(NAME simulator_node_pacing COMMAND tsnhub_node_pacing_test)
endif()

if(WIN32)
    target_link_libraries(tsnhub_core PUBLIC ws2_32)
endif()
