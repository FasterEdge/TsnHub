//
// https://github.com/FasterEdge
// 由 tyza66 于 2026/3/21 创建。
//
#include <csignal>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>
#include "tsn/Open62541Adapter.h"
#include "unix_socket/UnixSocketBridge.h"
#include "name_pipe/NamedPipeBridge.h"
#include "command_line/CommandLineBridge.h"

#ifdef _WIN32 // 这个宏在Windows上才生效
#include <windows.h>
#endif

// 这里放一些全局工具函数和变量，避免污染业务逻辑。
namespace {
    volatile std::sig_atomic_t gStop = 0;

    // 信号处理函数中只做最小工作：设置退出标志。
    void onSignal(int) {
        gStop = 1;
    }

#ifdef _WIN32
    // Windows 控制台退出处理，接收 Ctrl+C/关闭窗口等信号。
BOOL WINAPI consoleHandler(DWORD) {
        gStop = 1;
        return TRUE;
    }
#endif

    // 为 Unix Socket 路径创建父目录，避免 bind 时因目录不存在失败。
    bool ensureSocketParentDir(const std::string &socketPath) {
        namespace fs = std::filesystem;
        try {
            fs::path p(socketPath);
            fs::path parent = p.parent_path();
            if (parent.empty()) {
                return true;
            }
            if (fs::exists(parent)) {
                return true;
            }
            return fs::create_directories(parent);
        } catch (const std::exception &e) {
            std::cerr << "创建 socket 父目录失败: " << e.what() << std::endl;
            return false;
        }
    }

    // 统一转小写，方便模式判断。
    std::string toLower(std::string s) {
        for (char &ch: s) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return s;
    }

    // 解析 "ns:id" 形式。
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

    // PubSub 参数（UDP UADP）。
    std::string roleStr = "both"; // publisher|subscriber|both
    std::string address = "opc.udp://224.0.0.22:4840";
    uint16_t publisherId = 1001;
    uint16_t writerId = 62541;
    uint16_t readerId = 7001;
    uint32_t pubInterval = 100;
    std::string fieldName = "tsnPayload";

    auto parseRole = [](const std::string &v) -> PubSubRole {
        std::string l = toLower(v);
        if (l == "publisher") return PubSubRole::Publisher;
        if (l == "subscriber") return PubSubRole::Subscriber;
        return PubSubRole::Both;
    };
}

