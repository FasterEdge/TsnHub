// FasterEdge 开源项目 · https://github.com/FasterEdge · https://gitee.com/FasterEdge
#include "tsn/SimulatorNode.hpp"
#ifdef TSNHUB_USE_OPEN62541_PUBSUB
#include "pubsub/Open62541Bridge.hpp"
#endif

#include <CLI/CLI.hpp>
#include <open62541/config.h>
#include "tsn/StatsReport.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic_bool running{true};
void stopHandler(int) { running.store(false); }

tsnhub::UdpEndpoint parseEndpoint(const std::string &value) {
    const auto separator = value.rfind(':');
    if(separator == std::string::npos || separator == 0 || separator + 1 >= value.size())
        throw std::invalid_argument("endpoint must be host:port");
    const auto port = std::stoul(value.substr(separator + 1));
    if(port == 0 || port > 65535)
        throw std::invalid_argument("endpoint port must be in 1..65535");
    return {value.substr(0, separator), static_cast<std::uint16_t>(port)};
}

tsnhub::GateSlot parseGate(const std::string &value) {
    const auto separator = value.find(':');
    if(separator == std::string::npos)
        throw std::invalid_argument("gate must be duration_us:mask");
    const auto duration = std::stoll(value.substr(0, separator));
    const auto mask = std::stoul(value.substr(separator + 1), nullptr, 0);
    if(duration <= 0 || mask > 0xff)
        throw std::invalid_argument("invalid gate duration or mask");
    return {std::chrono::microseconds{duration}, static_cast<std::uint8_t>(mask)};
}
} // namespace

