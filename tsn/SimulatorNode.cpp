#include "tsn/SimulatorNode.hpp"

namespace tsnhub {
SimulatorNode::SimulatorNode(SchedulerConfig scheduler, UdpEndpoint input, UdpEndpoint output)
    : scheduler_(std::move(scheduler)), endpoint_(std::move(input), std::move(output)) {}
void SimulatorNode::run(std::atomic_bool &running, std::chrono::milliseconds poll) {
    while(running.load(std::memory_order_relaxed)) {
        const auto now = Scheduler::Clock::now();
        if(auto frame = endpoint_.receive(poll)) scheduler_.enqueue(std::move(*frame), now);
        for(auto &ready : scheduler_.releaseReady(Scheduler::Clock::now())) endpoint_.publish(ready);
    }
    for(auto &ready : scheduler_.releaseReady(Scheduler::Clock::now())) endpoint_.publish(ready);
}
} // namespace tsnhub