int main(int argc, char **argv) {
    // 创建CLI体系，并设置描述信息。
    CLI::App app{"TsnHub for many scenes by FasterEdge"};

    // mode：Windows 默认 NamedPipe，非 Windows 默认 UnixSocket。
#ifdef _WIN32
    std::string mode = "NamedPipe";
    // addr：Windows 命名管道名称，使用 raw string 避免转义。
    std::string addr = R"(\\.\pipe\tsn_hub_service)";
#else
    std::string mode = "UnixSocket";
    // addr：Unix 域套接字路径。
    std::string addr = "/var/run/tsn_hub_service.sock";
#endif

    // 限制 mode 取值范围，避免非法参数进入业务逻辑。
    // UnixSocket   使用 Unix 域套接字进行本地通信，适用于 Linux/macOS。
    // NamedPipe    专用于 Windows 的命名管道通信机制，适用于 Windows。
    // CommandLine  专用于命令行交互，适用于调试或无本地通信需求的场景（若作为子进程模式管理，则可借以交互）。
    std::vector<std::string> modeChoices = {"UnixSocket", "NamedPipe", "CommandLine"};

    // 添加命令行参数定义
    app.add_option("-m,--mode", mode, "Mode: UnixSocket (default) or NamedPipe or CommandLine")
            ->check(CLI::IsMember(modeChoices, CLI::ignore_case));
    app.add_option("-a,--addr", addr, "Unix socket path or Windows named pipe name");

    // CommandLine 模式参数：通过 CLI 设置 UA 端点与节点（可选）。
    app.add_option("--role", roleStr, "PubSub role: publisher/subscriber/both (default both)");
    app.add_option("--address", address, "UADP UDP address, e.g. opc.udp://239.0.0.1:4840");
    app.add_option("--publisher-id", publisherId, "PublisherId (uint16)");
    app.add_option("--writer-id", writerId, "WriterId (uint16)");
    app.add_option("--reader-id", readerId, "ReaderId (uint16)");
    app.add_option("--interval", pubInterval, "Publish interval ms");
    app.add_option("--field", fieldName, "Single string field name");

    try {
        // 解析命令行参数，若有错误会抛出异常。
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        // CLI11 会输出错误信息与帮助文本，并返回合适的退出码。
        return app.exit(e);
    }

    // 输出配置信息，便于日志排查。
    std::cout << "TsnHub for many scenes by FasterEdge" << std::endl;
    std::cout << "Mode: " << mode << std::endl;
    std::cout << "Addr: " << addr << std::endl;

    // 统一模式小写，避免大小写分支重复。
    const std::string modeLower = toLower(mode);
    const bool isUnixSocket = (modeLower == "unixsocket");
    const bool isNamedPipe = (modeLower == "namedpipe");
    const bool isCommandLine = (modeLower == "commandline");

    if (isUnixSocket) {
        std::cout << "Starting Unix Socket server at: " << addr << std::endl;
    } else if (isNamedPipe) {
        std::cout << "Starting Named Pipe server with name: " << addr << std::endl;
    } else if (isCommandLine) {
        std::cout << "Starting CommandLine mode" << std::endl;
    } else {
        // 理论上不会进入此分支（已由 CLI11 校验）。
        std::cerr << "Invalid mode: " << mode << std::endl;
        return 1;
    }

    // 确保 Unix Socket 的父目录存在，避免 bind 失败。
    if (isUnixSocket) {
        if (!ensureSocketParentDir(addr)) {
            return 5;
        }
    }

#ifdef _WIN32
    // Windows 控制台退出处理。
    SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
    // 注册退出信号，支持 Ctrl+C 或系统终止。
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#endif

    // 启动 open62541 适配层：负责 TSN 发送与订阅（PubSub）。
    Open62541Adapter tsnAdapter;
    if (!tsnAdapter.start()) {
        std::cerr << "启动 Open62541Adapter 失败" << std::endl;
        return 3;
    }

    // CommandLine 模式：通过命令行参数配置 PubSub，并用 stdin/stdout 交互。
    if (isCommandLine) {
        CommandLineBridge bridge;
        bridge.bindAdapter(&tsnAdapter);

        // 若提供了完整参数，则预配置；否则等待 connect 指令。
        if (!roleStr.empty() && !address.empty()) {
            PubSubConfig cfg;
            cfg.role = parseRole(roleStr);
            cfg.address = address;
            cfg.publisherId = publisherId;
            cfg.writerId = writerId;
            cfg.readerId = readerId;
            cfg.publishIntervalMs = pubInterval;
            cfg.fieldName = fieldName;
            std::string err;
            if (!bridge.configure(cfg, err)) {
                std::cerr << err << std::endl;
                tsnAdapter.stop();
                return 6;
            }
        } else {
            std::cout << "CommandLine 模式：未提供 PubSub 参数，等待 connect 指令" << std::endl;
        }

        bridge.run(&gStop);

        tsnAdapter.stop();
        std::cout << "TsnHub is stopping... " << addr << std::endl;
        return 0;
    }

    // 解析并应用运行时配置的回调（供 UnixSocket / NamedPipe 复用）。
    auto configHandler = [&tsnAdapter](const std::string &line, std::string &reply) {
        // 配置格式：CFG role=publisher address=opc.udp://239.0.0.1:4840 publisherId=1001 writerId=62541 readerId=7001 interval=100 field=name
        PubSubConfig cfg;
        std::istringstream iss(line);
        std::string token;
        iss >> token; // CFG
        while (iss >> token) {
            if (token.rfind("role=", 0) == 0) {
                cfg.role = parseRole(token.substr(5));
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
            reply = "ERR 缺少 address";
            return false;
        }

        bool ok = tsnAdapter.configure(cfg);
        if (!ok) {
            reply = "ERR 配置应用失败";
            return false;
        }

        reply = "OK role=" + std::string(cfg.role == PubSubRole::Publisher ? "publisher" : (cfg.role == PubSubRole::Subscriber ? "subscriber" : "both")) +
                " address=" + cfg.address +
                " pubId=" + std::to_string(cfg.publisherId) +
                " wrId=" + std::to_string(cfg.writerId) +
                " rdId=" + std::to_string(cfg.readerId) +
                " interval=" + std::to_string(cfg.publishIntervalMs) +
                " field=" + cfg.fieldName;
        return true;
    };

    if (isUnixSocket) {
        // 启动 Unix Socket 桥接层：负责本地 socket 与 TSN 的数据转发。
        UnixSocketBridge bridge(addr);
        bridge.setTsnSendFunc([&tsnAdapter](const std::string &msg) {
            // 上行：UnixSocket -> TSN
            return tsnAdapter.send(msg);
        });
        bridge.setTsnRecvSubscribeFunc([&tsnAdapter](std::function<void(const std::string &)> onMsg) {
            // 下行：TSN -> UnixSocket
            tsnAdapter.subscribe(std::move(onMsg));
        });
        bridge.setConfigFunc(configHandler);

        if (!bridge.start()) {
            std::cerr << "启动 UnixSocketBridge 失败" << std::endl;
            tsnAdapter.stop();
            return 4;
        }

        // 主线程驻留，避免忙等；收到退出信号后进入清理流程。
        while (!gStop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        // 退出时按顺序停止：先停 socket，再停 TSN 适配层。
        bridge.stop();
        tsnAdapter.stop();
    } else if (isNamedPipe) {
#ifdef _WIN32
        // 启动 Windows NamedPipe 桥接层。
        NamedPipeBridge bridge(addr);
        bridge.setTsnSendFunc([&tsnAdapter](const std::string &msg) {
            // 上行：NamedPipe -> TSN
            return tsnAdapter.send(msg);
        });
        bridge.setTsnRecvSubscribeFunc([&tsnAdapter](std::function<void(const std::string &)> onMsg) {
            // 下行：TSN -> NamedPipe
            tsnAdapter.subscribe(std::move(onMsg));
        });
        bridge.setConfigFunc(configHandler);

        if (!bridge.start()) {
            std::cerr << "启动 NamedPipeBridge 失败" << std::endl;
            tsnAdapter.stop();
            return 4;
        }

        // 主线程驻留，避免忙等；收到退出信号后进入清理流程。
        while (!gStop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        bridge.stop();
        tsnAdapter.stop();
#else
        std::cerr << "当前平台不支持 Windows NamedPipe 模式。" << std::endl;
        tsnAdapter.stop();
        return 2;
#endif
    }

    std::cout << "TsnHub is stopping... " << addr << std::endl;

    return 0;
}
