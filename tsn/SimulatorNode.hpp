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
    std::array<PrioritySummary, LatencyStats::priorityCount> latencySummaries() const {
        return scheduler_.latencySummaries();
    }
    // 当前 GCL 相位。测试用它核对唤醒时刻与窗口边界的关系, 运行中也能
    // 用来判断下一次唤醒落在哪个时隙。
    GatePhase gatePhase() const { return scheduler_.gatePhase(Scheduler::Clock::now()); }
    const SchedulerConfig &config() const noexcept { return scheduler_.config(); }
private:
    Scheduler scheduler_;
    UdpPubSubEndpoint endpoint_;
};

} // namespace tsnhub
