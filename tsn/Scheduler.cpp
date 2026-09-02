// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "tsn/Scheduler.hpp"

#include <algorithm>
#include <stdexcept>

namespace tsnhub {

Scheduler::Scheduler(SchedulerConfig config)
    : config_(std::move(config)), cycleStart_(Clock::now()), random_(config_.randomSeed) {
    if(config_.schedule.empty())
        throw std::invalid_argument("TSN schedule cannot be empty");
    if(config_.lossRate < 0.0 || config_.lossRate > 1.0)
        throw std::invalid_argument("loss rate must be in [0,1]");
    if(config_.maxQueuePerClass == 0)
        throw std::invalid_argument("max queue must be positive");
    for(const auto &slot : config_.schedule) {
        if(slot.duration.count() <= 0)
            throw std::invalid_argument("gate slot duration must be positive");
    }
}

std::chrono::microseconds Scheduler::sampledJitter() {
    const auto limit = config_.jitter.count();
    if(limit <= 0)
        return std::chrono::microseconds{0};
    std::uniform_int_distribution<long long> distribution(-limit, limit);
    return std::chrono::microseconds{distribution(random_)};
}

bool Scheduler::enqueue(Frame frame, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame.priority = std::min<std::uint8_t>(frame.priority, 7);
    if(probability_(random_) < config_.lossRate) {
        ++stats_.droppedByLoss;
        return false;
    }
    auto &queue = queues_[frame.priority];
    if(queue.size() >= config_.maxQueuePerClass) {
        ++stats_.droppedByQueue;
        return false;
    }
    frame.receivedAt = now;
    auto delay = config_.baseDelay + sampledJitter();
    if(delay.count() < 0)
        delay = std::chrono::microseconds{0};
    queue.push(ScheduledFrame{std::move(frame), now + delay});
    ++stats_.accepted;
    return true;
}

std::uint8_t Scheduler::gateMask(Clock::time_point now) const {
    auto cycle = std::chrono::microseconds{0};
    for(const auto &slot : config_.schedule)
        cycle += slot.duration;
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - cycleStart_);
    auto position = elapsed.count() % cycle.count();
    if(position < 0)
        position += cycle.count();
    long long cursor = 0;
    for(const auto &slot : config_.schedule) {
        cursor += slot.duration.count();
        if(position < cursor)
            return slot.openMask;
    }
    return config_.schedule.back().openMask;
}

std::vector<Frame> Scheduler::releaseReady(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto mask = gateMask(now);
    std::vector<Frame> released;
    for(int priority = 7; priority >= 0; --priority) {
        if((mask & (1u << priority)) == 0)
            continue;
        auto &queue = queues_[static_cast<std::size_t>(priority)];
        while(!queue.empty() && queue.front().eligibleAt <= now) {
            released.push_back(std::move(queue.front().frame));
            queue.pop();
            ++stats_.released;
        }
    }
    return released;
}

SchedulerStats Scheduler::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

} // namespace tsnhub
