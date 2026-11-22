// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "TestAssertions.hpp"
#include "tsn/LatencyStats.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using tsnhub::LatencyStats;
using tsnhub::PrioritySummary;

// 时间源由测试以 duration 显式注入, LatencyStats 自身不读真实时钟,
// 因此这里可以逐位断言分位数。
static PrioritySummary measure(const std::vector<std::int64_t> &latenciesUs,
                               std::uint8_t priority = 0) {
    LatencyStats stats(1024, 7);
    for(std::uint64_t sequence = 0; sequence < latenciesUs.size(); ++sequence)
        stats.record(priority, std::chrono::microseconds{latenciesUs[sequence]},
                     sequence, "stream-a");
    return stats.summary(priority);
}

// 分位数取最近秩: n=10 时 p50 是第 5 小, p99 是第 10 小(即 max)。
static void quantilesUseNearestRank() {
    const auto summary = measure({500, 100, 300, 700, 200, 900, 400, 600, 800, 1000});
    TSNHUB_TEST_REQUIRE(summary.samples == 10);
    TSNHUB_TEST_REQUIRE(summary.retained == 10);
    TSNHUB_TEST_REQUIRE(summary.minUs == 100);
    TSNHUB_TEST_REQUIRE(summary.maxUs == 1000);
    TSNHUB_TEST_REQUIRE_MESSAGE(summary.p50Us == 500, "p50 must be the 5th smallest sample");
    TSNHUB_TEST_REQUIRE_MESSAGE(summary.p90Us == 900, "p90 must be the 9th smallest sample");
    TSNHUB_TEST_REQUIRE_MESSAGE(summary.p99Us == 1000, "p99 must be the 10th smallest sample");
    TSNHUB_TEST_REQUIRE(summary.meanUs == 550);
    TSNHUB_TEST_REQUIRE(summary.samples == LatencyStats(1024, 1).samples() + 10);
}

// 单样本时 min/p50/p90/p99/max/mean 必须全部等于该样本, 抖动为 0。
static void singleSampleIsFullyDetermined() {
    const auto summary = measure({237});
    TSNHUB_TEST_REQUIRE(summary.samples == 1);
    TSNHUB_TEST_REQUIRE(summary.minUs == 237);
    TSNHUB_TEST_REQUIRE(summary.p50Us == 237);
    TSNHUB_TEST_REQUIRE(summary.p90Us == 237);
    TSNHUB_TEST_REQUIRE(summary.p99Us == 237);
    TSNHUB_TEST_REQUIRE(summary.maxUs == 237);
    TSNHUB_TEST_REQUIRE(summary.meanUs == 237);
    TSNHUB_TEST_REQUIRE_MESSAGE(summary.jitterUs == 0,
                                "jitter needs at least two samples");
}

// 抖动定义为相邻样本时延绝对差的均值。
static void jitterIsMeanAbsoluteDelta() {
    const auto summary = measure({100, 300, 300, 600});
    // |100-300| + |300-300| + |300-600| = 200 + 0 + 300 = 500, 除以 3 个间隔
    TSNHUB_TEST_REQUIRE_MESSAGE(summary.jitterUs == 166,
                                "jitter must be the mean absolute delta of neighbours");
}

// 空队列必须返回全零摘要, 而不是未定义值。
static void emptySummaryIsZeroed() {
    LatencyStats stats;
    const auto summary = stats.summary(3);
    TSNHUB_TEST_REQUIRE(summary.samples == 0);
    TSNHUB_TEST_REQUIRE(summary.retained == 0);
    TSNHUB_TEST_REQUIRE(summary.p50Us == 0);
    TSNHUB_TEST_REQUIRE(summary.p99Us == 0);
    TSNHUB_TEST_REQUIRE(summary.maxUs == 0);
    TSNHUB_TEST_REQUIRE(stats.samples() == 0);
    TSNHUB_TEST_REQUIRE(stats.retention() == LatencyStats::defaultRetention);
}

// 优先级 8 及以上必须被夹到 7, 不能越界写。
static void priorityAboveSevenIsClamped() {
    LatencyStats stats(64, 3);
    stats.record(200, 10us, 1, "s");
    const auto high = stats.summary(7);
    TSNHUB_TEST_REQUIRE_MESSAGE(high.samples == 1,
                                "priority 200 must be counted under priority 7");
    TSNHUB_TEST_REQUIRE(stats.summary(6).samples == 0);
    TSNHUB_TEST_REQUIRE(stats.summaries()[7].samples == 1);
    TSNHUB_TEST_REQUIRE(stats.samples() == 1);
}

