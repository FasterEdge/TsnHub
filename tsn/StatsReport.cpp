// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "tsn/StatsReport.hpp"

#include <sstream>

namespace tsnhub {
namespace {
PriorityCounters aggregate(const SchedulerStats &stats) {
    PriorityCounters total;
    total.accepted = stats.accepted;
    total.released = stats.released;
    total.droppedByLoss = stats.droppedByLoss;
    total.droppedByQueue = stats.droppedByQueue;
    total.shapedDeferrals = stats.shapedDeferrals;
    total.creditDeferrals = stats.creditDeferrals;
    return total;
}
void writeCounts(std::ostream &out, const PriorityCounters &counters) {
    out << "accepted=" << counters.accepted << " released=" << counters.released
        << " dropped_loss=" << counters.droppedByLoss
        << " dropped_queue=" << counters.droppedByQueue
        << " shaped_deferrals=" << counters.shapedDeferrals
        << " credit_deferrals=" << counters.creditDeferrals;
}
} // namespace

RunReport makeRunReport(const SchedulerStats &stats,
                        const std::array<PrioritySummary, LatencyStats::priorityCount> &latency,
                        std::string note) {
    RunReport report;
    report.perPriority = stats.perPriority;
    report.totals = stats;
    report.latency = latency;
    report.note = std::move(note);
    return report;
}

void writeSummaryLine(std::ostream &out, const RunReport &report) {
    writeCounts(out, aggregate(report.totals));
    out << " latency_samples=" << report.totals.latencySamples << '\n';
}

void writeLatencyTable(std::ostream &out, const RunReport &report) {
    out << "priority samples retained p50_us p99_us max_us mean_us jitter_us "
           "reordered duplicates\n";
    for(const auto &summary : report.latency) {
        out << static_cast<unsigned>(summary.priority) << ' ' << summary.samples << ' '
            << summary.retained << ' ' << summary.p50Us << ' ' << summary.p99Us << ' '
            << summary.maxUs << ' ' << summary.meanUs << ' ' << summary.jitterUs << ' '
            << summary.reordered << ' ' << summary.duplicates << '\n';
    }
}

void writePriorityCsv(std::ostream &out, const RunReport &report) {
    out << "priority,samples,retained,min_us,p50_us,p90_us,p99_us,max_us,mean_us,"
           "jitter_us,reordered,duplicates,offered,accepted,released,dropped_loss,"
           "dropped_queue,shaped_deferrals,credit_deferrals,loss_rate,drop_rate\n";
    for(const auto &summary : report.latency) {
        const auto &counters = report.perPriority[summary.priority];
        const auto offered = counters.accepted + counters.droppedByLoss +
                             counters.droppedByQueue;
        out << static_cast<unsigned>(summary.priority) << ',' << summary.samples << ','
            << summary.retained << ',' << summary.minUs << ',' << summary.p50Us << ','
            << summary.p90Us << ',' << summary.p99Us << ',' << summary.maxUs << ','
            << summary.meanUs << ',' << summary.jitterUs << ',' << summary.reordered << ','
            << summary.duplicates << ',' << offered << ',' << counters.accepted << ','
            << counters.released << ',' << counters.droppedByLoss << ','
            << counters.droppedByQueue << ',' << counters.shapedDeferrals << ','
            << counters.creditDeferrals;
        out << ',' << (offered == 0 ? 0.0 : static_cast<double>(counters.droppedByLoss) /
                                              static_cast<double>(offered))
            << ','
            << (offered == 0 ? 0.0 : static_cast<double>(counters.droppedByLoss +
                                                        counters.droppedByQueue) /
                                              static_cast<double>(offered))
            << '\n';
    }
}

void writeJson(std::ostream &out, const RunReport &report) {
    out << "{\n";
    if(!report.note.empty()) {
        out << "  \"note\": \"" << report.note << "\",\n";
    }
    out << "  \"totals\": {\"accepted\": " << report.totals.accepted
        << ", \"released\": " << report.totals.released
        << ", \"dropped_loss\": " << report.totals.droppedByLoss
        << ", \"dropped_queue\": " << report.totals.droppedByQueue
        << ", \"shaped_deferrals\": " << report.totals.shapedDeferrals
        << ", \"credit_deferrals\": " << report.totals.creditDeferrals
        << ", \"latency_samples\": " << report.totals.latencySamples << "},\n";
    out << "  \"priorities\": [\n";
    for(std::size_t index = 0; index < report.latency.size(); ++index) {
        const auto &summary = report.latency[index];
        const auto &counters = report.perPriority[index];
        const auto offered = counters.accepted + counters.droppedByLoss +
                             counters.droppedByQueue;
        out << "    {\"priority\": " << static_cast<unsigned>(summary.priority)
            << ", \"samples\": " << summary.samples
            << ", \"retained\": " << summary.retained
            << ", \"min_us\": " << summary.minUs
            << ", \"p50_us\": " << summary.p50Us
            << ", \"p90_us\": " << summary.p90Us
            << ", \"p99_us\": " << summary.p99Us
            << ", \"max_us\": " << summary.maxUs
            << ", \"mean_us\": " << summary.meanUs
            << ", \"jitter_us\": " << summary.jitterUs
            << ", \"reordered\": " << summary.reordered
            << ", \"duplicates\": " << summary.duplicates
            << ", \"offered\": " << offered
            << ", \"accepted\": " << counters.accepted
            << ", \"released\": " << counters.released
            << ", \"dropped_loss\": " << counters.droppedByLoss
            << ", \"dropped_queue\": " << counters.droppedByQueue
            << ", \"shaped_deferrals\": " << counters.shapedDeferrals
            << ", \"credit_deferrals\": " << counters.creditDeferrals << "}";
        if(index + 1 < report.latency.size())
            out << ',';
        out << '\n';
    }
    out << "  ]\n}\n";
}

} // namespace tsnhub
