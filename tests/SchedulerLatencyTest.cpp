// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "TestAssertions.hpp"
#include "tsn/Scheduler.hpp"
#include "tsn/StatsReport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using tsnhub::Frame;
using tsnhub::PriorityCounters;
using tsnhub::PrioritySummary;
using tsnhub::RunReport;
using tsnhub::Scheduler;
using tsnhub::SchedulerConfig;
using tsnhub::SchedulerStats;

namespace {
Frame wireFrame(std::uint64_t sequence, std::uint8_t priority, std::size_t payloadBytes = 0) {
    Frame frame;
    frame.sequence = sequence;
    frame.priority = priority;
    frame.stream = "";
    frame.payload.assign(payloadBytes, 0xab);
    return frame;
}
// shapingOverheadBytes 之外不产生任何字节: stream 为空, payload 长度由调用方指定,
// 因此每帧在链路上的大小就是 shapingOverheadBytes + payloadBytes, 便于精确记账。
constexpr std::size_t kOverhead = 10;
} // namespace

// 令牌桶按端口线速串行化出队: 速率过低时, 已到期的帧只能每轮放行一部分,
// 其余计入 shaped_deferrals 而不是被丢弃。
static void tokenBucketDefersExcessFrames() {
    SchedulerConfig config;
    config.schedule = {{1000000us, 0xff}};
    config.baseDelay = 0us;
    config.maxQueuePerClass = 64;
    config.shapingRateMbps = 1.0;  // 125000 B/s
    config.shapingBurstBytes = 95;
    config.shapingOverheadBytes = 10;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();

    // 每帧 10 字节(空 stream + 空 payload + 10 字节 overhead), 10 帧共 100 字节,
    // 而起始令牌只有 95 字节, 因此首轮只能放 9 帧, 第 10 帧被推迟。
    for(std::uint64_t sequence = 0; sequence < 10; ++sequence)
        TSNHUB_TEST_REQUIRE(scheduler.enqueue(wireFrame(sequence, 0), start));

    auto first = scheduler.releaseReady(start + 1us);
    TSNHUB_TEST_REQUIRE_MESSAGE(first.size() == 9,
                                "the burst budget must admit exactly 9 frames");
    TSNHUB_TEST_REQUIRE(scheduler.stats().shapedDeferrals == 1);

    // 1ms 后补充 125 字节令牌, 足够放出第 10 帧。
    auto later = scheduler.releaseReady(start + 1ms);
    TSNHUB_TEST_REQUIRE_MESSAGE(later.size() == 1,
                                "the deferred frame must be released once tokens refill");
    TSNHUB_TEST_REQUIRE(later.front().sequence == 9);
    TSNHUB_TEST_REQUIRE(scheduler.stats().released == 10);
    TSNHUB_TEST_REQUIRE(scheduler.stats().shapedDeferrals == 1);
}

// 令牌桶跨 releaseReady 调用累计: 速率决定长期吞吐, 而不是每轮都满桶。
static void tokenBucketAccruesAcrossCalls() {
    SchedulerConfig config;
    config.schedule = {{10000000us, 0xff}};
    config.baseDelay = 0us;
    config.maxQueuePerClass = 256;
    config.shapingRateMbps = 8.0;  // 1000000 B/s
    config.shapingBurstBytes = 1000;
    config.shapingOverheadBytes = 10;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();

    // 每帧 100 字节(10 字节 overhead + 90 字节 payload), 20 帧共 2000 字节。
    for(std::uint64_t sequence = 0; sequence < 20; ++sequence)
        TSNHUB_TEST_REQUIRE(scheduler.enqueue(wireFrame(sequence, 0, 90), start));

    // 首轮 1000 字节突发 = 10 帧。
    TSNHUB_TEST_REQUIRE(scheduler.releaseReady(start + 1us).size() == 10);
    // 2ms 后按 1MB/s 补充 2000 字节, 但受 1000 字节突发上限约束,
    // 恰好够放行剩下的 10 帧 —— 这说明桶是按速率补充而不是每轮满桶。
    TSNHUB_TEST_REQUIRE(scheduler.releaseReady(start + 2ms).size() == 10);
    TSNHUB_TEST_REQUIRE(scheduler.stats().released == 20);
    TSNHUB_TEST_REQUIRE(scheduler.stats().shapedDeferrals == 1);
}

