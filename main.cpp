#include "tsn/SimulatorNode.hpp"
#ifdef TSNHUB_USE_OPEN62541_PUBSUB
#include "pubsub/Open62541Bridge.hpp"
#endif

#include <CLI/CLI.hpp>
#include <open62541/config.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
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
    std::uint16_t inputPublisherId = 1;
    std::uint16_t inputWriterGroupId = 1;
    std::uint16_t inputWriterId = 1;
    std::uint16_t outputPublisherId = 2;
    std::uint16_t outputWriterGroupId = 2;
    std::uint16_t outputWriterId = 2;
    double publishIntervalMs = 1.0;
    std::vector<std::string> gates;
    std::int64_t delayUs = 0;
    std::int64_t jitterUs = 0;
    double loss = 0.0;
    std::size_t queueSize = 1024;
    std::uint64_t seed = 1;
    bool showCapabilities = false;

    app.add_option("--mode", mode, "Transport mode: portable or native")->check(CLI::IsMember({"portable", "native"}));
    app.add_option("--listen", input, "Portable input UDP endpoint host:port");
    app.add_option("--forward", output, "Portable output UDP endpoint host:port");
    app.add_option("--subscribe-url", subscribeUrl, "Native PubSub input URL");
    app.add_option("--publish-url", publishUrl, "Native PubSub output URL");
    app.add_option("--input-publisher-id", inputPublisherId, "Expected upstream PublisherId");
    app.add_option("--input-writer-group-id", inputWriterGroupId, "Expected upstream WriterGroupId");
    app.add_option("--input-writer-id", inputWriterId, "Expected upstream DataSetWriterId");
    app.add_option("--output-publisher-id", outputPublisherId, "TsnHub output PublisherId");
    app.add_option("--output-writer-group-id", outputWriterGroupId, "TsnHub output WriterGroupId");
    app.add_option("--output-writer-id", outputWriterId, "TsnHub output DataSetWriterId");
    app.add_option("--publish-interval-ms", publishIntervalMs, "Native PubSub publishing interval")->check(CLI::PositiveNumber);
    app.add_option("--gate", gates, "Gate slot duration_us:mask (repeatable, mask accepts 0xff)");
    app.add_option("--delay-us", delayUs, "Base forwarding delay in microseconds")->check(CLI::NonNegativeNumber);
    app.add_option("--jitter-us", jitterUs, "Symmetric delay jitter in microseconds")->check(CLI::NonNegativeNumber);
    app.add_option("--loss", loss, "Simulated packet loss in [0,1]")->check(CLI::Range(0.0, 1.0));
    app.add_option("--queue", queueSize, "Maximum queued frames per priority")->check(CLI::PositiveNumber);
    app.add_option("--seed", seed, "Deterministic random seed");
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

        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        tsnhub::SchedulerStats stats;
        if(mode == "native") {
#ifdef TSNHUB_USE_OPEN62541_PUBSUB
            tsnhub::NativePubSubConfig native;
            native.listenUrl = subscribeUrl;
            native.publishUrl = publishUrl;
            native.inputPublisherId = inputPublisherId;
            native.inputWriterGroupId = inputWriterGroupId;
            native.inputDataSetWriterId = inputWriterId;
            native.outputPublisherId = outputPublisherId;
            native.outputWriterGroupId = outputWriterGroupId;
            native.outputDataSetWriterId = outputWriterId;
            native.publishingIntervalMs = publishIntervalMs;
            tsnhub::Open62541Bridge node(native, scheduler);
            std::cout << "TsnHub native PubSub subscribing " << subscribeUrl
                      << ", publishing " << publishUrl << '\n';
            node.run(running);
            stats = node.stats();
#else
            throw std::runtime_error("native PubSub is not enabled in this build");
#endif
        } else {
            const auto listen = parseEndpoint(input);
            const auto forward = parseEndpoint(output);
            tsnhub::SimulatorNode node(scheduler, listen, forward);
            std::cout << "TsnHub portable mode listening on " << input
                      << ", forwarding to " << output << '\n';
            node.run(running);
            stats = node.stats();
        }
        std::cout << "accepted=" << stats.accepted << " released=" << stats.released
                  << " dropped_loss=" << stats.droppedByLoss
                  << " dropped_queue=" << stats.droppedByQueue << '\n';
        return 0;
    } catch(const std::exception &error) {
        std::cerr << "TsnHub: " << error.what() << '\n';
        return 1;
    }
}
