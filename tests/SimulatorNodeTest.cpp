// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "TestAssertions.hpp"
#include "transport/PortablePubSub.hpp"
#include "tsn/SimulatorNode.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// 用独立端口对跑一次端到端: 入口发一帧, 断言出口收到同样的帧。
void forwardsOneFrame(std::uint16_t inputPort, std::uint16_t outputPort, std::uint64_t sequence) {
    const tsnhub::UdpEndpoint input{"127.0.0.1", inputPort};
    const tsnhub::UdpEndpoint output{"127.0.0.1", outputPort};
    tsnhub::SchedulerConfig config;
    config.schedule = {{10ms, 0xff}};
    config.baseDelay = 5ms;
    config.randomSeed = 7;

    tsnhub::SimulatorNode node(config, input, output);
    tsnhub::UdpSocket receiver;
    receiver.bind(output);
    tsnhub::UdpSocket sender;
    std::atomic_bool running{true};
    std::thread worker([&] { node.run(running, 1ms); });

    sender.sendTo(input, tsnhub::PortablePubSub::encode(tsnhub::Frame{sequence, 3, "guard", {}, {}}));
    const auto packet = receiver.receive(65507, 500ms);
    running.store(false);
    worker.join();

    TSNHUB_TEST_REQUIRE(packet);
    const auto decoded = tsnhub::PortablePubSub::decode(*packet);
    TSNHUB_TEST_REQUIRE(decoded);
    TSNHUB_TEST_REQUIRE(decoded->sequence == sequence);
    TSNHUB_TEST_REQUIRE(node.stats().accepted == 1);
    TSNHUB_TEST_REQUIRE(node.stats().released == 1);
}

// 畸形入口帧必须被解码器丢弃, 不进入调度器也不出现在出口。
void dropsMalformedIngress(std::uint16_t inputPort, std::uint16_t outputPort) {
    const tsnhub::UdpEndpoint input{"127.0.0.1", inputPort};
    const tsnhub::UdpEndpoint output{"127.0.0.1", outputPort};
    tsnhub::SchedulerConfig config;
    config.schedule = {{10ms, 0xff}};
    config.baseDelay = 0ms;

    tsnhub::SimulatorNode node(config, input, output);
    tsnhub::UdpSocket receiver;
    receiver.bind(output);
    tsnhub::UdpSocket sender;
    std::atomic_bool running{true};
    std::thread worker([&] { node.run(running, 1ms); });

    sender.sendTo(input, std::vector<std::uint8_t>{0xde, 0xad, 0xbe, 0xef, 0x00});
    TSNHUB_TEST_REQUIRE_MESSAGE(!receiver.receive(65507, 200ms),
                                "malformed ingress frames must not be forwarded");
    running.store(false);
    worker.join();
    TSNHUB_TEST_REQUIRE(node.stats().accepted == 0);
    TSNHUB_TEST_REQUIRE(node.stats().released == 0);
}

} // namespace

using namespace std::chrono_literals;

int main() {
    const tsnhub::UdpEndpoint input{"127.0.0.1", 45851};
    const tsnhub::UdpEndpoint output{"127.0.0.1", 45852};
    tsnhub::SchedulerConfig config;
    config.schedule = {{10ms, 0xff}};
    config.baseDelay = 5ms;
    config.randomSeed = 7;

    tsnhub::SimulatorNode node(config, input, output);
    tsnhub::UdpSocket receiver;
    receiver.bind(output);
    tsnhub::UdpSocket sender;
    std::atomic_bool running{true};
    std::thread worker([&] { node.run(running, 1ms); });

    tsnhub::Frame first{1, 1, "sensor-a", {0x01, 0x02}, {}};
    tsnhub::Frame second{2, 7, "sensor-b", {0x03, 0x04}, {}};
    const auto sentAt = std::chrono::steady_clock::now();
    sender.sendTo(input, tsnhub::PortablePubSub::encode(first));
    sender.sendTo(input, tsnhub::PortablePubSub::encode(second));

    auto packetA = receiver.receive(65507, 500ms);
    auto packetB = receiver.receive(65507, 500ms);
    const auto receivedAt = std::chrono::steady_clock::now();
    running.store(false);
    worker.join();

    TSNHUB_TEST_REQUIRE(packetA);
    TSNHUB_TEST_REQUIRE(packetB);
    auto decodedA = tsnhub::PortablePubSub::decode(*packetA);
    auto decodedB = tsnhub::PortablePubSub::decode(*packetB);
    TSNHUB_TEST_REQUIRE(decodedA);
    TSNHUB_TEST_REQUIRE(decodedB);
    // UDP receive and scheduler polling may admit frames in separate iterations;
    // verify complete forwarding without assuming they become eligible together.
    TSNHUB_TEST_REQUIRE((decodedA->sequence == 1 && decodedB->sequence == 2) ||
                        (decodedA->sequence == 2 && decodedB->sequence == 1));
    TSNHUB_TEST_REQUIRE(receivedAt - sentAt >= 5ms);
    TSNHUB_TEST_REQUIRE_MESSAGE(receivedAt - sentAt < 500ms,
                                "5ms base delay must not stall end-to-end forwarding");
    const auto stats = node.stats();
    TSNHUB_TEST_REQUIRE(stats.accepted == 2);
    TSNHUB_TEST_REQUIRE(stats.released == 2);

    dropsMalformedIngress(45853, 45854);
    forwardsOneFrame(45855, 45856, 99);

    TSNHUB_TEST_REPORT("Simulator node tests passed");
    return 0;
}
