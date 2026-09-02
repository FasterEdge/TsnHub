// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "tsn/Scheduler.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;
using tsnhub::Frame;
using tsnhub::GateSlot;
using tsnhub::Scheduler;
using tsnhub::SchedulerConfig;

static Frame frame(std::uint64_t sequence, std::uint8_t priority) {
    return Frame{sequence, priority, "test", {1, 2, 3}, {}};
}

int main() {
    SchedulerConfig config;
    config.schedule = {{1000us, 0x01}, {1000us, 0x80}};
    config.baseDelay = 200us;
    config.maxQueuePerClass = 2;
    config.randomSeed = 42;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();

    assert(scheduler.enqueue(frame(1, 0), start));
    assert(scheduler.enqueue(frame(2, 7), start));
    assert(scheduler.releaseReady(start + 100us).empty());

    auto low = scheduler.releaseReady(start + 300us);
    assert(low.size() == 1 && low.front().sequence == 1);

    auto high = scheduler.releaseReady(start + 1200us);
    assert(high.size() == 1 && high.front().sequence == 2);

    SchedulerConfig bounded;
    bounded.maxQueuePerClass = 1;
    Scheduler small(bounded);
    assert(small.enqueue(frame(3, 3), start));
    assert(!small.enqueue(frame(4, 3), start));
    assert(small.stats().droppedByQueue == 1);

    SchedulerConfig lossy;
    lossy.lossRate = 1.0;
    Scheduler dropAll(lossy);
    assert(!dropAll.enqueue(frame(5, 1), start));
    assert(dropAll.stats().droppedByLoss == 1);

    std::cout << "Scheduler tests passed\n";
    return 0;
}
