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
}

bool CommandLineBridge::configure(const PubSubConfig &cfg, std::string &err) {
    if (cfg.address.empty()) {
        err = "ERR 缺少必要参数 address";
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
            PubSubConfig cfg;
            std::string token;
            std::string err;
            while (iss >> token) {
                if (token.rfind("role=", 0) == 0) {
                    cfg.role = toLower(token.substr(5)) == "publisher" ? PubSubRole::Publisher :
                                (toLower(token.substr(5)) == "subscriber" ? PubSubRole::Subscriber : PubSubRole::Both);
                } else if (token.rfind("address=", 0) == 0) {
                    cfg.address = token.substr(8);
                } else if (token.rfind("publisherId=", 0) == 0) {
                    cfg.publisherId = static_cast<uint16_t>(std::stoi(token.substr(12)));
                } else if (token.rfind("writerId=", 0) == 0) {
                    cfg.writerId = static_cast<uint16_t>(std::stoi(token.substr(9)));
                } else if (token.rfind("readerId=", 0) == 0) {
                    cfg.readerId = static_cast<uint16_t>(std::stoi(token.substr(9)));
                } else if (token.rfind("interval=", 0) == 0) {
                    cfg.publishIntervalMs = static_cast<uint32_t>(std::stoul(token.substr(9)));
                } else if (token.rfind("field=", 0) == 0) {
                    cfg.fieldName = token.substr(6);
                }
            }

            if (cfg.address.empty()) {
                std::cerr << "ERR connect 需要 address" << std::endl;
                continue;
            }

            if (!adapter_->configure(cfg)) {
                std::cerr << "ERR 配置应用失败" << std::endl;
                continue;
            }
            configured_ = true;
            std::cout << "OK 已连接 role=" << (cfg.role == PubSubRole::Publisher ? "publisher" : (cfg.role == PubSubRole::Subscriber ? "subscriber" : "both"))
                      << " address=" << cfg.address
                      << " pubId=" << cfg.publisherId
                      << " wrId=" << cfg.writerId
                      << " rdId=" << cfg.readerId
                      << " interval=" << cfg.publishIntervalMs
                      << " field=" << cfg.fieldName << std::endl;
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
