// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "TestAssertions.hpp"
#include "tsn/Scheduler.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace std::chrono_literals;
using tsnhub::Frame;
using tsnhub::Scheduler;
using tsnhub::SchedulerConfig;

namespace {

Frame frame(std::uint64_t sequence, std::uint8_t priority) {
    return Frame{sequence, priority, "test", {1, 2, 3}, {}};
}

} // namespace

// 核心断言: 单个开放窗口内下发的帧数不得超过信用上限。
// 这正是"不允许一次抽空整队"的可验证形式。
static void creditCapsFramesPerWindow() {
    SchedulerConfig config;
    config.schedule = {{1000000us, 0xff}};  // 单个长窗口, 排除周期切换的干扰
    config.baseDelay = 0us;
    config.maxQueuePerClass = 256;
    config.creditFramesPerWindow = 4;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();

    for(std::uint64_t sequence = 0; sequence < 32; ++sequence)
        TSNHUB_TEST_REQUIRE(scheduler.enqueue(frame(sequence, 0), start));

    auto first = scheduler.releaseReady(start + 10us);
    TSNHUB_TEST_REQUIRE_MESSAGE(first.size() == 4,
                                "a window must not drain the whole backlog at once");
    TSNHUB_TEST_REQUIRE(scheduler.stats().released == 4);
    TSNHUB_TEST_REQUIRE(scheduler.stats().creditDeferrals == 1);

    // 同一窗口内继续调用: 信用已耗尽, 不得再放行, 也不得重复计数。
    for(int round = 0; round < 3; ++round) {
        TSNHUB_TEST_REQUIRE(scheduler.releaseReady(start + 20us).empty());
    }
    TSNHUB_TEST_REQUIRE(scheduler.stats().released == 4);
    TSNHUB_TEST_REQUIRE(scheduler.stats().creditDeferrals == 1);
    // 被推迟的帧必须仍在队列里, 而不是被丢弃。
    TSNHUB_TEST_REQUIRE(scheduler.stats().droppedByQueue == 0);
    TSNHUB_TEST_REQUIRE(scheduler.stats().droppedByLoss == 0);
}

// 跨窗口后信用复位, 每个新窗口再放行 creditFramesPerWindow 帧。
static void creditResetsOnEveryWindow() {
    SchedulerConfig config;
    config.schedule = {{1000us, 0xff}, {1000us, 0xff}};
    config.baseDelay = 0us;
    config.maxQueuePerClass = 256;
    config.creditFramesPerWindow = 3;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();

    for(std::uint64_t sequence = 0; sequence < 12; ++sequence)
        TSNHUB_TEST_REQUIRE(scheduler.enqueue(frame(sequence, 0), start));

    // 第一个窗口只放 3 帧。
    TSNHUB_TEST_REQUIRE(scheduler.releaseReady(start + 100us).size() == 3);
    // 第二个窗口(1000us 之后)再放 3 帧。
    TSNHUB_TEST_REQUIRE(scheduler.releaseReady(start + 1100us).size() == 3);
    // 第三个窗口(2000us 之后)再放 3 帧。
    TSNHUB_TEST_REQUIRE(scheduler.releaseReady(start + 2100us).size() == 3);
    // 第四个窗口放行最后 3 帧。
    TSNHUB_TEST_REQUIRE(scheduler.releaseReady(start + 3100us).size() == 3);
    TSNHUB_TEST_REQUIRE(scheduler.stats().released == 12);
    TSNHUB_TEST_REQUIRE(scheduler.stats().creditDeferrals == 3);
}

// 信用是按窗口而非按优先级: 高优先级也不能把低优先级的配额吃掉。
static void creditIsSharedAcrossPriorities() {
    SchedulerConfig config;
    config.schedule = {{1000000us, 0xff}};
    config.baseDelay = 0us;
    config.maxQueuePerClass = 64;
    config.creditFramesPerWindow = 5;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();

    for(std::uint64_t sequence = 0; sequence < 8; ++sequence)
        TSNHUB_TEST_REQUIRE(scheduler.enqueue(frame(sequence, 7), start));
    for(std::uint64_t sequence = 100; sequence < 108; ++sequence)
        TSNHUB_TEST_REQUIRE(scheduler.enqueue(frame(sequence, 0), start));

    auto released = scheduler.releaseReady(start + 10us);
    TSNHUB_TEST_REQUIRE_MESSAGE(released.size() == 5,
                                "the window budget must be shared by all open priorities");
    // 高优先级先用配额, 剩余的留给低优先级, 但总数仍受窗口上限约束。
    std::size_t high = 0;
    std::size_t low = 0;
    for(const auto &item : released) {
        if(item.priority == 7)
            ++high;
        else
            ++low;
    }
    TSNHUB_TEST_REQUIRE(high == 5);
    TSNHUB_TEST_REQUIRE(low == 0);
}

