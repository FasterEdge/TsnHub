// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "TestAssertions.hpp"
#include "tsn/Scheduler.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;
using tsnhub::Frame;
using tsnhub::GateSlot;
using tsnhub::Scheduler;
using tsnhub::SchedulerConfig;

static Frame frame(std::uint64_t sequence, std::uint8_t priority) {
    return Frame{sequence, priority, "test", {1, 2, 3}, {}};
}

// 构造期校验: 空 GCL、越界丢包率、零队列容量、非正时隙都必须被拒绝。
static void constructorRejectsInvalidConfig() {
    auto expectInvalid = [](SchedulerConfig config, const char *what) {
        bool thrown = false;
        try {
            Scheduler scheduler(config);
            (void)scheduler;
        } catch(const std::invalid_argument &) {
            thrown = true;
        }
        TSNHUB_TEST_REQUIRE_MESSAGE(thrown, what);
    };

    SchedulerConfig emptySchedule;
    emptySchedule.schedule.clear();
    expectInvalid(emptySchedule, "empty schedule must throw");

    SchedulerConfig badLoss;
    badLoss.lossRate = 1.5;
    expectInvalid(badLoss, "lossRate > 1 must throw");

    SchedulerConfig zeroQueue;
    zeroQueue.maxQueuePerClass = 0;
    expectInvalid(zeroQueue, "zero queue capacity must throw");

    SchedulerConfig zeroSlot;
    zeroSlot.schedule = {{0us, 0xff}};
    expectInvalid(zeroSlot, "non-positive gate slot must throw");
}

// Gate: 关闭窗口内的队列即使已到期也不能释放, 窗口打开后才能放行。
static void gateControlsRelease() {
    SchedulerConfig config;
    config.schedule = {{1000us, 0x01}, {1000us, 0x02}};
    config.baseDelay = 0us;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();

    TSNHUB_TEST_REQUIRE(scheduler.enqueue(frame(11, 1), start));
    TSNHUB_TEST_REQUIRE_MESSAGE(scheduler.releaseReady(start + 400us).empty(),
                                "priority 1 must stay gated in slot mask 0x01");

    auto released = scheduler.releaseReady(start + 1300us);
    TSNHUB_TEST_REQUIRE_MESSAGE(released.size() == 1,
                                "priority 1 must be released once mask 0x02 opens");
    TSNHUB_TEST_REQUIRE(released.front().sequence == 11);
    TSNHUB_TEST_REQUIRE(scheduler.stats().released == 1);
}

// 同一开放窗口内按优先级从高到低出队, 而不是按入队顺序。
static void releasesHigherPriorityFirst() {
    SchedulerConfig config;
    config.schedule = {{1000us, 0xff}};
    config.baseDelay = 0us;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();

    TSNHUB_TEST_REQUIRE(scheduler.enqueue(frame(21, 0), start));
    TSNHUB_TEST_REQUIRE(scheduler.enqueue(frame(22, 3), start));
    TSNHUB_TEST_REQUIRE(scheduler.enqueue(frame(23, 7), start));

    auto released = scheduler.releaseReady(start + 10us);
    TSNHUB_TEST_REQUIRE(released.size() == 3);
    TSNHUB_TEST_REQUIRE(released[0].sequence == 23);
    TSNHUB_TEST_REQUIRE(released[1].sequence == 22);
    TSNHUB_TEST_REQUIRE(released[2].sequence == 21);
}

// 抖动必须落在 [baseDelay - jitter, baseDelay + jitter] 内, 用可观测的释放时刻验证。
static void jitterStaysInsideConfiguredRange() {
    SchedulerConfig config;
    config.schedule = {{1000us, 0xff}};
    config.baseDelay = 1000us;
    config.jitter = 50us;
    config.maxQueuePerClass = 64;
    config.randomSeed = 42;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();

    for(std::uint64_t sequence = 0; sequence < 20; ++sequence)
        TSNHUB_TEST_REQUIRE(scheduler.enqueue(frame(sequence, 0), start));

    TSNHUB_TEST_REQUIRE_MESSAGE(scheduler.releaseReady(start + 949us).empty(),
                                "jitter must never shorten delay below baseDelay - jitter");
    TSNHUB_TEST_REQUIRE_MESSAGE(scheduler.releaseReady(start + 1050us).size() == 20,
                                "all frames must be eligible at baseDelay + jitter");
}

// 固定 seed 时, 丢包决策在两条独立实例上必须完全一致(README 的可复现性承诺)。
static void lossIsReproducibleWithFixedSeed() {
    SchedulerConfig config;
    config.lossRate = 0.5;
    config.maxQueuePerClass = 512;
    config.randomSeed = 12345;

    Scheduler first(config);
    Scheduler second(config);
    const auto start = Scheduler::Clock::now();
    for(std::uint64_t sequence = 0; sequence < 200; ++sequence) {
        first.enqueue(frame(sequence, 2), start);
        second.enqueue(frame(sequence, 2), start);
    }
    const auto a = first.stats();
    const auto b = second.stats();
    TSNHUB_TEST_REQUIRE(a.droppedByLoss == b.droppedByLoss);
    TSNHUB_TEST_REQUIRE(a.accepted == b.accepted);
    TSNHUB_TEST_REQUIRE(a.droppedByLoss > 0);
    TSNHUB_TEST_REQUIRE(a.droppedByLoss < 200);
}

int main() {
    SchedulerConfig config;
    config.schedule = {{1000us, 0x01}, {1000us, 0x80}};
    config.baseDelay = 200us;
    config.maxQueuePerClass = 2;
    config.randomSeed = 42;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();

    TSNHUB_TEST_REQUIRE(scheduler.enqueue(frame(1, 0), start));
    TSNHUB_TEST_REQUIRE(scheduler.enqueue(frame(2, 7), start));
    TSNHUB_TEST_REQUIRE(scheduler.releaseReady(start + 100us).empty());

    auto low = scheduler.releaseReady(start + 300us);
    TSNHUB_TEST_REQUIRE(low.size() == 1 && low.front().sequence == 1);

    auto high = scheduler.releaseReady(start + 1200us);
    TSNHUB_TEST_REQUIRE(high.size() == 1 && high.front().sequence == 2);
    TSNHUB_TEST_REQUIRE(scheduler.stats().accepted == 2);
    TSNHUB_TEST_REQUIRE(scheduler.stats().released == 2);

    SchedulerConfig bounded;
    bounded.maxQueuePerClass = 1;
    Scheduler small(bounded);
    TSNHUB_TEST_REQUIRE(small.enqueue(frame(3, 3), start));
    TSNHUB_TEST_REQUIRE(!small.enqueue(frame(4, 3), start));
    TSNHUB_TEST_REQUIRE(small.stats().droppedByQueue == 1);

    SchedulerConfig lossy;
    lossy.lossRate = 1.0;
    Scheduler dropAll(lossy);
    TSNHUB_TEST_REQUIRE(!dropAll.enqueue(frame(5, 1), start));
    TSNHUB_TEST_REQUIRE(dropAll.stats().droppedByLoss == 1);

    constructorRejectsInvalidConfig();
    gateControlsRelease();
    releasesHigherPriorityFirst();
    jitterStaysInsideConfiguredRange();
    lossIsReproducibleWithFixedSeed();

    TSNHUB_TEST_REPORT("Scheduler tests passed");
    return 0;
}
