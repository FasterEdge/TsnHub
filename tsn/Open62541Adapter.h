#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <open62541/client.h>

struct Open62541RuntimeConfig {
    // OPC UA 服务器地址
    std::string endpoint = "opc.tcp://127.0.0.1:4840";
    // 上行写入节点（字符串 NodeId）
    uint16_t txNs = 2;
    std::string txId = "TsnTx";
    // 下行订阅节点（字符串 NodeId）
    uint16_t rxNs = 2;
    std::string rxId = "TsnRx";
};

// open62541 适配器：封装 TSN 消息发送与订阅接口（真实 UA 客户端实现）。
class Open62541Adapter {
public:
    Open62541Adapter();
    ~Open62541Adapter();

    bool start();
    void stop();

    // 运行时配置（可在运行中调用，将触发重连）。
    bool configure(const Open62541RuntimeConfig &cfg);
    // 可选：手工启停连接（UnixSocket/NamedPipe 默认不连，收到 CFG 后才连）。
    void setConnectEnabled(bool enabled);

    // 上行：UnixSocket -> TSN（写入 tx 节点）。
    bool send(const std::string &payload);

    // 下行：TSN -> UnixSocket（订阅 rx 节点并回调）。
    void subscribe(std::function<void(const std::string &)> onMsg);

private:
    // UA 订阅回调：收到数据变更时触发。
    static void dataChangeCallback(UA_Client *client,
                                   UA_UInt32 subId,
                                   void *subContext,
                                   UA_UInt32 monId,
                                   void *monContext,
                                   UA_DataValue *value);
    // 连接并创建订阅（内部加锁版本）。
    bool connectAndSubscribeLocked();
    // 断开连接并释放资源（内部加锁版本）。
    void disconnectLocked();
    // 后台循环：run_iterate + 重连逻辑。
    void loop();

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> reconfigureRequested_{false};
    std::atomic<bool> connectEnabled_{false};
    std::thread clientLoopThread_;

    std::mutex mu_;
    Open62541RuntimeConfig cfg_;
    std::function<void(const std::string &)> onMsg_;

    struct UA_Client *client_{nullptr};
    uint32_t subId_{0};
};