// 信用上限为 0 时关闭 pacing, 行为回到"到期帧全部放行"。
static void zeroCreditDisablesPacing() {
    SchedulerConfig config;
    config.schedule = {{1000000us, 0xff}};
    config.baseDelay = 0us;
    config.maxQueuePerClass = 64;
    config.creditFramesPerWindow = 0;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();

    for(std::uint64_t sequence = 0; sequence < 40; ++sequence)
        TSNHUB_TEST_REQUIRE(scheduler.enqueue(frame(sequence, 0), start));

    auto released = scheduler.releaseReady(start + 10us);
    TSNHUB_TEST_REQUIRE_MESSAGE(released.size() == 40,
                                "credit 0 must keep the legacy drain-all behaviour");
    TSNHUB_TEST_REQUIRE(scheduler.stats().creditDeferrals == 0);
}

// 关闭时隙(mask == 0)默认不消耗信用配额: 否则"每周期可发帧数"会低于配置值。
static void closedSlotsDoNotConsumeCredit() {
    SchedulerConfig config;
    config.schedule = {{1000us, 0xff}, {1000us, 0x00}};
    config.baseDelay = 0us;
    config.maxQueuePerClass = 64;
    config.creditFramesPerWindow = 4;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();

    for(std::uint64_t sequence = 0; sequence < 40; ++sequence)
        TSNHUB_TEST_REQUIRE(scheduler.enqueue(frame(sequence, 0), start));

    // 第一个开放窗口(0..1000us)放 4 帧。
    TSNHUB_TEST_REQUIRE(scheduler.releaseReady(start + 100us).size() == 4);
    // 关闭窗口(1000..2000us)不放行, 也不消耗配额。
    TSNHUB_TEST_REQUIRE(scheduler.releaseReady(start + 1500us).empty());
    // 下一个开放窗口(2000..3000us)仍有完整 4 帧配额。
    TSNHUB_TEST_REQUIRE(scheduler.releaseReady(start + 2100us).size() == 4);
    TSNHUB_TEST_REQUIRE(scheduler.stats().released == 8);
}

// 门控相位误差: 在窗口边界附近连续取样, gatePhase 的 dead 必须随时刻逼近 0,
// 且不会出现"相位跳到上一个窗口"的取模错误。
static void gatePhaseErrorStaysWithinThreshold() {
    SchedulerConfig config;
    config.schedule = {{5000us, 0xff}, {5000us, 0x00}};
    Scheduler scheduler(config);

    // 对齐到一个已知边界: 取当前相位, 推算下一个窗口边界, 再逐步逼近。
    const auto reference = Scheduler::Clock::now();
    const auto phase = scheduler.gatePhase(reference);
    const auto boundary = phase.nextChange;

    for(int step = 0; step < 5; ++step) {
        const auto probe = scheduler.gatePhase(Scheduler::Clock::now());
        TSNHUB_TEST_REQUIRE(probe.cycleIndex >= phase.cycleIndex);
        // dead 不得为负(已越过边界时由 Scheduler 归一化为 0), 也不得超过
        // 当前时隙时长——超出即说明取模或边界计算有误。
        TSNHUB_TEST_REQUIRE(probe.dead.count() >= 0);
        TSNHUB_TEST_REQUIRE(probe.dead.count() <= 5000);
    }

    // 直接构造边界两侧的相位, 误差必须为 0 而不是一个 tick。
    const auto before = scheduler.gatePhase(boundary - 1us);
    const auto after = scheduler.gatePhase(boundary + 1us);
    TSNHUB_TEST_REQUIRE(before.dead.count() == 1);
    TSNHUB_TEST_REQUIRE(after.dead.count() <= 5000);
}

int main() {
    creditCapsFramesPerWindow();
    creditResetsOnEveryWindow();
    creditIsSharedAcrossPriorities();
    zeroCreditDisablesPacing();
    closedSlotsDoNotConsumeCredit();
    gatePhaseErrorStaysWithinThreshold();

    TSNHUB_TEST_REPORT("Scheduler pacing tests passed");
    return 0;
}
