#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

// Named Pipe 桥接器：负责 Windows NamedPipe 与 TSN(open62541 适配层)之间的消息转发。
// 约定与 UnixSocketBridge 一致：
// - CFG 配置行用于运行时设置 UA 连接参数。
// - 普通消息默认视为上行数据，写入 TSN。
class NamedPipeBridge {
public:
    using TsnSendFunc = std::function<bool(const std::string &payload)>;
    using TsnRecvSubscribeFunc = std::function<void(std::function<void(const std::string &)> onMsg)>;
    using ConfigFunc = std::function<bool(const std::string &line, std::string &reply)>;

    explicit NamedPipeBridge(std::string pipeName);
    ~NamedPipeBridge();

    // 设置上行：NamedPipe -> TSN
    void setTsnSendFunc(TsnSendFunc fn);

    // 设置下行订阅：TSN -> NamedPipe
    void setTsnRecvSubscribeFunc(TsnRecvSubscribeFunc fn);

    // 处理运行时配置行（例如 CFG endpoint=... tx=... rx=...）。
    // reply 用于回执给客户端（OK/ERR + 详情）。
    void setConfigFunc(ConfigFunc fn);

    // 启动/停止 NamedPipe 服务。
    bool start();
    void stop();

private:
    // 主线程：等待客户端连接并串行处理。
    void serverLoop();
    // 处理单个客户端连接（读配置/转发消息）。
    void handleClient();

private:
    std::string pipeName_;

    std::atomic<bool> running_{false};
    std::thread serverThread_;

    TsnSendFunc tsnSendFunc_;
    TsnRecvSubscribeFunc tsnRecvSubscribeFunc_;
    ConfigFunc configFunc_;

#ifdef _WIN32
    void *pipeHandle_{reinterpret_cast<void *>(-1)};
#endif
};
