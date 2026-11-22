// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "tsn/GateClock.hpp"

#include "tsn/Scheduler.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <time.h>
#endif

namespace tsnhub {

GatePhase gatePhaseAt(std::chrono::steady_clock::time_point now,
                      std::chrono::steady_clock::time_point cycleStart,
                      const GateSlot *schedule, std::size_t scheduleLength) {
    GatePhase phase;
    if(schedule == nullptr || scheduleLength == 0)
        return phase;

    auto cycle = std::chrono::microseconds{0};
    for(std::size_t index = 0; index < scheduleLength; ++index)
        cycle += schedule[index].duration;
    phase.cycleUs = cycle;

    // 归一化到 [0, cycle): steady_clock 的纪元在不同进程间不一致, 但进程内
    // 单调递增, 取模即得到确定性的周期相位。
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - cycleStart);
    const auto cycleUs = cycle.count();
    auto position = elapsed.count() % cycleUs;
    auto cycles = elapsed.count() / cycleUs;
    if(position < 0) {
        position += cycleUs;
        --cycles;
    }
    phase.positionUs = std::chrono::microseconds{position};
    phase.cycleIndex = static_cast<std::int64_t>(cycles);

    long long cursor = 0;
    std::size_t active = scheduleLength - 1;
    for(std::size_t index = 0; index < scheduleLength; ++index) {
        cursor += schedule[index].duration.count();
        if(position < cursor) {
            active = index;
            break;
        }
    }

    // 边界用"周期起点 + 累计时长"直接算出, 而不是在 now 上做增量累加,
    // 避免每次唤醒的少量误差被累积进下一个窗口。
    const auto slotEnd = cycleStart + std::chrono::microseconds{cycles * cycleUs + cursor};
    phase.slotIndex = active;
    phase.nextChange = slotEnd;
    phase.dead = std::chrono::duration_cast<std::chrono::microseconds>(slotEnd - now);
    if(phase.dead.count() < 0)
        phase.dead = std::chrono::microseconds{0};
    phase.mask = schedule[active].openMask;
    phase.closed = schedule[active].openMask == 0;
    return phase;
}

std::chrono::steady_clock::time_point sleepUntilMonotonic(
    std::chrono::steady_clock::time_point deadline) {
#ifdef _WIN32
    // Waitable timer 以 FILETIME(100ns, 1601 纪元)为基准, 与 steady_clock 不同源;
    // 直接换算绝对时刻会引入纪元差。这里用负的 due 表示"相对等待",
    // 间隔取剩余时间, 由内核负责精确唤醒。
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if(remaining.count() <= 0)
        return std::chrono::steady_clock::now();

    HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr,
                                            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);
    if(timer == nullptr) {
        // 系统不支持高精度定时器时退回标准定时器, 保证调用方仍能前进。
        timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    if(timer == nullptr)
        return std::chrono::steady_clock::now();

    LARGE_INTEGER due;
    // 负值 = 相对时间, 单位 100ns。向上取整, 保证不会早于 deadline 醒来。
    const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
    due.QuadPart = -static_cast<LONGLONG>((ticks + 99) / 100);
    if(::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != 0)
        ::WaitForSingleObject(timer, INFINITE);
    ::CloseHandle(timer);
    return std::chrono::steady_clock::now();
#else
    struct timespec deadlineSpec {};
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
        deadline.time_since_epoch()).count();
    deadlineSpec.tv_sec = static_cast<time_t>(nanoseconds / 1000000000LL);
    deadlineSpec.tv_nsec = static_cast<long>(nanoseconds % 1000000000LL);
    // TIMER_ABSTIME + CLOCK_MONOTONIC: 唤醒时刻由绝对时钟决定,
    // 单次调度延迟不会让后续窗口整体后移。
    while(::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadlineSpec, nullptr) == EINTR) {
    }
    return std::chrono::steady_clock::now();
#endif
}

std::chrono::microseconds remainingUntil(std::chrono::steady_clock::time_point deadline,
                                         std::chrono::steady_clock::time_point now) {
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    return remaining.count() < 0 ? std::chrono::microseconds{0} : remaining;
}

} // namespace tsnhub
