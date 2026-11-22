// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#pragma once

#include "tsn/PriorityMap.hpp"
#include "tsn/Scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace tsnhub {

struct NativePubSubConfig {
    std::string listenUrl{"opc.udp://0.0.0.0:4841"};
    std::string publishUrl{"opc.udp://127.0.0.1:4842"};
    // open62541 >= 1.5.x rejects a UDP PubSub connection URL with port 0, so the
    // publisher can no longer bind an OS-assigned ephemeral port. This is the
    // local bind for the writer's send socket; it must be non-zero and distinct
    // from listenUrl and from any co-located subscriber bound to publishUrl.
    std::string publisherBindUrl{"opc.udp://0.0.0.0:4843"};
    std::uint16_t inputPublisherId{1};
    std::uint16_t inputWriterGroupId{1};
    std::uint16_t inputDataSetWriterId{1};
    std::uint16_t outputPublisherId{2};
    std::uint16_t outputWriterGroupId{2};
    std::uint16_t outputDataSetWriterId{2};
    double publishingIntervalMs{1.0};
    std::chrono::milliseconds pollInterval{1};
    // PublisherId/WriterGroupId/DataSetWriterId -> priority 映射。空规则集表示
    // 所有输入帧都落到 defaultPriority，等价于 Open62541Bridge 早期把 priority
    // 硬编码为 0 的行为。
    PriorityMap priorityMap;
    // 未命中任何规则时使用的优先级，可通过 --input-default-priority 调整。
    std::uint8_t inputDefaultPriority{0};
};

class Open62541Bridge {
public:
    Open62541Bridge(NativePubSubConfig config, SchedulerConfig scheduler);
    ~Open62541Bridge();
    Open62541Bridge(const Open62541Bridge &) = delete;
    Open62541Bridge &operator=(const Open62541Bridge &) = delete;

    void run(std::atomic_bool &running);
    SchedulerStats stats() const;
    std::array<PrioritySummary, LatencyStats::priorityCount> latencySummaries() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tsnhub
