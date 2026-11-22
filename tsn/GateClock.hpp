// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace tsnhub {

struct GateSlot;

// GCL 相位信息, 由 Scheduler 按当前时刻计算得出。
//
// positionUs 为自 cycleStart 起算的周期内偏移(0 <= positionUs < cycleUs)；
// slotIndex 为当前时隙在 schedule 中的下标, cycleIndex 为已经过去的完整周期数。
// 两者的组合 (cycleIndex, slotIndex) 唯一确定一次"开放窗口", 信用机制据此复位。
// nextChange 为该窗口的结束边界(即下一个时隙的开始)的绝对时刻。
struct GatePhase {
    std::chrono::steady_clock::time_point nextChange{};
    std::chrono::microseconds dead{0};
    std::uint8_t mask{0};
    bool closed{true};
    std::size_t slotIndex{0};
    std::int64_t cycleIndex{0};
    std::chrono::microseconds positionUs{0};
    std::chrono::microseconds cycleUs{0};
};

// 按 GCL 周期与当前时刻计算相位。schedule 为空或含非正 duration 时返回
// closed=true 的空相位, 由 Scheduler 构造期校验保证不会走到该分支。
GatePhase gatePhaseAt(std::chrono::steady_clock::time_point now,
                      std::chrono::steady_clock::time_point cycleStart,
                      const GateSlot *schedule, std::size_t scheduleLength);

// 跨平台的绝对时刻睡眠: 睡到 deadline 为止, 提前返回只由信号/中断引起。
// 与 sleep_for(相对时间) 不同, 绝对时刻不会因调度延迟而累积相位误差,
// 因此门控窗口边界不会随时间漂移。
//
// Linux/macOS 走 clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME);
// Windows 走高精度 waitable timer(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)。
// 返回实际醒来的时刻, 便于调用方统计唤醒相位误差。
std::chrono::steady_clock::time_point sleepUntilMonotonic(
    std::chrono::steady_clock::time_point deadline);

// 当前时刻到 deadline 的剩余时间; deadline 已过时返回零。
std::chrono::microseconds remainingUntil(std::chrono::steady_clock::time_point deadline,
                                         std::chrono::steady_clock::time_point now);

} // namespace tsnhub