int main(int argc, char **argv) {
    CLI::App app{"Cross-platform local TSN simulation hub"};
    std::string mode{"portable"};
    std::string input{"127.0.0.1:4841"};
    std::string output{"127.0.0.1:4842"};
    std::string subscribeUrl{"opc.udp://0.0.0.0:4841"};
    std::string publishUrl{"opc.udp://127.0.0.1:4842"};
    std::string publisherBindUrl{"opc.udp://0.0.0.0:4843"};
    std::uint16_t inputPublisherId = 1;
    std::uint16_t inputWriterGroupId = 1;
    std::uint16_t inputWriterId = 1;
    std::uint16_t outputPublisherId = 2;
    std::uint16_t outputWriterGroupId = 2;
    std::uint16_t outputWriterId = 2;
    double publishIntervalMs = 1.0;
    std::vector<std::string> priorityRules;
    std::string priorityMapFile;
    std::uint8_t inputDefaultPriority = 0;
    std::vector<std::string> gates;
    std::int64_t delayUs = 0;
    std::int64_t jitterUs = 0;
    double loss = 0.0;
    std::size_t queueSize = 1024;
    std::uint64_t seed = 1;
    double shapingMbps = 0.0;
    std::size_t shapingBurstBytes = 0;
    std::size_t creditFramesPerWindow = 0;
    std::string statsFormat = "summary";
    std::string statsFile;
    // 固定测量窗口: 0 表示一直运行到信号/管道关闭。Windows 的 MSYS kill 只做
    // TerminateProcess, 进程拿不到信号, 统计输出永远不会落盘; 给一个窗口就能
    // 让脚本化实验在没有信号通道的环境里同样拿到完整统计。
    std::uint64_t runMs = 0;
    bool showCapabilities = false;

    app.add_option("--mode", mode, "Transport mode: portable or native")->check(CLI::IsMember({"portable", "native"}));
    app.add_option("--listen", input, "Portable input UDP endpoint host:port");
    app.add_option("--forward", output, "Portable output UDP endpoint host:port");
    app.add_option("--subscribe-url", subscribeUrl, "Native PubSub input URL");
    app.add_option("--publish-url", publishUrl, "Native PubSub output URL");
    app.add_option("--publisher-bind-url", publisherBindUrl, "Native PubSub publisher local bind URL");
    app.add_option("--input-publisher-id", inputPublisherId, "Expected upstream PublisherId");
    app.add_option("--input-writer-group-id", inputWriterGroupId, "Expected upstream WriterGroupId");
    app.add_option("--input-writer-id", inputWriterId, "Expected upstream DataSetWriterId");
    app.add_option("--output-publisher-id", outputPublisherId, "TsnHub output PublisherId");
    app.add_option("--output-writer-group-id", outputWriterGroupId, "TsnHub output WriterGroupId");
    app.add_option("--output-writer-id", outputWriterId, "TsnHub output DataSetWriterId");
    app.add_option("--publish-interval-ms", publishIntervalMs, "Native PubSub publishing interval")->check(CLI::PositiveNumber);
    app.add_option("--priority-rule", priorityRules,
                   "Native PubSub priority rule publisher:writerGroup:writer:priority "
                   "(repeatable, '*' accepted as a wildcard)");
    app.add_option("--priority-map-file", priorityMapFile,
                   "File with one publisher:writerGroup:writer:priority rule per line, '#' comments");
    app.add_option("--input-default-priority", inputDefaultPriority,
                   "Priority used when no rule matches an inbound stream")
        ->check(CLI::Range(0, 7));
    app.add_option("--gate", gates, "Gate slot duration_us:mask (repeatable, mask accepts 0xff)");
    app.add_option("--delay-us", delayUs, "Base forwarding delay in microseconds")->check(CLI::NonNegativeNumber);
    app.add_option("--jitter-us", jitterUs, "Symmetric delay jitter in microseconds")->check(CLI::NonNegativeNumber);
    app.add_option("--loss", loss, "Simulated packet loss in [0,1]")->check(CLI::Range(0.0, 1.0));
    app.add_option("--queue", queueSize, "Maximum queued frames per priority")->check(CLI::PositiveNumber);
    app.add_option("--seed", seed, "Deterministic random seed");
    app.add_option("--shape-mbps", shapingMbps,
                   "Egress token-bucket shaping rate in Mbit/s (0 disables shaping)")
        ->check(CLI::NonNegativeNumber);
    app.add_option("--shape-burst-bytes", shapingBurstBytes,
                   "Token bucket depth in bytes (0 uses the 64 KiB default)")
        ->check(CLI::NonNegativeNumber);
    app.add_option("--credit-frames-per-window", creditFramesPerWindow,
                   "Maximum frames released per open GCL window (0 disables window pacing, "
                   "legacy drain-all behaviour)")
        ->check(CLI::NonNegativeNumber);
    app.add_option("--stats-format", statsFormat,
                   "Statistics output format: summary, table, csv or json")
        ->check(CLI::IsMember({"summary", "table", "csv", "json"}));
    app.add_option("--stats-file", statsFile,
                   "Write statistics to this file instead of stdout (requires a non-summary format)");
    app.add_option("--run-ms", runMs,
                   "Stop after this many milliseconds (0 runs until SIGINT/SIGTERM)")
        ->check(CLI::NonNegativeNumber);
    app.add_flag("--capabilities", showCapabilities, "Print capabilities and exit");
    CLI11_PARSE(app, argc, argv);

    if(showCapabilities) {
        std::cout << "open62541=" << UA_OPEN62541_VER_MAJOR << '.' << UA_OPEN62541_VER_MINOR << '.' << UA_OPEN62541_VER_PATCH << '\n';
#if defined(UA_ENABLE_PUBSUB) && defined(TSNHUB_USE_OPEN62541_PUBSUB)
        std::cout << "native_pubsub=true\n";
#else
        std::cout << "native_pubsub=false\n";
#endif
        std::cout << "portable_pubsub=true\nportable_scheduler=true\n";
        // 新增能力在运行时自报, 便于脚本核对构建产物与 README 宣称一致。
        std::cout << "latency_stats=true\negress_shaping=true\nstats_formats=summary,table,csv,json\n";
        return 0;
    }

    try {
        tsnhub::SchedulerConfig scheduler;
        scheduler.schedule.clear();
        if(gates.empty()) scheduler.schedule.push_back({std::chrono::microseconds{1000}, 0xff});
        else for(const auto &gate : gates) scheduler.schedule.push_back(parseGate(gate));
        scheduler.baseDelay = std::chrono::microseconds{delayUs};
        scheduler.jitter = std::chrono::microseconds{jitterUs};
        scheduler.lossRate = loss;
        scheduler.maxQueuePerClass = queueSize;
        scheduler.randomSeed = seed;
        scheduler.shapingRateMbps = shapingMbps;
        scheduler.shapingBurstBytes = shapingBurstBytes;
        scheduler.creditFramesPerWindow = creditFramesPerWindow;

        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        // 固定窗口由独立的定时线程置位, 与节点自身的运行循环解耦;
        // 窗口为 0 时不起线程, 行为与"运行到信号为止"完全一致。
        std::thread windowThread;
        if(runMs > 0) {
            // running 是命名空间作用域的静态对象, lambda 不能按引用捕获它,
            // 因此先把引用落到局部变量再捕获。
            auto &runFlag = running;
            windowThread = std::thread([runMs, &runFlag] {
                std::this_thread::sleep_for(std::chrono::milliseconds{runMs});
                runFlag.store(false);
            });
        }
        tsnhub::SchedulerStats stats;
        std::array<tsnhub::PrioritySummary, tsnhub::LatencyStats::priorityCount> latency{};
        if(mode == "native") {
#ifdef TSNHUB_USE_OPEN62541_PUBSUB
            tsnhub::NativePubSubConfig native;
            native.listenUrl = subscribeUrl;
            native.publishUrl = publishUrl;
            native.publisherBindUrl = publisherBindUrl;
            native.inputPublisherId = inputPublisherId;
            native.inputWriterGroupId = inputWriterGroupId;
            native.inputDataSetWriterId = inputWriterId;
            native.outputPublisherId = outputPublisherId;
            native.outputWriterGroupId = outputWriterGroupId;
            native.outputDataSetWriterId = outputWriterId;
            native.publishingIntervalMs = publishIntervalMs;
            // 规则与文件可同时给出，重复规则由 PriorityMap 判定为歧义并拒绝；
            // 装载期失败优于把错配的优先级静默带进实验。
            tsnhub::PriorityMap priorityMap;
            if(!priorityMapFile.empty()) {
                priorityMap = tsnhub::PriorityMap::loadFile(priorityMapFile);
                if(!priorityRules.empty())
                    for(const auto &rule : priorityRules) priorityMap.add(rule);
            } else {
                priorityMap = tsnhub::PriorityMap::parse(priorityRules);
            }
            priorityMap.setDefaultPriority(inputDefaultPriority);
            native.priorityMap = priorityMap;
            native.inputDefaultPriority = inputDefaultPriority;
            tsnhub::Open62541Bridge node(native, scheduler);
            // 容器/管道场景下 stdout 为全缓冲, 必须用 endl 立即冲刷,
            // 否则长驻循环的启动消息永远不进 docker logs。
            std::cout << "TsnHub native PubSub subscribing " << subscribeUrl
                      << ", publishing " << publishUrl << std::endl;
            std::cout << "TsnHub native PubSub priority default="
                      << static_cast<unsigned>(inputDefaultPriority) << " rules="
                      << priorityMap.ruleCount() << std::endl;
            for(const auto &rule : priorityMap.describe())
                std::cout << "TsnHub native PubSub priority rule " << rule << std::endl;
            node.run(running);
            stats = node.stats();
            latency = node.latencySummaries();
#else
            throw std::runtime_error("native PubSub is not enabled in this build");
#endif
        } else {
            const auto listen = parseEndpoint(input);
            const auto forward = parseEndpoint(output);
            tsnhub::SimulatorNode node(scheduler, listen, forward);
            std::cout << "TsnHub portable mode listening on " << input
                      << ", forwarding to " << output << std::endl;
            node.run(running);
            stats = node.stats();
            latency = node.latencySummaries();
        }
        auto report = tsnhub::makeRunReport(stats, latency);
        {
            std::ostringstream note;
            note << "mode=" << mode << " delay_us=" << delayUs << " jitter_us=" << jitterUs
                 << " loss=" << loss << " queue=" << queueSize << " seed=" << seed
                 << " shape_mbps=" << shapingMbps
                 << " shape_burst_bytes="
                 << (scheduler.shapingBurstBytes > 0 ? scheduler.shapingBurstBytes : 65536);
            report.note = note.str();
        }
        if(!statsFile.empty()) {
            std::ofstream file(statsFile, std::ios::trunc);
            if(!file)
                throw std::runtime_error("cannot open stats file " + statsFile);
            if(statsFormat == "csv")
                tsnhub::writePriorityCsv(file, report);
            else if(statsFormat == "json")
                tsnhub::writeJson(file, report);
            else if(statsFormat == "table")
                tsnhub::writeLatencyTable(file, report);
            else
                tsnhub::writeSummaryLine(file, report);
            file.flush();
            if(!file)
                throw std::runtime_error("failed writing stats file " + statsFile);
            std::cout << "TsnHub statistics written to " << statsFile << " (" << statsFormat
                      << ")" << std::endl;
        }
        tsnhub::writeSummaryLine(std::cout, report);
        if(statsFormat != "summary")
            tsnhub::writeLatencyTable(std::cout, report);
        if(windowThread.joinable())
            windowThread.join();
        return 0;
    } catch(const std::exception &error) {
        std::cerr << "TsnHub: " << error.what() << '\n';
        return 1;
    }
}