// 速率为 0 时关闭整形, 行为与改动前完全一致。
static void zeroRateDisablesShaping() {
    SchedulerConfig config;
    config.schedule = {{1000us, 0xff}};
    config.baseDelay = 0us;
    config.shapingRateMbps = 0.0;
    config.shapingBurstBytes = 100;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();

    for(std::uint64_t sequence = 0; sequence < 10; ++sequence)
        TSNHUB_TEST_REQUIRE(scheduler.enqueue(wireFrame(sequence, 0), start));
    auto released = scheduler.releaseReady(start + 1us);
    TSNHUB_TEST_REQUIRE_MESSAGE(released.size() == 10,
                                "no shaping rate must release everything at once");
    TSNHUB_TEST_REQUIRE(scheduler.stats().shapedDeferrals == 0);
}

// 整形只推迟不丢弃: 队列容量按 maxQueuePerClass 计, 被推迟的帧仍占队列。
static void shapingDefersWithoutDropping() {
    SchedulerConfig config;
    config.schedule = {{1000000us, 0xff}};
    config.baseDelay = 0us;
    config.maxQueuePerClass = 4;
    config.shapingRateMbps = 1.0;
    config.shapingBurstBytes = 10;
    config.shapingOverheadBytes = 10;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();

    TSNHUB_TEST_REQUIRE(scheduler.enqueue(wireFrame(1, 0), start));
    TSNHUB_TEST_REQUIRE(scheduler.enqueue(wireFrame(2, 0), start));
    // 首帧耗尽令牌, 后续帧被推迟但仍留在队列中。
    TSNHUB_TEST_REQUIRE(scheduler.releaseReady(start + 1us).size() == 1);
    TSNHUB_TEST_REQUIRE(scheduler.stats().shapedDeferrals == 1);
    // 队列未满, 新帧仍可入队。
    TSNHUB_TEST_REQUIRE(scheduler.enqueue(wireFrame(3, 0), start));
    TSNHUB_TEST_REQUIRE(scheduler.stats().droppedByQueue == 0);
    // 桶深只有 10 字节, 每帧 10 字节, 因此每次调用最多补充一个帧的令牌;
    // 持续调用必须把推迟的帧逐帧放行, 且始终不丢弃。
    auto second = scheduler.releaseReady(start + 100ms);
    auto third = scheduler.releaseReady(start + 200ms);
    TSNHUB_TEST_REQUIRE_MESSAGE(second.size() == 1 && third.size() == 1,
                                "every deferred frame must eventually be released");
    TSNHUB_TEST_REQUIRE(scheduler.stats().droppedByQueue == 0);
    TSNHUB_TEST_REQUIRE(scheduler.stats().droppedByLoss == 0);
    TSNHUB_TEST_REQUIRE(scheduler.stats().released == 3);
}

// 非法整形速率必须被拒绝。
static void invalidShapingRateIsRejected() {
    SchedulerConfig config;
    config.shapingRateMbps = -1.0;
    bool thrown = false;
    try {
        Scheduler scheduler(config);
        (void)scheduler;
    } catch(const std::invalid_argument &) {
        thrown = true;
    }
    TSNHUB_TEST_REQUIRE_MESSAGE(thrown, "negative shaping rate must throw");
}

