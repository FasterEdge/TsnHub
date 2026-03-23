#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/plugin/log_stdout.h>
#include <open62541/networkmessage.h>

// 运行角色：发布者 / 订阅者 / 双角色。
enum class PubSubRole { Publisher, Subscriber, Both };

struct PubSubConfig {
    // 角色：缺省 Both。
    PubSubRole role = PubSubRole::Both;
    // 传输地址（UADP over UDP），如 opc.udp://224.0.0.22:4840 或 opc.udp://239.0.0.1:4840。
    std::string address = "opc.udp://224.0.0.22:4840";
    // PublisherId / WriterId / ReaderId。
    uint16_t publisherId = 1001;
    uint16_t writerId = 62541;
    uint16_t readerId = 7001;
    // 发布周期（ms）。
    uint32_t publishIntervalMs = 100;
    // 数据集字段名，单字段字符串。
    std::string fieldName = "tsnPayload";
};

// open62541 PubSub 适配器（UDP，单字符串字段）。
class Open62541Adapter {
public:
    Open62541Adapter();
    ~Open62541Adapter();

    bool start();
    void stop();

    // 运行时配置（触发重建 PubSub 配置）。
    bool configure(const PubSubConfig &cfg);
    // 开关：允许/停止 PubSub 运行。
    void setConnectEnabled(bool enabled);

    // 发布：将 payload 写入数据集（仅当角色包含 Publisher）。
    bool send(const std::string &payload);

    // 订阅：注册下行回调（仅当角色包含 Subscriber）。
    void subscribe(std::function<void(const std::string &)> onMsg);

private:
    bool setupPubSubLocked();
    void teardownLocked();
    void loop();

    // Subscriber 回调。
    static void readerDataSetListener(UA_Server *server, UA_UInt32 readerId,
                                      void *readerContext, const UA_ByteString *msg,
                                      const UA_NetworkMessage *nm);

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> reconfigureRequested_{false};
    std::atomic<bool> connectEnabled_{false};
    std::thread serverThread_;

    std::mutex mu_;
    PubSubConfig cfg_{};
    std::function<void(const std::string &)> onMsg_;

    UA_Server *server_{nullptr};
    UA_NodeId publishedDataSet_{UA_NODEID_NULL};
    UA_NodeId writerGroup_{UA_NODEID_NULL};
    UA_NodeId dataSetWriter_{UA_NODEID_NULL};
    UA_NodeId connection_{UA_NODEID_NULL};
    UA_NodeId readerGroup_{UA_NODEID_NULL};
    UA_NodeId dataSetReader_{UA_NODEID_NULL};
    UA_NodeId payloadVar_{UA_NODEID_NULL};
};
