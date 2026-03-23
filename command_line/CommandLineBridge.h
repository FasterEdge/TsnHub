#pragma once

#include <csignal>
#include <string>

#include "tsn/Open62541Adapter.h"

// CommandLine 桥接器：
// - 可选地通过命令行初始配置 PubSub
// - 运行时支持命令：
//   connect role=... address=... publisherId=... writerId=... readerId=... interval=... field=...
//   disconnect
// - stdin 其他行作为上行输入；订阅回调输出到 stdout
class CommandLineBridge {
public:
    CommandLineBridge() = default;

    // 可选预配置（缺省则等待 connect 指令）。
    bool configure(const PubSubConfig &cfg, std::string &err);

    // 绑定适配层。
    void bindAdapter(Open62541Adapter *adapter);

    // 运行循环：处理 connect/disconnect 命令，或将普通行发送到 TSN。
    void run(volatile std::sig_atomic_t *stopFlag);

private:
    Open62541Adapter *adapter_{nullptr};
    PubSubConfig cfg_{};
    bool configured_{false};
};
