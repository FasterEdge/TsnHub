// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "tsn/PriorityMap.hpp"
#include "tsn/Scheduler.hpp"

#include "tests/TestAssertions.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using tsnhub::makeStreamKey;
using tsnhub::PriorityMap;
using tsnhub::resolveStreamPriority;
using tsnhub::StreamKey;

static bool throwsInvalidArgument(const std::function<void()> &action) {
    try {
        action();
    } catch(const std::invalid_argument &) {
        return true;
    } catch(...) {
        return false;
    }
    return false;
}

static void writeFixture(const std::string &path, const std::string &contents) {
    std::ofstream out(path);
    out << contents;
    if(!out) throw std::runtime_error("cannot write fixture: " + path);
}

int main() {
    // 1) makeStreamKey 用 UInt16 生成十进制标识，与 CLI 传入的数值保持同形。
    TSNHUB_TEST_REQUIRE(makeStreamKey(1, 2, 3) == (StreamKey{"1", "2", "3"}));
    TSNHUB_TEST_REQUIRE(makeStreamKey(0, 65535, 7) == (StreamKey{"0", "65535", "7"}));

    // 2) 精确三元组命中。
    const StreamKey camera{makeStreamKey(7, 1, 4)};
    const auto exact = PriorityMap::parse({"7:1:4:7"}, 0);
    TSNHUB_TEST_REQUIRE(exact.ruleCount() == 1);
    TSNHUB_TEST_REQUIRE(exact.lookup(camera).has_value() && *exact.lookup(camera) == 7);
    TSNHUB_TEST_REQUIRE(exact.resolve(camera) == 7);
    TSNHUB_TEST_REQUIRE(exact.matchedRule(camera) == "7:1:4:7");

    // 3) 未命中走默认优先级；lookup 与 resolve 必须区分开。
    const auto fallback = PriorityMap::parse({"7:1:4:7"}, 3);
    const StreamKey other{makeStreamKey(7, 1, 5)};
    TSNHUB_TEST_REQUIRE(!fallback.lookup(other).has_value());
    TSNHUB_TEST_REQUIRE(fallback.resolve(other) == 3);
    TSNHUB_TEST_REQUIRE(fallback.matchedRule(other).empty());
    TSNHUB_TEST_REQUIRE(fallback.defaultPriority() == 3);

    // 4) 最具体规则优先：同一 PublisherId 上，三元组压过两分量，两分量压过单分量。
    const auto layered = PriorityMap::parse({"7:*:*:2", "7:1:*:5", "7:1:4:6"}, 0);
    TSNHUB_TEST_REQUIRE(layered.resolve(camera) == 6);
    const StreamKey sameGroup{makeStreamKey(7, 1, 9)};
    TSNHUB_TEST_REQUIRE(layered.resolve(sameGroup) == 5);
    TSNHUB_TEST_REQUIRE(layered.matchedRule(sameGroup) == "7:1:*:5");
    const StreamKey samePublisher{makeStreamKey(7, 2, 0)};
    TSNHUB_TEST_REQUIRE(layered.resolve(samePublisher) == 2);

    // 5) 通配不匹配字面量 "*"：非数字 PublisherId（如字符串型）不会被通配误吞。
    const auto stringIds = PriorityMap::parse({"node-a:*:*:4"}, 0);
    TSNHUB_TEST_REQUIRE(stringIds.resolve(StreamKey{"node-a", "1", "1"}) == 4);
    TSNHUB_TEST_REQUIRE(stringIds.resolve(StreamKey{"node-b", "1", "1"}) == 0);

    // 6) 归一化：前导零等于无前导零，使配置文件可以照抄调试输出。
    const auto normalized = PriorityMap::parse({"007:01:004:7"}, 0);
    TSNHUB_TEST_REQUIRE(normalized.resolve(camera) == 7);
    TSNHUB_TEST_REQUIRE(normalized.matchedRule(camera) == "7:1:4:7");

    // 7) 非法输入必须在装载期拒绝，而不是静默忽略。
    TSNHUB_TEST_REQUIRE(throwsInvalidArgument([] { PriorityMap::parse({"7:1:4:8"}, 0); }));
    TSNHUB_TEST_REQUIRE(throwsInvalidArgument([] { PriorityMap::parse({"7:1:4:-1"}, 0); }));
    TSNHUB_TEST_REQUIRE(throwsInvalidArgument([] { PriorityMap::parse({"7:1:4"}, 0); }));
    TSNHUB_TEST_REQUIRE(throwsInvalidArgument([] { PriorityMap::parse({"7:1:4:1:2"}, 0); }));
    TSNHUB_TEST_REQUIRE(throwsInvalidArgument([] { PriorityMap::parse({"7:1::4"}, 0); }));
    TSNHUB_TEST_REQUIRE(throwsInvalidArgument([] { PriorityMap::parse({"*:*:*:4"}, 0); }));
    TSNHUB_TEST_REQUIRE(throwsInvalidArgument([] { PriorityMap::parse({}, 8); }));
    TSNHUB_TEST_REQUIRE(throwsInvalidArgument([] { PriorityMap::parse({"7:1:4:x"}, 0); }));

    // 8) 同具体度且可能同时命中的两条规则属于歧义配置，装载期拒绝。
    TSNHUB_TEST_REQUIRE(
        throwsInvalidArgument([] { PriorityMap::parse({"7:*:4:1", "*:1:4:2"}, 0); }));
    TSNHUB_TEST_REQUIRE(
        throwsInvalidArgument([] { PriorityMap::parse({"7:1:*:1", "7:*:1:2"}, 0); }));
    // 具体度不同则允许：更具体的规则确定胜出，等价于最长前缀匹配。
    const auto shadowing = PriorityMap::parse({"7:1:*:2", "7:1:4:1"}, 0);
    TSNHUB_TEST_REQUIRE(shadowing.resolve(camera) == 1);
    TSNHUB_TEST_REQUIRE(shadowing.matchedRule(camera) == "7:1:4:1");
    TSNHUB_TEST_REQUIRE(shadowing.resolve(StreamKey{"7", "1", "9"}) == 2);
    const auto nested = PriorityMap::parse({"7:*:*:2", "7:1:4:1"}, 0);
    TSNHUB_TEST_REQUIRE(nested.resolve(camera) == 1);
    TSNHUB_TEST_REQUIRE(nested.resolve(StreamKey{"7", "2", "9"}) == 2);
    // 同具体度但不可能同时命中则允许。
    const auto disjoint = PriorityMap::parse({"7:1:4:1", "7:1:5:2"}, 0);
    TSNHUB_TEST_REQUIRE(disjoint.resolve(camera) == 1);
    TSNHUB_TEST_REQUIRE(disjoint.resolve(StreamKey{"7", "1", "5"}) == 2);

    // 9) describe() 输出归一化后的规则，可直接回灌给 CLI。
    const auto described = PriorityMap::parse({"7:1:4:6", "9:*:*:1"}, 0);
    const auto entries = described.describe();
    TSNHUB_TEST_REQUIRE(entries.size() == 2);
    TSNHUB_TEST_REQUIRE(entries[0] == "7:1:4:6");
    TSNHUB_TEST_REQUIRE(entries[1] == "9:*:*:1");

    // 10) 文件装载：支持注释、空行、CRLF，并把错误定位到行号。
    const std::string path = std::tmpnam(nullptr);
    writeFixture(path,
                 "# TsnHub priority map\r\n"
                 "1:1:1:4\r\n"
                 "\r\n"
                 "2:2:2:7   # inline comment\r\n");
    const auto fromFile = PriorityMap::loadFile(path);
    TSNHUB_TEST_REQUIRE(fromFile.ruleCount() == 2);
    TSNHUB_TEST_REQUIRE(fromFile.resolve(makeStreamKey(1, 1, 1)) == 4);
    TSNHUB_TEST_REQUIRE(fromFile.resolve(makeStreamKey(2, 2, 2)) == 7);
    TSNHUB_TEST_REQUIRE(
        throwsInvalidArgument([&path] { PriorityMap::loadFile(path + "-missing"); }));

    writeFixture(path, "1:1:1:9\n");
    bool lineNumberReported = false;
    try {
        PriorityMap::loadFile(path);
    } catch(const std::invalid_argument &error) {
        const auto message = std::string(error.what());
        lineNumberReported = message.find(path + ":1:") != std::string::npos;
    }
    TSNHUB_TEST_REQUIRE(lineNumberReported);
    std::remove(path.c_str());

    // 11) 映射层与桥接层解耦：不依赖 open62541，因此 portable 构建也能覆盖。
    //     原生桥接层调用的就是 resolveStreamPriority 这个接缝。
    const auto wired = PriorityMap::parse({"1:1:1:6"}, 0);
    TSNHUB_TEST_REQUIRE(resolveStreamPriority(wired, 1, 1, 1) == 6);
    TSNHUB_TEST_REQUIRE(resolveStreamPriority(wired, 1, 1, 2) == wired.defaultPriority());

    // 12) 映射结果必须真正驱动调度器的八级队列与 GCL，而不是只影响信封字段。
    //     GCL 只开放 bit6：映射到 6 的流被放行，回落到默认优先级的流被拦下。
    tsnhub::SchedulerConfig gated;
    gated.schedule = {{std::chrono::microseconds{1000000}, 0x40}};
    tsnhub::Scheduler scheduler(gated);
    const auto now = tsnhub::Scheduler::Clock::now();
    auto selected = tsnhub::Frame{1, resolveStreamPriority(wired, 1, 1, 1), "opcua.int32",
                                  {0x2a}, {}};
    auto blocked = tsnhub::Frame{2, resolveStreamPriority(wired, 1, 1, 2), "opcua.int32",
                                 {0x2b}, {}};
    TSNHUB_TEST_REQUIRE(scheduler.enqueue(std::move(selected), now));
    TSNHUB_TEST_REQUIRE(scheduler.enqueue(std::move(blocked), now));
    const auto released = scheduler.releaseReady(now + std::chrono::microseconds{500000});
    TSNHUB_TEST_REQUIRE(released.size() == 1);
    TSNHUB_TEST_REQUIRE(released.front().sequence == 1);
    TSNHUB_TEST_REQUIRE(scheduler.stats().accepted == 2);
    TSNHUB_TEST_REQUIRE(scheduler.stats().released == 1);

    TSNHUB_TEST_REPORT("Priority map tests passed");
    return 0;
}
