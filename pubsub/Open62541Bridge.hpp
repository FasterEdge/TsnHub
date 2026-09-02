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
