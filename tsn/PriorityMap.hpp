// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tsnhub {

// PubSub 流的身份：PublisherId、WriterGroupId、DataSetWriterId 三元组。
// 三个分量都用字符串保存，因为 OPC UA 的 PublisherId 允许 UInt16/UInt32/String，
// 而映射表要对三者一视同仁。分量按字面值比较，首尾空白会被去掉。
struct StreamKey {
    std::string publisherId{};
    std::string writerGroupId{};
    std::string dataSetWriterId{};

    bool operator==(const StreamKey &other) const noexcept {
        return publisherId == other.publisherId && writerGroupId == other.writerGroupId &&
               dataSetWriterId == other.dataSetWriterId;
    }
    bool operator!=(const StreamKey &other) const noexcept { return !(*this == other); }
};

// 用桥接层当前配置的 UInt16 标识构造 StreamKey，使映射层不依赖 open62541。
StreamKey makeStreamKey(std::uint16_t publisherId, std::uint16_t writerGroupId,
                        std::uint16_t dataSetWriterId);

// 把流身份映射到 TSN 优先级（0..7，7 最高）。
//
// 规则形如 "publisher:writerGroup:writer:priority"，任一分量可写 "*" 表示通配。
// 匹配时取最具体的一条（非通配分量最多者）；同具体度且可能同时命中的两条规则
// 视为歧义，在装载阶段直接拒绝，保证同一份配置的解析结果唯一。
class PriorityMap {
public:
    static constexpr std::uint8_t highestPriority = 7;
    static constexpr char wildcard = '*';

    // 解析一组命令行条目。空规则集表示全部走 defaultPriority。
    static PriorityMap parse(const std::vector<std::string> &entries,
                             std::uint8_t defaultPriority = 0);
    // 从文本文件装载：每行一条规则，允许空行，'#' 起为注释。
    static PriorityMap loadFile(const std::string &path);

    // 追加一条规则，与已有规则冲突时抛 std::invalid_argument。
    void add(const std::string &entry);
    void setDefaultPriority(std::uint8_t priority);
    std::uint8_t defaultPriority() const noexcept { return defaultPriority_; }

    // 命中规则时返回其优先级，否则返回 std::nullopt（不套用默认值）。
    std::optional<std::uint8_t> lookup(const StreamKey &key) const;
    // 命中规则时返回优先级，否则返回 defaultPriority_。
    std::uint8_t resolve(const StreamKey &key) const;
    // 命中的规则原文（归一化形式），未命中返回空串。
    std::string matchedRule(const StreamKey &key) const;

    std::size_t ruleCount() const noexcept { return rules_.size(); }
    std::vector<std::string> describe() const;

private:
    struct Rule {
        StreamKey key;
        std::uint8_t priority{};
        std::size_t specificity{};
    };

    const Rule *find(const StreamKey &key) const;
    std::string describeRule(const Rule &rule) const;

    std::vector<Rule> rules_;
    std::uint8_t defaultPriority_{0};
};

// 把一条流的身份解析为 TSN 优先级（0..7）。
//
// Open62541Bridge 在收到 inbound 帧时调用的就是本函数：DataSetReader 只接受与
// 配置三元组完全一致的网络消息，因此配置里的 (publisherId, writerGroupId,
// dataSetWriterId) 就是该帧的身份，可安全用作映射键。把它独立成自由函数，
// 是为了让映射语义在无 open62541 的 portable 构建里也能被单测覆盖。
std::uint8_t resolveStreamPriority(const PriorityMap &map, std::uint16_t publisherId,
                                   std::uint16_t writerGroupId,
                                   std::uint16_t dataSetWriterId);

} // namespace tsnhub
