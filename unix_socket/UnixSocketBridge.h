#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

// Unix Socket 桥接器：负责本地 Unix Socket 与 TSN(open62541 适配层)之间的消息转发。
// 约定：
// - CFG 配置行用于运行时设置 UA 连接参数。
// - 普通消息默认视为上行数据，写入 TSN。
class UnixSocketBridge {
public:
    using TsnSendFunc = std::function<bool(const std::string &payload)>;
    using TsnRecvSubscribeFunc = std::function<void(std::function<void(const std::string &)> onMsg)>;
    using ConfigFunc = std::function<bool(const std::string &line, std::string &reply)>;

    explicit UnixSocketBridge(std::string socketPath);
    ~UnixSocketBridge();

    // 设置上行：UnixSocket -> TSN
    void setTsnSendFunc(TsnSendFunc fn);

    // 设置下行订阅：TSN -> UnixSocket
    void setTsnRecvSubscribeFunc(TsnRecvSubscribeFunc fn);

    // 处理运行时配置行（例如 CFG endpoint=... tx=... rx=...）。
    // reply 用于回执给客户端（OK/ERR + 详情）。
    void setConfigFunc(ConfigFunc fn);

    // 启动/停止 Socket 服务。
    bool start();
    void stop();

private:
    // 主线程：accept 新连接并串行处理。
    void serverLoop();
    // 处理单个客户端连接（读配置/转发消息）。
    void handleClient(int clientFd);
    // 可靠写入：确保写完所有字节。
    bool writeAll(int fd, const char *buf, size_t len);

private:
    std::string socketPath_;
    int serverFd_{-1};
    int currentClientFd_{-1};

    std::atomic<bool> running_{false};
    std::thread serverThread_;

    TsnSendFunc tsnSendFunc_;
    TsnRecvSubscribeFunc tsnRecvSubscribeFunc_;
    ConfigFunc configFunc_;
};
