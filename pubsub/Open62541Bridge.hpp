// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#pragma once

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
};

class Open62541Bridge {
public:
    Open62541Bridge(NativePubSubConfig config, SchedulerConfig scheduler);
    ~Open62541Bridge();
    Open62541Bridge(const Open62541Bridge &) = delete;
    Open62541Bridge &operator=(const Open62541Bridge &) = delete;

    void run(std::atomic_bool &running);
    SchedulerStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tsnhub
