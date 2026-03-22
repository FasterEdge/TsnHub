#include "command_line/CommandLineBridge.h"

#include <algorithm>
#include <csignal>
#include <iostream>
#include <sstream>
#include <cctype>

namespace {
std::string toLower(std::string s) {
    for (char &ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

bool parseNsId(const std::string &v, uint16_t &ns, std::string &id, std::string &err) {
    auto p = v.find(':');
    if (p == std::string::npos) {
        err = "ERR 格式应为 ns:id";
        return false;
    }
    try {
        ns = static_cast<uint16_t>(std::stoi(v.substr(0, p)));
    } catch (...) {
        err = "ERR ns 应为数字";
        return false;
    }
    id = v.substr(p + 1);
    if (id.empty()) {
        err = "ERR id 不能为空";
        return false;
    }
    return true;
}
}

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

    // 先注册订阅回调，确保连接后立即能下行输出。
    adapter_->subscribe([](const std::string &msg) {
        std::cout << msg << std::endl;
    });

    // 若已预配置，则先应用并开启连接。
    if (configured_) {
        if (!adapter_->configure(cfg_)) {
            std::cerr << "ERR 配置应用失败" << std::endl;
        }
    } else {
        // 未配置则保持断开，等待 connect 指令。
        adapter_->setConnectEnabled(false);
    }

    std::string line;
    while (stopFlag && !(*stopFlag) && std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        std::string cmdLower = toLower(cmd);

        if (cmdLower == "connect") {
            Open62541RuntimeConfig cfg;
            std::string token;
            std::string err;
            while (iss >> token) {
                if (token.rfind("endpoint=", 0) == 0) {
                    cfg.endpoint = token.substr(std::strlen("endpoint="));
                } else if (token.rfind("tx=", 0) == 0) {
                    std::string v = token.substr(3);
                    if (!parseNsId(v, cfg.txNs, cfg.txId, err)) {
                        break;
                    }
                } else if (token.rfind("rx=", 0) == 0) {
                    std::string v = token.substr(3);
                    if (!parseNsId(v, cfg.rxNs, cfg.rxId, err)) {
                        break;
                    }
                }
            }

            if (!err.empty() || cfg.endpoint.empty() || cfg.txId.empty() || cfg.rxId.empty()) {
                std::cerr << (err.empty() ? "ERR connect 参数不完整" : err) << std::endl;
                continue;
            }

            if (!adapter_->configure(cfg)) {
                std::cerr << "ERR 配置应用失败" << std::endl;
                continue;
            }
            configured_ = true;
            std::cout << "OK 已连接 endpoint=" << cfg.endpoint
                      << " tx=" << cfg.txNs << ":" << cfg.txId
                      << " rx=" << cfg.rxNs << ":" << cfg.rxId << std::endl;
            continue;
        }

        if (cmdLower == "disconnect") {
            adapter_->setConnectEnabled(false);
            configured_ = false;
            std::cout << "OK 已断开" << std::endl;
            continue;
        }

        // 普通业务消息：发送到 TSN。
        if (!adapter_->send(line)) {
            std::cerr << "发送失败" << std::endl;
        }
    }
}
