// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#pragma once

#include "tsn/Scheduler.hpp"

#include <array>
#include <cstdint>
#include <iosfwd>
#include <string>

namespace tsnhub {

// 一次运行结束时的可观测输出: 每优先级的计数、端到端时延分位数、
// 抖动与乱序, 以及画丢包率-负载曲线所需的分母。
struct RunReport {
    std::array<PriorityCounters, 8> perPriority{};
    std::array<PrioritySummary, LatencyStats::priorityCount> latency{};
    SchedulerStats totals{};
    std::string note{};  // 例如整形速率、门控周期, 便于把数据集与配置对上
};

RunReport makeRunReport(const SchedulerStats &stats,
                        const std::array<PrioritySummary, LatencyStats::priorityCount> &latency,
                        std::string note = {});

// 单行摘要, 与既有 accepted=/released=/dropped_loss=/dropped_queue= 前缀保持兼容。
void writeSummaryLine(std::ostream &out, const RunReport &report);
// 人类可读的按优先级分位数表。
void writeLatencyTable(std::ostream &out, const RunReport &report);
// 机器可读 CSV: 每个优先级一行, 表头固定, 便于论文/脚本直接画图。
void writePriorityCsv(std::ostream &out, const RunReport &report);
// JSON 对象, 供后续工具链消费。
void writeJson(std::ostream &out, const RunReport &report);

} // namespace tsnhub
