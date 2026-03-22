#include "tsn/Open62541Adapter.h"

#include <chrono>
#include <iostream>

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/client_subscriptions.h>

namespace {
UA_NodeId makeStringNodeId(uint16_t ns, const std::string &id) {
    return UA_NODEID_STRING_ALLOC(ns, const_cast<char *>(id.c_str()));
}

void clearNodeId(UA_NodeId *id) {
    UA_NodeId_clear(id);
}

std::string variantToString(const UA_Variant *v) {
    if (!v || !UA_Variant_isScalar(v) || !v->data) {
        return "";
    }
    if (v->type == &UA_TYPES[UA_TYPES_STRING]) {
        const auto *s = static_cast<const UA_String *>(v->data);
        return std::string(reinterpret_cast<const char *>(s->data), s->length);
    }
    if (v->type == &UA_TYPES[UA_TYPES_BYTESTRING]) {
        const auto *s = static_cast<const UA_ByteString *>(v->data);
        return std::string(reinterpret_cast<const char *>(s->data), s->length);
    }
    return "";
}
}

Open62541Adapter::Open62541Adapter() = default;

Open62541Adapter::~Open62541Adapter() {
    stop();
}

bool Open62541Adapter::configure(const Open62541RuntimeConfig &cfg) {
    {
        // 更新配置并触发重连。
        std::lock_guard<std::mutex> lk(mu_);
        cfg_ = cfg;
    }
    connectEnabled_.store(true);
    if (running_.load()) {
        reconfigureRequested_.store(true);
    }
    return true;
}

void Open62541Adapter::setConnectEnabled(bool enabled) {
    if (!enabled) {
        std::lock_guard<std::mutex> lk(mu_);
        disconnectLocked();
    }
    connectEnabled_.store(enabled);
}

void Open62541Adapter::subscribe(std::function<void(const std::string &)> onMsg) {
    std::lock_guard<std::mutex> lk(mu_);
    onMsg_ = std::move(onMsg);
}

bool Open62541Adapter::start() {
    if (running_.load()) {
        return true;
    }
    // 启动后台线程，负责连接、订阅与 run_iterate 循环。
    running_.store(true);
    clientLoopThread_ = std::thread(&Open62541Adapter::loop, this);
    std::cout << "[Open62541Adapter] 已启动真实 UA 客户端循环" << std::endl;
    return true;
}

void Open62541Adapter::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (clientLoopThread_.joinable()) {
        clientLoopThread_.join();
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        disconnectLocked();
    }
    std::cout << "[Open62541Adapter] 已停止" << std::endl;
}

bool Open62541Adapter::connectAndSubscribeLocked() {
    // 先清理旧连接。
    disconnectLocked();

    client_ = UA_Client_new();
    if (!client_) {
        std::cerr << "[Open62541Adapter] UA_Client_new 失败" << std::endl;
        return false;
    }
    UA_ClientConfig_setDefault(UA_Client_getConfig(client_));

    const std::string endpoint = cfg_.endpoint;
    UA_StatusCode rc = UA_Client_connect(client_, endpoint.c_str());
    if (rc != UA_STATUSCODE_GOOD) {
        std::cerr << "[Open62541Adapter] 连接失败: " << endpoint
                  << ", code=0x" << std::hex << rc << std::dec << std::endl;
        disconnectLocked();
        return false;
    }

    // 创建订阅，用于监听 rx 节点变更。
    UA_CreateSubscriptionRequest req = UA_CreateSubscriptionRequest_default();
    UA_CreateSubscriptionResponse resp = UA_Client_Subscriptions_create(client_, req, nullptr, nullptr, nullptr);
    if (resp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        std::cerr << "[Open62541Adapter] 创建订阅失败, code=0x" << std::hex
                  << resp.responseHeader.serviceResult << std::dec << std::endl;
        disconnectLocked();
        return false;
    }
    subId_ = resp.subscriptionId;

    // 为 rx 节点创建监控项，接收数据变更通知。
    UA_NodeId rxNode = makeStringNodeId(cfg_.rxNs, cfg_.rxId);
    UA_MonitoredItemCreateRequest monReq = UA_MonitoredItemCreateRequest_default(rxNode);
    UA_MonitoredItemCreateResult monResp = UA_Client_MonitoredItems_createDataChange(
        client_,
        subId_,
        UA_TIMESTAMPSTORETURN_BOTH,
        monReq,
        this,
        Open62541Adapter::dataChangeCallback,
        nullptr
    );
    clearNodeId(&rxNode);

    if (monResp.statusCode != UA_STATUSCODE_GOOD) {
        std::cerr << "[Open62541Adapter] 创建监控项失败, code=0x" << std::hex
                  << monResp.statusCode << std::dec << std::endl;
        disconnectLocked();
        return false;
    }

    std::cout << "[Open62541Adapter] 连接并订阅成功 endpoint=" << endpoint
              << " rx=ns=" << cfg_.rxNs << ";s=" << cfg_.rxId << std::endl;
    return true;
}

