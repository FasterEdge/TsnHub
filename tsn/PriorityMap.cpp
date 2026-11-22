// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#include "tsn/PriorityMap.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace tsnhub {
namespace {

const std::string wildcardString(1, PriorityMap::wildcard);

std::string trim(const std::string &value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if(begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

bool isDigits(const std::string &value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::vector<std::string> splitComponents(const std::string &entry) {
    std::vector<std::string> parts;
    std::string current;
    for(const char c : trim(entry)) {
        if(c == ':') {
            parts.push_back(trim(current));
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    parts.push_back(trim(current));
    return parts;
}

// 纯数字标识去掉前导零，使配置里的 01 与运行期由 UInt16 生成的 1 相等；
// 非数字标识按字面值保存，从而 UInt32 或字符串型 PublisherId 也能直接写。
std::string normalizeId(const std::string &value, const std::string &entry) {
    if(value == wildcardString) return value;
    if(value.empty())
        throw std::invalid_argument("priority rule '" + entry + "' has an empty component");
    if(value.find_first_of(" \t\r\n") != std::string::npos)
        throw std::invalid_argument("priority rule '" + entry + "' contains whitespace");
    if(!isDigits(value)) return value;
    const auto firstNonZero = value.find_first_not_of('0');
    return firstNonZero == std::string::npos ? std::string("0") : value.substr(firstNonZero);
}

std::uint8_t parsePriority(const std::string &text, const std::string &entry) {
    if(!isDigits(text))
        throw std::invalid_argument("priority rule '" + entry + "' has a non-numeric priority");
    const auto value = std::stoul(text);
    if(value > PriorityMap::highestPriority)
        throw std::invalid_argument("priority rule '" + entry + "' priority must be in 0..7");
    return static_cast<std::uint8_t>(value);
}

std::size_t specificityOf(const StreamKey &key) {
    const std::string parts[3] = {key.publisherId, key.writerGroupId, key.dataSetWriterId};
    std::size_t count = 0;
    for(const auto &part : parts)
        if(part != wildcardString) ++count;
    return count;
}

// 两条规则能同时命中同一个 StreamKey：每个分量要么相等，要么至少一方为通配。
bool compatible(const StreamKey &left, const StreamKey &right) {
    const std::string leftParts[3] = {left.publisherId, left.writerGroupId, left.dataSetWriterId};
    const std::string rightParts[3] = {right.publisherId, right.writerGroupId,
                                       right.dataSetWriterId};
    for(int i = 0; i < 3; ++i) {
        if(leftParts[i] == rightParts[i]) continue;
        if(leftParts[i] == wildcardString || rightParts[i] == wildcardString) continue;
        return false;
    }
    return true;
}

} // namespace

StreamKey makeStreamKey(std::uint16_t publisherId, std::uint16_t writerGroupId,
                        std::uint16_t dataSetWriterId) {
    return StreamKey{std::to_string(publisherId), std::to_string(writerGroupId),
                     std::to_string(dataSetWriterId)};
}

std::uint8_t resolveStreamPriority(const PriorityMap &map, std::uint16_t publisherId,
                                   std::uint16_t writerGroupId,
                                   std::uint16_t dataSetWriterId) {
    return map.resolve(makeStreamKey(publisherId, writerGroupId, dataSetWriterId));
}

PriorityMap PriorityMap::parse(const std::vector<std::string> &entries,
                              std::uint8_t defaultPriority) {
    PriorityMap map;
    map.setDefaultPriority(defaultPriority);
    for(const auto &entry : entries) map.add(entry);
    return map;
}

PriorityMap PriorityMap::loadFile(const std::string &path) {
    std::ifstream stream(path);
    // MinGW 的 basic_filebuf 在文件不存在时可能保持 failbit 为假，只有 is_open()
    // 可靠地反映打开失败，因此这里显式检查 is_open()。
    if(!stream.is_open()) throw std::invalid_argument("cannot open priority map file: " + path);
    PriorityMap map;
    std::string line;
    std::size_t lineNumber = 0;
    while(std::getline(stream, line)) {
        ++lineNumber;
        const auto comment = line.find('#');
        if(comment != std::string::npos) line.erase(comment);
        const auto entry = trim(line);
        if(entry.empty()) continue;
        try {
            map.add(entry);
        } catch(const std::invalid_argument &error) {
            throw std::invalid_argument(path + ":" + std::to_string(lineNumber) + ": " +
                                        error.what());
        }
    }
    return map;
}

void PriorityMap::setDefaultPriority(std::uint8_t priority) {
    if(priority > highestPriority)
        throw std::invalid_argument("default priority must be in 0..7");
    defaultPriority_ = priority;
}

void PriorityMap::add(const std::string &entry) {
    const auto parts = splitComponents(entry);
    if(parts.size() != 4)
        throw std::invalid_argument(
            "priority rule '" + entry +
            "' must be publisher:writerGroup:writer:priority (components may be '*')");
    Rule rule;
    rule.key = StreamKey{normalizeId(parts[0], entry), normalizeId(parts[1], entry),
                         normalizeId(parts[2], entry)};
    rule.priority = parsePriority(parts[3], entry);
    rule.specificity = specificityOf(rule.key);
    if(rule.specificity == 0)
        throw std::invalid_argument("priority rule '" + entry +
                                    "' is fully wildcard, use the default priority instead");
    for(const auto &existing : rules_) {
        if(existing.specificity != rule.specificity) continue;
        if(!compatible(existing.key, rule.key)) continue;
        throw std::invalid_argument("priority rule '" + entry + "' is ambiguous with '" +
                                    describeRule(existing) +
                                    "': both can match the same stream at equal specificity");
    }
    rules_.push_back(std::move(rule));
}

const PriorityMap::Rule *PriorityMap::find(const StreamKey &key) const {
    const Rule *best = nullptr;
    for(const auto &rule : rules_) {
        if(rule.key.publisherId != wildcardString && rule.key.publisherId != key.publisherId)
            continue;
        if(rule.key.writerGroupId != wildcardString && rule.key.writerGroupId != key.writerGroupId)
            continue;
        if(rule.key.dataSetWriterId != wildcardString &&
           rule.key.dataSetWriterId != key.dataSetWriterId)
            continue;
        // specificity 相同且同时命中会被 add() 拒绝，因此这里不会出现平局。
        if(!best || rule.specificity > best->specificity) best = &rule;
    }
    return best;
}

std::optional<std::uint8_t> PriorityMap::lookup(const StreamKey &key) const {
    const Rule *rule = find(key);
    if(!rule) return std::nullopt;
    return rule->priority;
}

std::uint8_t PriorityMap::resolve(const StreamKey &key) const {
    const auto priority = lookup(key);
    return priority ? *priority : defaultPriority_;
}

std::string PriorityMap::matchedRule(const StreamKey &key) const {
    const Rule *rule = find(key);
    return rule ? describeRule(*rule) : std::string{};
}

std::vector<std::string> PriorityMap::describe() const {
    std::vector<std::string> entries;
    entries.reserve(rules_.size());
    for(const auto &rule : rules_) entries.push_back(describeRule(rule));
    return entries;
}

std::string PriorityMap::describeRule(const Rule &rule) const {
    return rule.key.publisherId + ':' + rule.key.writerGroupId + ':' +
           rule.key.dataSetWriterId + ':' + std::to_string(rule.priority);
}

} // namespace tsnhub
