#include "command_line/CommandLineBridge.h"

#include <csignal>
#include <iostream>

bool CommandLineBridge::configure(const Open62541RuntimeConfig &cfg, std::string &err) {
    if (cfg.endpoint.empty() || cfg.txId.empty() || cfg.rxId.empty()) {
        err = "ERR 缺少必要参数 endpoint/tx/rx";
        return false;
    }
    cfg_ = cfg;
    configured_ = true;
    return true;
}

void CommandLineBridge::bindAdapter(Open62541Adapter *adapter) {
    adapter_ = adapter;
}

void CommandLineBridge::run(volatile std::sig_atomic_t *stopFlag) {
    if (!adapter_) {
        std::cerr << "CommandLineBridge 未绑定 Open62541Adapter" << std::endl;
        return;
    }
    if (!configured_) {
        std::cerr << "CommandLineBridge 未配置 UA 参数" << std::endl;
        return;
    }

    if (!adapter_->configure(cfg_)) {
        std::cerr << "ERR 配置应用失败" << std::endl;
        return;
    }

    // 下行：订阅回调直接输出到 stdout。
    adapter_->subscribe([](const std::string &msg) {
        std::cout << msg << std::endl;
    });

    // 上行：从 stdin 逐行读取并发送到 TSN。
    std::string line;
    while (stopFlag && !(*stopFlag) && std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        if (!adapter_->send(line)) {
            std::cerr << "发送失败" << std::endl;
        }
    }
}
