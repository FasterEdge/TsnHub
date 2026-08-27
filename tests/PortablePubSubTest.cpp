#include "transport/PortablePubSub.hpp"

#include <cassert>
#include <iostream>

int main() {
    tsnhub::Frame source;
    source.sequence = 42;
    source.priority = 6;
    source.stream = "opcua.temperature";
    source.payload = {0x55, 0x41, 0x44, 0x50, 0, 1, 2, 3};
    const auto packet = tsnhub::PortablePubSub::encode(source);
    const auto decoded = tsnhub::PortablePubSub::decode(packet);
    assert(decoded);
    assert(decoded->sequence == source.sequence);
    assert(decoded->priority == source.priority);
    assert(decoded->stream == source.stream);
    assert(decoded->payload == source.payload);
    assert(!tsnhub::PortablePubSub::decode({1, 2, 3}));

    auto malformed = packet;
    malformed[13] = 0xff;
    malformed[14] = 0xff;
    assert(!tsnhub::PortablePubSub::decode(malformed));
    std::cout << "Portable PubSub tests passed\n";
    return 0;
}
