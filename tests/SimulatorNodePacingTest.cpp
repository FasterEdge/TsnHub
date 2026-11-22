// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "TestAssertions.hpp"
#include "transport/PortablePubSub.hpp"
#include "tsn/SimulatorNode.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// 端到端验证 paced 出队: 灌入一批同优先级帧后, 每次"成簇下发"的帧数不得超过
// creditFramesPerWindow, 且所有帧最终都被转发(推迟而不是丢弃)。
//
// 判定方式: 用远大于窗口内抖动、远小于窗口间隔的 gap 阈值把到达时刻切分成簇,
// 再断言"每簇帧数 <= 信用上限"。这样既不依赖节点的绝对相位(cycleStart_ 与测试
// 开始时刻不同源), 也不会因为两个相邻窗口的帧恰好落在同一个滑动区间里而误判。
struct Arrival {
    std::uint64_t sequence{};
    std::chrono::microseconds offsetUs{};
};

std::vector<Arrival> runPacedForwarding(const tsnhub::SchedulerConfig &config,
                                        std::uint16_t inputPort, std::uint16_t outputPort,
                                        std::size_t frameCount,
                                        std::chrono::milliseconds overallTimeout) {
    const tsnhub::UdpEndpoint input{"127.0.0.1", inputPort};
    const tsnhub::UdpEndpoint output{"127.0.0.1", outputPort};
    tsnhub::SimulatorNode node(config, input, output);
    tsnhub::UdpSocket receiver;
    receiver.bind(output);
    tsnhub::UdpSocket sender;
    std::atomic_bool running{true};
    std::thread worker([&] { node.run(running, 1ms); });

    const auto beganAt = std::chrono::steady_clock::now();
    for(std::uint64_t sequence = 1; sequence <= frameCount; ++sequence)
        sender.sendTo(input, tsnhub::PortablePubSub::encode(
                                 tsnhub::Frame{sequence, 0, "paced", {}, {}}));

    std::vector<Arrival> arrivals;
    for(std::size_t index = 0; index < frameCount; ++index) {
        const auto packet = receiver.receive(65507, overallTimeout);
        if(!packet)
            break;
        const auto decoded = tsnhub::PortablePubSub::decode(*packet);
        if(!decoded)
            continue;
        arrivals.push_back(Arrival{decoded->sequence,
                                   std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - beganAt)});
    }
    running.store(false);
    worker.join();
    return arrivals;
}

std::vector<Arrival> sortedByArrival(std::vector<Arrival> arrivals) {
    std::sort(arrivals.begin(), arrivals.end(),
              [](const Arrival &a, const Arrival &b) { return a.offsetUs < b.offsetUs; });
    return arrivals;
}

// 按 gap 阈值切簇: 相邻帧间隔超过 gap 即认为跨过一次"窗口边界"。
std::size_t largestCluster(const std::vector<Arrival> &ordered, std::chrono::microseconds gap) {
    std::size_t worst = 0;
    std::size_t current = 0;
    for(std::size_t index = 0; index < ordered.size(); ++index) {
        if(index == 0 || ordered[index].offsetUs - ordered[index - 1].offsetUs >= gap)
            current = 0;
        ++current;
        worst = std::max(worst, current);
    }
    return worst;
}

} // namespace

// 每簇帧数不得超过信用上限。窗口取 20ms 开放 + 20ms 关闭、信用 3,
// 因此每次成簇下发最多 3 帧, 簇间隔约为一个关闭窗口(>=15ms)。
// 若出队仍是一次抽空整队, 9 帧会落在同一簇里, 该断言必然失败。
static void perWindowFramesStayUnderCreditCap() {
    tsnhub::SchedulerConfig config;
    config.schedule = {{20000us, 0xff}, {20000us, 0x00}};
    config.baseDelay = 0us;
    config.maxQueuePerClass = 128;
    config.creditFramesPerWindow = 3;
    config.randomSeed = 3;

    const auto arrivals = runPacedForwarding(config, 46021, 46022, 9, 1500ms);
    TSNHUB_TEST_REQUIRE_MESSAGE(arrivals.size() == 9,
                                "pacing must defer frames, never drop them");
    const auto ordered = sortedByArrival(arrivals);

    // 簇阈值 5ms: 远大于窗口内下发抖动(实测 <1ms 量级), 远小于窗口间隔。
    const auto worst = largestCluster(ordered, 5000us);
    TSNHUB_TEST_REQUIRE_MESSAGE(worst <= 3,
                                "a single open window must not release more frames than the "
                                "credit cap");
    // 9 帧 / 每窗口 3 帧 => 至少跨 3 个开放窗口 => 首末帧跨度 >= 2 个关闭窗口。
    TSNHUB_TEST_REQUIRE_MESSAGE(ordered.back().offsetUs - ordered.front().offsetUs >= 20000us,
                                "a 3-frame credit for 9 frames must span several windows");
}

// 信用为 0 时不施加窗口配额: 同一批帧应在单个窗口内一起到达, 与改动前一致。
static void zeroCreditKeepsBurstBehaviour() {
    tsnhub::SchedulerConfig config;
    config.schedule = {{20000us, 0xff}, {20000us, 0x00}};
    config.baseDelay = 0us;
    config.maxQueuePerClass = 128;
    config.creditFramesPerWindow = 0;
    config.randomSeed = 3;

    const auto arrivals = runPacedForwarding(config, 46031, 46032, 9, 1500ms);
    TSNHUB_TEST_REQUIRE(arrivals.size() == 9);
    const auto ordered = sortedByArrival(arrivals);
    TSNHUB_TEST_REQUIRE_MESSAGE(largestCluster(ordered, 5000us) == 9,
                                "without a credit cap the whole backlog drains in one window");
}

int main() {
    perWindowFramesStayUnderCreditCap();
    zeroCreditKeepsBurstBehaviour();

    TSNHUB_TEST_REPORT("Simulator node pacing tests passed");
    return 0;
}
