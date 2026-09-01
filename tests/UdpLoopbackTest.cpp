// ─────────────────────────────────────────────────────────────
// FasterEdge 开源项目
// Github: https://github.com/FasterEdge
// Gitee:  https://gitee.com/FasterEdge
// ─────────────────────────────────────────────────────────────
#include "transport/PortablePubSub.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

int main() {
    const tsnhub::UdpEndpoint receiverAddress{"127.0.0.1", 45841};
    tsnhub::UdpSocket receiver;
    receiver.bind(receiverAddress);
    tsnhub::UdpSocket sender;

    tsnhub::Frame source{7, 5, "loopback", {9, 8, 7, 6}, {}};
    sender.sendTo(receiverAddress, tsnhub::PortablePubSub::encode(source));
    auto packet = receiver.receive(65507, 500ms);
    assert(packet);
    auto decoded = tsnhub::PortablePubSub::decode(*packet);
    assert(decoded);
    assert(decoded->sequence == 7);
    assert(decoded->priority == 5);
    assert(decoded->stream == "loopback");
    assert(decoded->payload == source.payload);
    assert(!receiver.receive(1024, 1ms));
    std::cout << "UDP loopback tests passed\n";
    return 0;
}
