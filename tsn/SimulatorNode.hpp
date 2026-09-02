// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#pragma once

#include "transport/PortablePubSub.hpp"
#include "tsn/Scheduler.hpp"

#include <atomic>
#include <chrono>

namespace tsnhub {

class SimulatorNode {
public:
    SimulatorNode(SchedulerConfig scheduler, UdpEndpoint input, UdpEndpoint output);
    void run(std::atomic_bool &running, std::chrono::milliseconds poll = std::chrono::milliseconds{1});
    SchedulerStats stats() const { return scheduler_.stats(); }
private:
    Scheduler scheduler_;
    UdpPubSubEndpoint endpoint_;
};

} // namespace tsnhub
