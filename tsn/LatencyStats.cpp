// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "tsn/LatencyStats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <stdexcept>

namespace tsnhub {
namespace {
std::int64_t toUs(std::chrono::nanoseconds value) {
    return std::chrono::duration_cast<std::chrono::microseconds>(value).count();
}
// 最近秩法: 第 q 分位数是排序后第 ceil(q*n) 小的元素(1 起)。
std::int64_t quantile(const std::vector<std::int64_t> &sorted, double q) {
    if(sorted.empty()) return 0;
    if(q <= 0.0) return sorted.front();
    if(q >= 1.0) return sorted.back();
    const double rank = q * static_cast<double>(sorted.size());
    auto index = static_cast<std::size_t>(std::ceil(rank));
    if(index == 0) index = 1;
    if(index > sorted.size()) index = sorted.size();
    return sorted[index - 1];
}
} // namespace

LatencyStats::LatencyStats(std::size_t retention, std::uint64_t seed)
    : retention_(retention == 0 ? defaultRetention : retention),
      randomState_(seed ^ 0x9e3779b97f4a7c15ULL) {}

std::uint64_t LatencyStats::nextRandom() {
    // splitmix64: 64 位状态、周期足够长，避免蓄水池抽样在少量样本上出现模式。
    randomState_ += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = randomState_;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

std::size_t LatencyStats::clampPriority(std::uint8_t priority) {
    return priority > 7 ? 7u : static_cast<std::size_t>(priority);
}

void LatencyStats::admit(std::vector<std::int64_t> &series, std::int64_t value,
                         std::uint64_t seen) {
    if(series.size() < retention_)
        series.push_back(value);
    else {
        const auto index = static_cast<std::size_t>(nextRandom() % static_cast<std::uint64_t>(seen));
        if(index < series.size())
            series[index] = value;
    }
}

void LatencyStats::record(std::uint8_t priority, std::chrono::nanoseconds latency,
                          std::uint64_t sequence, const std::string &stream) {
    if(latency.count() < 0)
        throw std::invalid_argument("latency must not be negative");
    auto &state = states_[clampPriority(priority)];
    ++state.samples;
    const auto latencyUs = toUs(latency);

    const auto [entry, inserted] = lastSequenceByStream_.emplace(stream, 0);
    (void)entry;
    if(inserted) {
        // 同一 stream 的第一个样本只负责建立序号基准。
    } else {
        const auto previous = entry->second;
        if(sequence < previous)
            ++state.reordered;
        else if(sequence == previous)
            ++state.duplicates;
    }
    entry->second = sequence;

    if(state.hasPrevious) {
        admit(state.deltasUs, std::llabs(latencyUs - state.previousUs), state.samples);
        ++state.deltaSamples;
    }
    state.previousUs = latencyUs;
    state.hasPrevious = true;
    admit(state.latenciesUs, latencyUs, state.samples);
}

PrioritySummary LatencyStats::summary(std::uint8_t priority) const {
    const auto &state = states_[clampPriority(priority)];
    PrioritySummary out;
    out.priority = clampPriority(priority);
    out.samples = state.samples;
    out.retained = state.latenciesUs.size();
    out.reordered = state.reordered;
    out.duplicates = state.duplicates;
    if(state.latenciesUs.empty())
        return out;

    std::vector<std::int64_t> sorted = state.latenciesUs;
    std::sort(sorted.begin(), sorted.end());
    out.minUs = sorted.front();
    out.p50Us = quantile(sorted, 0.50);
    out.p90Us = quantile(sorted, 0.90);
    out.p99Us = quantile(sorted, 0.99);
    out.maxUs = sorted.back();
    const auto total = std::accumulate(sorted.begin(), sorted.end(), 0LL);
    out.meanUs = total / static_cast<std::int64_t>(sorted.size());

    if(!state.deltasUs.empty()) {
        const auto deltas = std::accumulate(state.deltasUs.begin(), state.deltasUs.end(), 0LL);
        out.jitterUs = deltas / static_cast<std::int64_t>(state.deltasUs.size());
    }
    return out;
}

std::array<PrioritySummary, LatencyStats::priorityCount> LatencyStats::summaries() const {
    std::array<PrioritySummary, priorityCount> out{};
    for(std::size_t priority = 0; priority < priorityCount; ++priority)
        out[priority] = summary(static_cast<std::uint8_t>(priority));
    return out;
}

std::uint64_t LatencyStats::samples() const {
    std::uint64_t total = 0;
    for(const auto &state : states_)
        total += state.samples;
    return total;
}

void LatencyStats::clear() {
    for(auto &state : states_) {
        PriorityState fresh;
        state = std::move(fresh);
    }
    lastSequenceByStream_.clear();
}

} // namespace tsnhub
