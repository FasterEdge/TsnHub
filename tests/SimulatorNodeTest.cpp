#include "transport/PortablePubSub.hpp"
#include "tsn/SimulatorNode.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

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

    assert(packetA && packetB);
    auto decodedA = tsnhub::PortablePubSub::decode(*packetA);
    auto decodedB = tsnhub::PortablePubSub::decode(*packetB);
    assert(decodedA && decodedB);
    // Once both frames are eligible, strict priority emits class 7 first.
    assert(decodedA->sequence == 2);
    assert(decodedB->sequence == 1);
    assert(receivedAt - sentAt >= 5ms);
    const auto stats = node.stats();
    assert(stats.accepted == 2);
    assert(stats.released == 2);

    std::cout << "Simulator node tests passed\n";
    return 0;
}
