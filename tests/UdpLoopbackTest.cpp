// ─────────────────────────────────────────────────────────────
// FasterEdge 开源项目
// Github: https://github.com/FasterEdge
// Gitee:  https://gitee.com/FasterEdge
// ─────────────────────────────────────────────────────────────
#include "TestAssertions.hpp"
#include "transport/PortablePubSub.hpp"

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main() {
    const tsnhub::UdpEndpoint receiverAddress{"127.0.0.1", 45841};
    tsnhub::UdpSocket receiver;
    receiver.bind(receiverAddress);
    tsnhub::UdpSocket sender;

    tsnhub::Frame source{7, 5, "loopback", {9, 8, 7, 6}, {}};
    sender.sendTo(receiverAddress, tsnhub::PortablePubSub::encode(source));
    auto packet = receiver.receive(65507, 500ms);
    TSNHUB_TEST_REQUIRE(packet);
    auto decoded = tsnhub::PortablePubSub::decode(*packet);
    TSNHUB_TEST_REQUIRE(decoded);
    TSNHUB_TEST_REQUIRE(decoded->sequence == 7);
    TSNHUB_TEST_REQUIRE(decoded->priority == 5);
    TSNHUB_TEST_REQUIRE(decoded->stream == "loopback");
    TSNHUB_TEST_REQUIRE(decoded->payload == source.payload);
    TSNHUB_TEST_REQUIRE(!receiver.receive(1024, 1ms));

    // 超过 UDP 单包上限(65507 字节)的 payload 必须被 sendTo 拒绝, 而不是静默截断。
    tsnhub::Frame oversized{8, 1, "oversize", std::vector<std::uint8_t>(70000, 0x5a), {}};
    const auto oversizedPacket = tsnhub::PortablePubSub::encode(oversized);
    TSNHUB_TEST_REQUIRE(oversizedPacket.size() > 65507);
    bool sendRejected = false;
    try {
        sender.sendTo(receiverAddress, oversizedPacket);
    } catch(const std::runtime_error &) {
        sendRejected = true;
    }
    TSNHUB_TEST_REQUIRE_MESSAGE(sendRejected, "payload larger than the UDP limit must be rejected");

    // 空输入窗口必须返回超时占位, 而不是抛错或返回空包。
    TSNHUB_TEST_REQUIRE(!receiver.receive(1024, 1ms));

    TSNHUB_TEST_REPORT("UDP loopback tests passed");
    return 0;
}
