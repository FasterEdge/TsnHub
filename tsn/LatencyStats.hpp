// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tsnhub {

// 单个优先级队列的时延统计。分位数采用最近秩法(nearest-rank)，
// 因此样本量较小时结果也是确定的整数，便于在测试里逐位断言。
struct PrioritySummary {
    std::uint8_t priority{};
    std::uint64_t samples{};      // 进入统计的总帧数
    std::uint64_t retained{};     // 实际保留、参与分位数计算的样本数
    std::uint64_t reordered{};    // 同一 stream 内序号回退的次数
    std::uint64_t duplicates{};   // 同一 stream 内序号重复的次数
    std::int64_t minUs{};
    std::int64_t p50Us{};
    std::int64_t p90Us{};
    std::int64_t p99Us{};
    std::int64_t maxUs{};
    std::int64_t meanUs{};
    std::int64_t jitterUs{};      // 相邻样本时延绝对差的均值
};

// 端到端(节点内驻留)时延、抖动与乱序统计，按 TSN 优先级分类。
//
// 时间源由调用方以 std::chrono::nanoseconds 显式传入，本类不读取任何真实时钟，
// 因此同一组输入必然得到同一组输出。调用方负责串行化访问。
//
// 样本量超过 retention 后使用固定种子的蓄水池抽样(reservoir sampling)，
// 使内存占用有界；此时分位数是保留样本上的无偏估计，retained 字段给出实际样本数。
class LatencyStats {
public:
    static constexpr std::size_t priorityCount = 8;
    static constexpr std::size_t defaultRetention = 16384;

    explicit LatencyStats(std::size_t retention = defaultRetention,
                          std::uint64_t seed = 0x9e3779b97f4a7c15ULL);

    void record(std::uint8_t priority, std::chrono::nanoseconds latency,
                std::uint64_t sequence, const std::string &stream);
    PrioritySummary summary(std::uint8_t priority) const;
    std::array<PrioritySummary, priorityCount> summaries() const;
    std::uint64_t samples() const;
    std::size_t retention() const noexcept { return retention_; }
    void clear();

private:
    struct PriorityState {
        std::uint64_t samples{};
        std::vector<std::int64_t> latenciesUs;
        std::vector<std::int64_t> deltasUs;
        std::uint64_t deltaSamples{};
        std::uint64_t reordered{};
        std::uint64_t duplicates{};
        bool hasPrevious{};
        std::int64_t previousUs{};
    };

    static std::size_t clampPriority(std::uint8_t priority);
    void admit(std::vector<std::int64_t> &series, std::int64_t value, std::uint64_t seen);
    std::uint64_t nextRandom();

    std::size_t retention_;
    std::uint64_t randomState_;
    std::array<PriorityState, priorityCount> states_{};
    std::map<std::string, std::uint64_t> lastSequenceByStream_;
};

} // namespace tsnhub