void Open62541Adapter::disconnectLocked() {
    if (client_) {
        UA_Client_disconnect(client_);
        UA_Client_delete(client_);
        client_ = nullptr;
    }
    subId_ = 0;
}

void Open62541Adapter::loop() {
    while (running_.load()) {
        // 未收到连接指令时，保持空转等待。
        if (!connectEnabled_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        bool connected = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            connected = connectAndSubscribeLocked();
        }

        // 连接失败时等待后重试。
        if (!connected) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        while (running_.load()) {
            // 收到配置更新时，触发重连。
            if (reconfigureRequested_.exchange(false)) {
                std::lock_guard<std::mutex> lk(mu_);
                disconnectLocked();
                break;
            }
            // 外部关闭连接开关时，立即断开并等待下一次开启。
            if (!connectEnabled_.load()) {
                std::lock_guard<std::mutex> lk(mu_);
                disconnectLocked();
                break;
            }

            UA_Client *localClient = nullptr;
            {
                std::lock_guard<std::mutex> lk(mu_);
                localClient = client_;
            }
            if (!localClient) {
                break;
            }

            // run_iterate 驱动 UA 客户端处理订阅与 keepalive。
            UA_StatusCode rc = UA_Client_run_iterate(localClient, 100);
            if (rc != UA_STATUSCODE_GOOD && rc != UA_STATUSCODE_GOODNONCRITICALTIMEOUT) {
                std::cerr << "[Open62541Adapter] run_iterate 异常, code=0x" << std::hex
                          << rc << std::dec << ", 准备重连" << std::endl;
                std::lock_guard<std::mutex> lk(mu_);
                disconnectLocked();
                break;
            }
        }
    }
}

bool Open62541Adapter::send(const std::string &payload) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!connectEnabled_.load()) {
        std::cerr << "[Open62541Adapter] send 失败：连接未开启" << std::endl;
        return false;
    }
    if (!client_) {
        std::cerr << "[Open62541Adapter] send 失败：客户端未连接" << std::endl;
        return false;
    }

    // 将字符串写入 tx 节点。
    UA_NodeId txNode = makeStringNodeId(cfg_.txNs, cfg_.txId);

    UA_String uaStr = UA_STRING_ALLOC(payload.c_str());
    UA_Variant v;
    UA_Variant_init(&v);
    UA_Variant_setScalar(&v, &uaStr, &UA_TYPES[UA_TYPES_STRING]);

    UA_StatusCode rc = UA_Client_writeValueAttribute(client_, txNode, &v);

    UA_String_clear(&uaStr);
    clearNodeId(&txNode);

    if (rc != UA_STATUSCODE_GOOD) {
        std::cerr << "[Open62541Adapter] 写入失败 tx=ns=" << cfg_.txNs << ";s=" << cfg_.txId
                  << ", code=0x" << std::hex << rc << std::dec << std::endl;
        return false;
    }
    return true;
}

void Open62541Adapter::dataChangeCallback(UA_Client *client,
                                          UA_UInt32 subId,
                                          void *subContext,
                                          UA_UInt32 monId,
                                          void *monContext,
                                          UA_DataValue *value) {
    (void)client;
    (void)subId;
    (void)subContext;
    (void)monId;

    auto *self = static_cast<Open62541Adapter *>(monContext);
    if (!self || !value || !value->hasValue) {
        return;
    }

    std::string msg = variantToString(&value->value);
    if (msg.empty()) {
        return;
    }

    std::function<void(const std::string &)> cb;
    {
        std::lock_guard<std::mutex> lk(self->mu_);
        cb = self->onMsg_;
    }
    if (cb) {
        cb(msg);
    }
}
