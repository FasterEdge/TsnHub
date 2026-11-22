// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#pragma once

#include "tsn/Frame.hpp"
#include "tsn/GateClock.hpp"
#include "tsn/LatencyStats.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
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
    // 出队整形(令牌桶)。shapingRateMbps 为 0 时不整形，出队只受门控与
    // 固定/随机时延约束；大于 0 时按端口线速把帧序列化成字节流，
    // 令牌不足则本轮停止出队并计入 shapedDeferrals。
    double shapingRateMbps{0.0};
    // 令牌桶深度(字节)。0 表示按 shapingBurstBytes 的默认值(64 KiB)取值。
    std::size_t shapingBurstBytes{0};
    // 单帧在链路上占用的额外字节，默认等于 Portable PubSub Envelope 头长度。
    std::size_t shapingOverheadBytes{15};
    // 时延统计保留的样本数上限(每优先级)。
    std::size_t latencyRetention{LatencyStats::defaultRetention};
    // 每个 GCL 开放窗口允许下发的帧数上限(pacing)。0 表示只由门控决定，
    // 沿用"到期帧全部放行"的旧行为；大于 0 时单个窗口内最多放行这么多帧，
    // 超出部分顺延到下一个同类开放窗口(见 Scheduler::creditLimitFor)。
    // 这正是 TAS 的 paced 语义: 一个时隙不是把积压一次性抽空，而是按
    // 速率上限/信用发送，与令牌桶形成"窗口配额 + 线速"两级约束。
    std::size_t creditFramesPerWindow{0};
    // 关闭窗口(mask == 0)是否也计入信用配额。默认 false: 关闭时隙本来
    // 就不放行任何帧，把配额浪费在没有发送能力的时隙上会让模型失真。
    bool creditOnClosedSlots{false};
};

// 单优先级计数器。offered = accepted + droppedByLoss + droppedByQueue，
// 这四项是绘制丢包率-负载曲线的最小充分统计量。
struct PriorityCounters {
    std::uint64_t accepted{};
    std::uint64_t released{};
    std::uint64_t droppedByLoss{};
    std::uint64_t droppedByQueue{};
    std::uint64_t shapedDeferrals{};
    // 该优先级因本窗口信用耗尽被顺延的次数。
    std::uint64_t creditDeferrals{};
};

struct SchedulerStats {
    std::uint64_t accepted{};
    std::uint64_t released{};
    std::uint64_t droppedByLoss{};
    std::uint64_t droppedByQueue{};
    std::uint64_t shapedDeferrals{};
    // 因本窗口信用耗尽而被顺延到下一个窗口的帧数。它与 shapedDeferrals
    // 共同区分两种限速来源: 前者是 GCL 窗口配额, 后者是端口线速。
    std::uint64_t creditDeferrals{};
    std::uint64_t latencySamples{};
    std::array<PriorityCounters, 8> perPriority{};
};

class Scheduler {
public:
    using Clock = std::chrono::steady_clock;

    explicit Scheduler(SchedulerConfig config = {});
    bool enqueue(Frame frame, Clock::time_point now = Clock::now());
    std::vector<Frame> releaseReady(Clock::time_point now = Clock::now());
    SchedulerStats stats() const;
    std::array<PrioritySummary, LatencyStats::priorityCount> latencySummaries() const;
    // 当前时刻所属的 GCL 相位(窗口边界、掩码、周期序号)。驱动器据此在
    // 绝对时刻唤醒, 测试据此核对相位误差与窗口配额。
    GatePhase gatePhase(Clock::time_point now = Clock::now()) const;
    const SchedulerConfig &config() const noexcept { return config_; }

private:
    struct ScheduledFrame {
        Frame frame;
        Clock::time_point eligibleAt;
    };

    std::uint8_t gateMask(Clock::time_point now) const;
    std::chrono::microseconds sampledJitter();
    std::size_t wireBytes(const Frame &frame) const;
    bool shapingEnabled() const noexcept { return config_.shapingRateMbps > 0.0; }
    bool creditEnabled() const noexcept { return config_.creditFramesPerWindow > 0; }
    // 按 now 补充令牌，返回本帧能否立即出队；不足时不消耗令牌。
    bool takeTokens(std::size_t bytes, Clock::time_point now);
    // 相位变化时重置窗口信用, 返回本窗口剩余可放行帧数; 未启用时返回
    // std::numeric_limits<std::size_t>::max() 表示不设上限。
    std::size_t refreshCredit(const GatePhase &phase);

    SchedulerConfig config_;
    Clock::time_point cycleStart_;
    std::array<std::queue<ScheduledFrame>, 8> queues_;
    mutable std::mutex mutex_;
    SchedulerStats stats_;
    std::mt19937_64 random_;
    std::uniform_real_distribution<double> probability_{0.0, 1.0};
    LatencyStats latency_;
    // 令牌桶状态。首次出队时才用传入的 now 建立基准，使同一组显式时间戳
    // 无论构造时刻如何都得到相同的整形结果。
    std::optional<Clock::time_point> shapingClock_;
    double tokens_{0.0};
    double burst_{0.0};
    double rateBytesPerSecond_{0.0};
    // 窗口信用状态。窗口由 (cycleIndex, slotIndex) 唯一标识，只有跨窗口时才
    // 重置配额，因此同一窗口内多次 releaseReady 共享同一份信用。
    bool creditActive_{false};
    bool creditDeferralCounted_{false};
    std::int64_t creditCycle_{0};
    std::size_t creditSlot_{0};
    std::size_t creditLeft_{0};
};

} // namespace tsnhub
