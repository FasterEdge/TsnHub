// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace tsnhub {

struct Frame {
    std::uint64_t sequence{};
    std::uint8_t priority{};
    std::string stream;
    std::vector<std::uint8_t> payload;
    std::chrono::steady_clock::time_point receivedAt{};
};

} // namespace tsnhub
