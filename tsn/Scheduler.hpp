// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#pragma once

#include "tsn/Frame.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <vector>

namespace tsnhub {

struct GateSlot {
    std::chrono::microseconds duration{1000};
    std::uint8_t openMask{0xff};
};

struct SchedulerConfig {
    std::vector<GateSlot> schedule{{std::chrono::microseconds{1000}, 0xff}};
    std::chrono::microseconds baseDelay{0};
    std::chrono::microseconds jitter{0};
    double lossRate{0.0};
    std::size_t maxQueuePerClass{1024};
    std::uint64_t randomSeed{1};
};

struct SchedulerStats {
    std::uint64_t accepted{};
    std::uint64_t released{};
    std::uint64_t droppedByLoss{};
    std::uint64_t droppedByQueue{};
};

class Scheduler {
public:
    using Clock = std::chrono::steady_clock;

    explicit Scheduler(SchedulerConfig config = {});
    bool enqueue(Frame frame, Clock::time_point now = Clock::now());
    std::vector<Frame> releaseReady(Clock::time_point now = Clock::now());
    SchedulerStats stats() const;
    const SchedulerConfig &config() const noexcept { return config_; }

private:
    struct ScheduledFrame {
        Frame frame;
        Clock::time_point eligibleAt;
    };

    std::uint8_t gateMask(Clock::time_point now) const;
    std::chrono::microseconds sampledJitter();

    SchedulerConfig config_;
    Clock::time_point cycleStart_;
    std::array<std::queue<ScheduledFrame>, 8> queues_;
    mutable std::mutex mutex_;
    SchedulerStats stats_;
    std::mt19937_64 random_;
    std::uniform_real_distribution<double> probability_{0.0, 1.0};
};

} // namespace tsnhub