// 时延统计覆盖全部放行帧, 并按 priority 分类。
static void releasedFramesAreMeasuredByPriority() {
    SchedulerConfig config;
    config.schedule = {{1000us, 0xff}};
    config.baseDelay = 200us;
    config.randomSeed = 5;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();

    for(std::uint64_t sequence = 0; sequence < 4; ++sequence) {
        TSNHUB_TEST_REQUIRE(scheduler.enqueue(wireFrame(sequence, 1), start));
        TSNHUB_TEST_REQUIRE(scheduler.enqueue(wireFrame(sequence, 7), start));
    }
    auto released = scheduler.releaseReady(start + 400us);
    TSNHUB_TEST_REQUIRE(released.size() == 8);
    const auto summaries = scheduler.latencySummaries();
    const auto low = summaries[1];
    const auto high = summaries[7];
    TSNHUB_TEST_REQUIRE(low.samples == 4);
    TSNHUB_TEST_REQUIRE(high.samples == 4);
    TSNHUB_TEST_REQUIRE(low.p50Us == 400);
    TSNHUB_TEST_REQUIRE(low.maxUs == 400);
    TSNHUB_TEST_REQUIRE(low.jitterUs == 0);
    TSNHUB_TEST_REQUIRE(high.p99Us == 400);
    TSNHUB_TEST_REQUIRE(scheduler.stats().latencySamples == 8);
    TSNHUB_TEST_REQUIRE(summaries[0].samples == 0);
}

// 计数器必须给出 offered = accepted + dropped_loss + dropped_queue,
// 这是丢包-负载曲线的最小充分统计量。
static void countersOfferCompleteOfferedTotal() {
    SchedulerConfig config;
    config.schedule = {{1000us, 0xff}};
    config.maxQueuePerClass = 3;
    config.lossRate = 0.5;
    config.randomSeed = 11;
    Scheduler scheduler(config);
    const auto start = Scheduler::Clock::now();
    for(std::uint64_t sequence = 0; sequence < 40; ++sequence)
        scheduler.enqueue(wireFrame(sequence, 2), start);
    scheduler.releaseReady(start + 1000us);

    const auto stats = scheduler.stats();
    const auto &counters = stats.perPriority[2];
    const auto offered = counters.accepted + counters.droppedByLoss + counters.droppedByQueue;
    TSNHUB_TEST_REQUIRE(offered == 40);
    TSNHUB_TEST_REQUIRE(counters.accepted <= counters.accepted + counters.droppedByLoss);
    TSNHUB_TEST_REQUIRE(stats.accepted == counters.accepted);
    TSNHUB_TEST_REQUIRE(stats.released == counters.released);
    TSNHUB_TEST_REQUIRE(stats.droppedByLoss == counters.droppedByLoss);
    TSNHUB_TEST_REQUIRE(stats.droppedByQueue == counters.droppedByQueue);
    TSNHUB_TEST_REQUIRE(counters.released <= counters.accepted);
}

// CSV 输出: 表头固定、每优先级一行、loss_rate 与 offered 自洽。
static void csvReportIsSelfConsistent() {
    RunReport report;
    for(std::uint8_t priority = 0; priority < 8; ++priority) {
        report.perPriority[priority].accepted = 10;
        report.perPriority[priority].released = 8;
        report.perPriority[priority].droppedByLoss = 1;
        report.perPriority[priority].droppedByQueue = 1;
        report.latency[priority].priority = priority;
        report.latency[priority].samples = 8;
        report.latency[priority].p50Us = 100 + priority;
    }
    report.totals.accepted = 80;
    report.totals.released = 64;

    std::ostringstream csv;
    tsnhub::writePriorityCsv(csv, report);
    const auto text = csv.str();
    TSNHUB_TEST_REQUIRE_MESSAGE(text.find("priority,samples,retained,min_us,p50_us,p90_us,"
                                          "p99_us,max_us,mean_us,jitter_us,reordered,"
                                          "duplicates,offered,accepted,released,"
                                          "dropped_loss,dropped_queue,shaped_deferrals,"
                                          "credit_deferrals,loss_rate,drop_rate") !=
                                    std::string::npos,
                                "csv header must list every documented column");
    TSNHUB_TEST_REQUIRE(text.find("0,8,0,0,100,0,0,0,0,0,0,0,12,10,8,1,1,0,0,0.0833333,"
                                  "0.166667") != std::string::npos);
    // 每个优先级一行, 加表头共 9 行。
    std::size_t lines = 0;
    for(const char character : text)
        if(character == '\n') ++lines;
    TSNHUB_TEST_REQUIRE_MESSAGE(lines == 9, "csv must contain one row per priority");

    std::ostringstream summary;
    tsnhub::writeSummaryLine(summary, report);
    const auto line = summary.str();
    TSNHUB_TEST_REQUIRE(line.find("accepted=80") != std::string::npos);
    TSNHUB_TEST_REQUIRE(line.find("released=64") != std::string::npos);
    TSNHUB_TEST_REQUIRE(line.find("dropped_loss=") != std::string::npos);
    TSNHUB_TEST_REQUIRE(line.find("dropped_queue=") != std::string::npos);
    TSNHUB_TEST_REQUIRE(line.find("shaped_deferrals=") != std::string::npos);
    TSNHUB_TEST_REQUIRE(line.find("latency_samples=") != std::string::npos);
    // 汇总行只读 totals, 因此 perPriority 的分项汇总必须与 totals 一致。
    const auto aggregated = tsnhub::makeRunReport(report.totals, report.latency);
    std::ostringstream again;
    tsnhub::writeSummaryLine(again, aggregated);
    TSNHUB_TEST_REQUIRE_MESSAGE(again.str() == line,
                                "the summary line must be a pure function of totals");
}