// 乱序与重复按 stream 内的 sequence 回退/重复计数, 各 stream 互不干扰。
static void reorderingAndDuplicatesAreCountedPerStream() {
    LatencyStats stats(64, 3);
    stats.record(1, 100us, 10, "a");
    stats.record(1, 110us, 11, "a");
    stats.record(1, 120us, 11, "a");  // 重复
    stats.record(1, 130us, 9, "a");   // 回退
    stats.record(1, 140us, 20, "b");
    stats.record(1, 150us, 21, "b");
    const auto summary = stats.summary(1);
    TSNHUB_TEST_REQUIRE_MESSAGE(summary.samples == 6, "every recorded frame counts");
    TSNHUB_TEST_REQUIRE_MESSAGE(summary.reordered == 1, "one backwards step in stream a");
    TSNHUB_TEST_REQUIRE_MESSAGE(summary.duplicates == 1, "one repeated sequence in stream a");
}

// 负时延必须被拒绝, 否则分位数会退化。
static void negativeLatencyIsRejected() {
    LatencyStats stats(64, 3);
    bool thrown = false;
    try {
        stats.record(0, std::chrono::microseconds{-1}, 1, "s");
    } catch(const std::invalid_argument &) {
        thrown = true;
    }
    TSNHUB_TEST_REQUIRE_MESSAGE(thrown, "negative latency must throw");
    TSNHUB_TEST_REQUIRE(stats.samples() == 0);
}

// 保留上限生效: samples 统计全部帧, retained 只统计保留样本。
static void retentionBoundsMemory() {
    LatencyStats stats(8, 99);
    for(std::uint64_t sequence = 0; sequence < 100; ++sequence)
        stats.record(0, std::chrono::microseconds{static_cast<long long>(sequence)}, sequence, "s");
    const auto summary = stats.summary(0);
    TSNHUB_TEST_REQUIRE_MESSAGE(summary.samples == 100,
                                "samples must count every offered frame");
    TSNHUB_TEST_REQUIRE_MESSAGE(summary.retained == 8,
                                "retained must stay at the retention bound");
    TSNHUB_TEST_REQUIRE(summary.retained <= stats.retention());
    // 蓄水池抽样是确定性的: 相同输入两次运行必须得到同一组样本。
    LatencyStats repeat(8, 99);
    for(std::uint64_t sequence = 0; sequence < 100; ++sequence)
        repeat.record(0, std::chrono::microseconds{static_cast<long long>(sequence)}, sequence, "s");
    const auto again = repeat.summary(0);
    TSNHUB_TEST_REQUIRE(again.p50Us == summary.p50Us);
    TSNHUB_TEST_REQUIRE(again.p99Us == summary.p99Us);
    TSNHUB_TEST_REQUIRE(again.maxUs == summary.maxUs);
    TSNHUB_TEST_REQUIRE(again.meanUs == summary.meanUs);
}

// clear 必须清空样本、分位数与 stream 序号基准。
static void clearResetsEverything() {
    LatencyStats stats(64, 5);
    stats.record(1, 100us, 5, "a");
    stats.clear();
    TSNHUB_TEST_REQUIRE(stats.samples() == 0);
    const auto summary = stats.summary(1);
    TSNHUB_TEST_REQUIRE(summary.samples == 0);
    TSNHUB_TEST_REQUIRE(summary.reordered == 0);
    stats.record(1, 200us, 3, "a");
    TSNHUB_TEST_REQUIRE_MESSAGE(stats.summary(1).reordered == 0,
                                "stream sequence baseline must be reset by clear");
}

int main() {
    quantilesUseNearestRank();
    singleSampleIsFullyDetermined();
    jitterIsMeanAbsoluteDelta();
    emptySummaryIsZeroed();
    priorityAboveSevenIsClamped();
    reorderingAndDuplicatesAreCountedPerStream();
    negativeLatencyIsRejected();
    retentionBoundsMemory();
    clearResetsEverything();

    TSNHUB_TEST_REPORT("Latency stats tests passed");
    return 0;
}
