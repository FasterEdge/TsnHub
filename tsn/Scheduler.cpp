// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "tsn/Scheduler.hpp"

#include <algorithm>
#include <cmath>
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
    if(config_.shapingRateMbps < 0.0 || !std::isfinite(config_.shapingRateMbps))
        throw std::invalid_argument("shaping rate must be a non-negative finite number");
    if(config_.latencyRetention == 0)
        throw std::invalid_argument("latency retention must be positive");
    if(config_.shapingRateMbps > 0.0) {
        rateBytesPerSecond_ = config_.shapingRateMbps * 1e6 / 8.0;
        burst_ = config_.shapingBurstBytes > 0
                     ? static_cast<double>(config_.shapingBurstBytes)
                     : 65536.0;
        tokens_ = burst_;
    }
}

std::chrono::microseconds Scheduler::sampledJitter() {
    const auto limit = config_.jitter.count();
    if(limit <= 0)
        return std::chrono::microseconds{0};
    std::uniform_int_distribution<long long> distribution(-limit, limit);
    return std::chrono::microseconds{distribution(random_)};
}

std::size_t Scheduler::wireBytes(const Frame &frame) const {
    return config_.shapingOverheadBytes + frame.stream.size() + frame.payload.size();
}

bool Scheduler::takeTokens(std::size_t bytes, Clock::time_point now) {
    if(!shapingClock_) {
        // 首次出队建立令牌桶时间基准; 起始满桶即允许最多一个突发通过。
        shapingClock_ = now;
        tokens_ = burst_;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(
        now - *shapingClock_);
    shapingClock_ = now;
    if(elapsed.count() < 0.0)
        throw std::logic_error("shaping clock must not move backwards");
    tokens_ = std::min(burst_, tokens_ + elapsed.count() * rateBytesPerSecond_);
    if(tokens_ < static_cast<double>(bytes))
        return false;
    tokens_ -= static_cast<double>(bytes);
    return true;
}

bool Scheduler::enqueue(Frame frame, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame.priority = std::min<std::uint8_t>(frame.priority, 7);
    auto &counters = stats_.perPriority[frame.priority];
    if(probability_(random_) < config_.lossRate) {
        ++stats_.droppedByLoss;
        ++counters.droppedByLoss;
        return false;
    }
    auto &queue = queues_[frame.priority];
    if(queue.size() >= config_.maxQueuePerClass) {
        ++stats_.droppedByQueue;
        ++counters.droppedByQueue;
        return false;
    }
    frame.receivedAt = now;
    auto delay = config_.baseDelay + sampledJitter();
    if(delay.count() < 0)
        delay = std::chrono::microseconds{0};
    queue.push(ScheduledFrame{std::move(frame), now + delay});
    ++stats_.accepted;
    ++counters.accepted;
    return true;
}

std::uint8_t Scheduler::gateMask(Clock::time_point now) const {
    return gatePhase(now).mask;
}

GatePhase Scheduler::gatePhase(Clock::time_point now) const {
    return gatePhaseAt(now, cycleStart_, config_.schedule.data(), config_.schedule.size());
}

std::size_t Scheduler::refreshCredit(const GatePhase &phase) {
    if(!creditEnabled())
        return std::numeric_limits<std::size_t>::max();
    if(!creditActive_ || phase.cycleIndex != creditCycle_ || phase.slotIndex != creditSlot_) {
        // 新窗口: 配额复位。关闭窗口默认不消耗配额(见 creditOnClosedSlots),
        // 否则关闭时隙会白白吃掉配额, 让"每周期可发帧数"低于配置值。
        creditActive_ = true;
        creditCycle_ = phase.cycleIndex;
        creditSlot_ = phase.slotIndex;
        creditLeft_ = (phase.closed && !config_.creditOnClosedSlots)
                          ? 0
                          : config_.creditFramesPerWindow;
        creditDeferralCounted_ = false;
    }
    return creditLeft_;
}

std::vector<Frame> Scheduler::releaseReady(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto phase = gatePhase(now);
    const auto mask = phase.mask;
    // 信用按窗口共享: 一旦本窗口配额用尽, 所有已开放优先级都必须停止,
    // 因此用 exhausted 标志跳出外层循环, 而不是每个优先级各记一次。
    refreshCredit(phase);
    bool exhausted = false;
    std::vector<Frame> released;
    for(int priority = 7; priority >= 0 && !exhausted; --priority) {
        if((mask & (1u << priority)) == 0)
            continue;
        auto &queue = queues_[static_cast<std::size_t>(priority)];
        auto &counters = stats_.perPriority[static_cast<std::size_t>(priority)];
        while(!queue.empty() && queue.front().eligibleAt <= now) {
            if(creditEnabled() && creditLeft_ == 0) {
                // 本窗口配额已用尽: 停止出队并顺延到下一个同类窗口。
                // 每个窗口最多计一次, 语义是"本窗口因配额被截断"的次数。
                if(!creditDeferralCounted_) {
                    ++stats_.creditDeferrals;
                    creditDeferralCounted_ = true;
                }
                exhausted = true;
                break;
            }
            // 令牌桶在端口线速上序列化出队: 令牌不足时高优先级后续帧也停止发送,
            // 这与真实链路上低优先级流量占满带宽时的表现一致。
            if(shapingEnabled() && !takeTokens(wireBytes(queue.front().frame), now)) {
                ++stats_.shapedDeferrals;
                ++counters.shapedDeferrals;
                break;
            }
            if(creditEnabled())
                --creditLeft_;
            const auto lane = static_cast<std::size_t>(priority);
            const auto receivedAt = queue.front().frame.receivedAt;
            const auto sequence = queue.front().frame.sequence;
            const auto stream = queue.front().frame.stream;
            auto frame = std::move(queue.front().frame);
            queue.pop();
            latency_.record(static_cast<std::uint8_t>(lane), now - receivedAt, sequence,
                            stream);
            ++stats_.released;
            ++counters.released;
            released.push_back(std::move(frame));
        }
    }
    return released;
}

SchedulerStats Scheduler::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto out = stats_;
    out.latencySamples = latency_.samples();
    return out;
}

std::array<PrioritySummary, LatencyStats::priorityCount> Scheduler::latencySummaries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latency_.summaries();
}

} // namespace tsnhub
