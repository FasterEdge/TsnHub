// ─────────────────────────────────────────────────────────────
// FasterEdge 开源项目
// Github: https://github.com/FasterEdge
// Gitee:  https://gitee.com/FasterEdge
// ─────────────────────────────────────────────────────────────
#include "tsn/SimulatorNode.hpp"

#include "tsn/GateClock.hpp"

#include <algorithm>

namespace tsnhub {
SimulatorNode::SimulatorNode(SchedulerConfig scheduler, UdpEndpoint input, UdpEndpoint output)
    : scheduler_(std::move(scheduler)), endpoint_(std::move(input), std::move(output)) {}
void SimulatorNode::run(std::atomic_bool &running, std::chrono::milliseconds poll) {
    // poll 不再是驱动门控的节拍, 只用于限制"本轮收包最多阻塞多久", 并且永远
    // 不超过到下一个 GCL 边界的剩余时间; 门控相位本身始终来自时钟。
    const auto pollLimit = poll.count() > 0 ? poll : std::chrono::milliseconds{1};
    while(running.load(std::memory_order_relaxed)) {
        const auto now = Scheduler::Clock::now();
        const auto phase = scheduler_.gatePhase(now);
        const auto wait = std::min(remainingUntil(phase.nextChange, now),
                                  std::chrono::duration_cast<std::chrono::microseconds>(pollLimit));

        bool progressed = false;
        if(auto frame = endpoint_.receive(std::chrono::duration_cast<std::chrono::milliseconds>(wait))) {
            scheduler_.enqueue(std::move(*frame), Scheduler::Clock::now());
            progressed = true;
        }
        // 出队用重新读取的时刻: 收包耗时可能已把进程推进到新窗口, 继续按旧相位
        // 出队就会把帧放进已经关闭的时隙。
        for(auto &ready : scheduler_.releaseReady()) {
            endpoint_.publish(ready);
            progressed = true;
        }

        // 有进展就立刻再跑一轮(同一窗口内继续按信用放行、继续排空 socket 缓冲),
        // 只有确实无事可做时才睡到窗口边界的绝对时刻。这样既不会把
        // "每轮只处理一帧"变成隐式限速, 也不会退化成固定 1ms 轮询。
        if(progressed)
            continue;
        const auto deadline = scheduler_.gatePhase().nextChange;
        if(Scheduler::Clock::now() < deadline)
            sleepUntilMonotonic(deadline);
    }
    for(auto &ready : scheduler_.releaseReady()) endpoint_.publish(ready);
}
} // namespace tsnhub
