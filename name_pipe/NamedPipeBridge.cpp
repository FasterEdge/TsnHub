#include "name_pipe/NamedPipeBridge.h"

#include <cstring>
#include <iostream>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr size_t kBufferSize = 4096;
// 回写客户端时需要互斥，避免与关闭连接并发。
std::mutex gPipeWriteMutex;

#ifdef _WIN32
std::string normalizePipeName(const std::string &name) {
    const std::string prefix = "\\\\.\\pipe\\";
    if (name.rfind(prefix, 0) == 0) {
        return name;
    }
    return prefix + name;
}
#endif
} // namespace

NamedPipeBridge::NamedPipeBridge(std::string pipeName)
    : pipeName_(std::move(pipeName)) {}

NamedPipeBridge::~NamedPipeBridge() {
    stop();
}

void NamedPipeBridge::setTsnSendFunc(TsnSendFunc fn) {
    tsnSendFunc_ = std::move(fn);
}

void NamedPipeBridge::setTsnRecvSubscribeFunc(TsnRecvSubscribeFunc fn) {
    tsnRecvSubscribeFunc_ = std::move(fn);
}

void NamedPipeBridge::setConfigFunc(ConfigFunc fn) {
    configFunc_ = std::move(fn);
}

bool NamedPipeBridge::start() {
    if (running_.load()) {
        return true;
    }

#ifndef _WIN32
    std::cerr << "[NamedPipeBridge] 当前平台不支持 Windows NamedPipe。" << std::endl;
    return false;
#else
    running_.store(true);
    serverThread_ = std::thread(&NamedPipeBridge::serverLoop, this);
    std::cout << "[NamedPipeBridge] 已启动, 名称: " << pipeName_ << std::endl;
    return true;
#endif
}

void NamedPipeBridge::stop() {
    if (!running_.exchange(false)) {
        return;
    }

#ifdef _WIN32
    // 尝试中断阻塞 I/O 并关闭句柄。
    HANDLE h = static_cast<HANDLE>(pipeHandle_);
    if (h != INVALID_HANDLE_VALUE) {
        CancelIoEx(h, nullptr);
        DisconnectNamedPipe(h);
        CloseHandle(h);
        pipeHandle_ = reinterpret_cast<void *>(-1);
    }
#endif

    if (serverThread_.joinable()) {
        serverThread_.join();
    }

    std::cout << "[NamedPipeBridge] 已停止" << std::endl;
}

void NamedPipeBridge::serverLoop() {
#ifndef _WIN32
    return;
#else
    // 注册 TSN 下行回调：收到 TSN 消息后写回当前客户端。
    if (tsnRecvSubscribeFunc_) {
        tsnRecvSubscribeFunc_([this](const std::string &msg) {
            std::lock_guard<std::mutex> lk(gPipeWriteMutex);
            HANDLE h = static_cast<HANDLE>(pipeHandle_);
            if (h == INVALID_HANDLE_VALUE) {
                return;
            }
            std::string out = msg;
            if (out.empty() || out.back() != '\n') {
                out.push_back('\n');
            }
            DWORD written = 0;
            WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
        });
    }

    const std::string fullName = normalizePipeName(pipeName_);

    while (running_.load()) {
        // 创建命名管道实例（单客户端）。
        HANDLE hPipe = CreateNamedPipeA(
            fullName.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            kBufferSize,
            kBufferSize,
            0,
            nullptr
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            std::cerr << "[NamedPipeBridge] CreateNamedPipe 失败, error=" << GetLastError() << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        pipeHandle_ = hPipe;

        // 等待客户端连接。
        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(hPipe);
            pipeHandle_ = reinterpret_cast<void *>(-1);
            continue;
        }

        // 连接成功后进入读写处理。
        handleClient();

        // 连接断开后清理。
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
        pipeHandle_ = reinterpret_cast<void *>(-1);
    }
#endif
}

void NamedPipeBridge::handleClient() {
#ifndef _WIN32
    return;
#else
    HANDLE h = static_cast<HANDLE>(pipeHandle_);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    char buf[kBufferSize];
    while (running_.load()) {
        DWORD readBytes = 0;
        BOOL ok = ReadFile(h, buf, static_cast<DWORD>(sizeof(buf)), &readBytes, nullptr);
        if (!ok || readBytes == 0) {
            break;
        }

        std::string msg(buf, buf + readBytes);

        // 约定配置行格式：
        // CFG endpoint=opc.tcp://127.0.0.1:4840 tx=2:TsnTx rx=2:TsnRx
        // 若识别为配置行，则优先处理并回执给客户端。
        if (configFunc_ && msg.rfind("CFG ", 0) == 0) {
            std::string reply;
            bool cfgOk = configFunc_(msg, reply);
            if (reply.empty()) {
                reply = cfgOk ? "OK" : "ERR";
            }
            if (reply.back() != '\n') {
                reply.push_back('\n');
            }
            DWORD written = 0;
            WriteFile(h, reply.data(), static_cast<DWORD>(reply.size()), &written, nullptr);
            continue;
        }

        // 普通业务消息：转发到 TSN 侧。
        if (tsnSendFunc_) {
            bool sendOk = tsnSendFunc_(msg);
            if (!sendOk) {
                std::cerr << "[NamedPipeBridge] 转发到 TSN 失败" << std::endl;
            }
        } else {
            // 未绑定 TSN 发送函数时，回显用于调试。
            std::string echo = "[echo-no-tsn] " + msg;
            DWORD written = 0;
            WriteFile(h, echo.data(), static_cast<DWORD>(echo.size()), &written, nullptr);
        }
    }
#endif
}