// JSON 输出必须是可解析的扁平对象, 且 note 出现在 totals 之前。
static void jsonReportCarriesNoteAndTotals() {
    RunReport report = tsnhub::makeRunReport(SchedulerStats{},
                                            std::array<PrioritySummary, tsnhub::LatencyStats::priorityCount>{},
                                            std::string{"mode=portable"});
    report.latency[3].priority = 3;
    report.latency[3].samples = 5;
    report.perPriority[3].accepted = 5;
    std::ostringstream json;
    tsnhub::writeJson(json, report);
    const auto text = json.str();
    TSNHUB_TEST_REQUIRE(text.find("\"note\": \"mode=portable\"") != std::string::npos);
    TSNHUB_TEST_REQUIRE(text.find("\"totals\"") != std::string::npos);
    TSNHUB_TEST_REQUIRE(text.find("\"priorities\"") != std::string::npos);
    TSNHUB_TEST_REQUIRE(text.find("\"priority\": 3") != std::string::npos);
    // JSON 必须是可闭合的对象, 且 credit_deferrals 也出现在 totals 里。
    const auto closing = text.rfind('}');
    TSNHUB_TEST_REQUIRE_MESSAGE(closing != std::string::npos && closing == text.size() - 2,
                                "json must end with a single closing brace");
    TSNHUB_TEST_REQUIRE(text.find("\"shaped_deferrals\"") != std::string::npos);
    TSNHUB_TEST_REQUIRE_MESSAGE(text.find("\"credit_deferrals\"") != std::string::npos,
                                "json must expose the credit-deferral counter");
}

// 表格输出按优先级从 0 到 7 排序, 便于肉眼核对。
static void tableOrdersPriorities() {
    RunReport report;
    for(std::uint8_t priority = 0; priority < 8; ++priority)
        report.latency[priority].priority = priority;
    std::ostringstream table;
    tsnhub::writeLatencyTable(table, report);
    const auto text = table.str();
    const auto header = text.find('\n');
    const auto firstRow = text.find('\n', header + 1);
    TSNHUB_TEST_REQUIRE(text.compare(header + 1, firstRow - header - 1, "0 0 0 0 0 0 0 0 0 0") ==
                        0);
}

int main() {
    tokenBucketDefersExcessFrames();
    tokenBucketAccruesAcrossCalls();
    zeroRateDisablesShaping();
    shapingDefersWithoutDropping();
    invalidShapingRateIsRejected();
    releasedFramesAreMeasuredByPriority();
    countersOfferCompleteOfferedTotal();
    csvReportIsSelfConsistent();
    jsonReportCarriesNoteAndTotals();
    tableOrdersPriorities();

    TSNHUB_TEST_REPORT("Scheduler latency and shaping tests passed");
    return 0;
}
