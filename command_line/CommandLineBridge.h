#pragma once

#include <csignal>
#include <string>

#include "tsn/Open62541Adapter.h"

// CommandLine 桥接器：
// - 使用命令行参数设置 UA 端点/节点
// - stdin 作为上行输入（发送到 TSN）
// - 订阅回调输出到 stdout
class CommandLineBridge {
public:
    CommandLineBridge() = default;

    // 配置 UA 端点与节点。
    bool configure(const Open62541RuntimeConfig &cfg, std::string &err);

    // 绑定 open62541 适配层。
    void bindAdapter(Open62541Adapter *adapter);

    // 运行循环：从 stdin 读取并发送；直到 stopFlag 置位或 EOF。
    void run(volatile std::sig_atomic_t *stopFlag);

private:
    Open62541Adapter *adapter_{nullptr};
    Open62541RuntimeConfig cfg_{};
    bool configured_{false};
};
