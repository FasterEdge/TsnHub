// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "TestAssertions.hpp"
#include "tsn/GateClock.hpp"
#include "tsn/Scheduler.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

using namespace std::chrono_literals;
using tsnhub::GatePhase;
using tsnhub::GateSlot;
using tsnhub::Scheduler;
using tsnhub::SchedulerConfig;
using tsnhub::gatePhaseAt;

namespace {

// 固定起点的相位表: [slot0: 1000us mask 0x0f][slot1: 500us mask 0xf0][slot2: 250us mask 0x00]
// 周期 1750us。用于逐点核对 position/slotIndex/cycleIndex/mask。
void phaseWalksThroughEverySlot() {
    const std::vector<GateSlot> schedule{{1000us, 0x0f}, {500us, 0xf0}, {250us, 0x00}};
    const auto start = std::chrono::steady_clock::now();

    struct Expectation {
        std::int64_t atUs;
        std::uint8_t mask;
        std::size_t slot;
        std::int64_t cycle;
        bool closed;
    };
    const Expectation expected[] = {
        {0, 0x0f, 0, 0, false},
        {999, 0x0f, 0, 0, false},
        {1000, 0xf0, 1, 0, false},
        {1499, 0xf0, 1, 0, false},
        {1500, 0x00, 2, 0, true},
        {1749, 0x00, 2, 0, true},
        {1750, 0x0f, 0, 1, false},
        {2749, 0x0f, 0, 1, false},
        {3499, 0x00, 2, 1, true},
        {6250, 0xf0, 1, 3, false},
    };

    for(const auto &want : expected) {
        const auto phase = gatePhaseAt(start + std::chrono::microseconds{want.atUs}, start,
                                       schedule.data(), schedule.size());
        TSNHUB_TEST_REQUIRE_MESSAGE(phase.mask == want.mask,
                                    "gate mask must follow the slot at this offset");
        TSNHUB_TEST_REQUIRE_MESSAGE(phase.slotIndex == want.slot,
                                    "slot index must identify the open window");
        TSNHUB_TEST_REQUIRE_MESSAGE(phase.cycleIndex == want.cycle,
                                    "cycle index must count completed cycles");
        TSNHUB_TEST_REQUIRE_MESSAGE(phase.closed == want.closed,
                                    "closed flag must reflect an empty mask");
        // 周期内偏移必须与输入一致, 否则信用复位会落在错误的窗口上。
        TSNHUB_TEST_REQUIRE(phase.positionUs.count() == want.atUs % 1750);
    }
}

// nextChange 必须始终指向当前时隙的结束边界, 且 remaining 时间随时刻单调收敛。
void phaseBoundaryIsTheSlotEnd() {
    const std::vector<GateSlot> schedule{{2000us, 0xff}, {1000us, 0x01}};
    const auto start = std::chrono::steady_clock::now();

    const auto inside = gatePhaseAt(start + 1500us, start, schedule.data(), schedule.size());
    TSNHUB_TEST_REQUIRE_MESSAGE(inside.nextChange == start + 2000us,
                                "nextChange must be the end of the 2000us slot");
    TSNHUB_TEST_REQUIRE_MESSAGE(inside.dead == 500us, "dead must be the time left in the slot");

    const auto atEdge = gatePhaseAt(start + 2000us, start, schedule.data(), schedule.size());
    TSNHUB_TEST_REQUIRE(atEdge.nextChange == start + 3000us);
    TSNHUB_TEST_REQUIRE(atEdge.dead == 1000us);
    TSNHUB_TEST_REQUIRE(atEdge.slotIndex == 1);
}

// Scheduler 暴露的 gatePhase 必须与配置的 GCL 自洽: 相位给出的 mask 就是
// positionUs 所在时隙的 openMask, 源单一, 不允许两处各自算一套。
void schedulerPhaseMatchesGateClock() {
    SchedulerConfig config;
    config.schedule = {{1000us, 0x01}, {1000us, 0x80}};
    Scheduler scheduler(config);

    for(int round = 0; round < 20; ++round) {
        const auto phase = scheduler.gatePhase();
        const auto position = phase.positionUs.count();
        const std::uint8_t expectedMask = position < 1000 ? 0x01 : 0x80;
        TSNHUB_TEST_REQUIRE_MESSAGE(phase.mask == expectedMask,
                                    "mask must be the openMask of the slot at positionUs");
        TSNHUB_TEST_REQUIRE(phase.cycleUs == 2000us);
        TSNHUB_TEST_REQUIRE(phase.slotIndex == (position < 1000 ? 0u : 1u));
        // 剩余时间不超过当前时隙时长, 驱动器据此决定睡到边界。
        TSNHUB_TEST_REQUIRE(phase.dead.count() > 0);
        TSNHUB_TEST_REQUIRE(phase.dead.count() <= 1000);
        std::this_thread::sleep_for(300us);
    }
}

// 绝对时刻睡眠不得早于 deadline 醒来; 抖动必须在操作系统调度粒度附近,
// 而不是把 1ms 轮询的累计误差暴露出来。
void sleepUntilMonotonicRespectsDeadline() {
    for(int round = 0; round < 5; ++round) {
        const auto deadline = std::chrono::steady_clock::now() + 2ms;
        const auto woke = tsnhub::sleepUntilMonotonic(deadline);
        TSNHUB_TEST_REQUIRE_MESSAGE(woke >= deadline,
                                    "absolute-time sleep must never wake before the deadline");
        // 1500us 的上限留足调度余量, 又足以暴露"睡过头几个 tick"的回归。
        TSNHUB_TEST_REQUIRE_MESSAGE(woke - deadline < 1500us,
                                    "wake-up overshoot must stay within one scheduler tick");
    }
}

// 已过的 deadline 必须立即返回, 不能反向睡眠。
void sleepUntilMonotonicHandlesPastDeadline() {
    const auto deadline = std::chrono::steady_clock::now() - 5ms;
    const auto began = std::chrono::steady_clock::now();
    const auto woke = tsnhub::sleepUntilMonotonic(deadline);
    TSNHUB_TEST_REQUIRE(woke - began < 5ms);
}

} // namespace

int main() {
    phaseWalksThroughEverySlot();
    phaseBoundaryIsTheSlotEnd();
    schedulerPhaseMatchesGateClock();
    sleepUntilMonotonicRespectsDeadline();
    sleepUntilMonotonicHandlesPastDeadline();

    TSNHUB_TEST_REPORT("Gate clock tests passed");
    return 0;
}
