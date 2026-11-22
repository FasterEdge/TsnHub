// FasterEdge 开源项目 - Github: https://github.com/FasterEdge - Gitee: https://gitee.com/FasterEdge
#pragma once

// 测试专用断言, 刻意不复用 <cassert>。
// <cassert> 的 assert 被 `#ifndef NDEBUG` 包裹, 而 CMake 的 Release 配置会注入
// -DNDEBUG(GCC/Clang 为 -O3 -DNDEBUG, MSVC 为 /O2 /DNDEBUG), 于是整个 Release
// 构建里断言被编译器删除, 测试用例直接返回 0, ctest 变成"零检查的绿灯"。
// 这里的宏是普通函数调用, 与 NDEBUG 无关, 任何构建类型下都会真正执行,
// 失败时打印 file:line + 表达式原文 + 可选详情, 并以退出码 1 结束进程。

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>

namespace tsnhub {
namespace test {

// 本进程执行过的断言条数, 结束时打印, 用来证明检查真的跑过而不是被删掉了。
inline std::size_t &assertionCount() {
    static std::size_t count = 0;
    return count;
}

// 打印失败详情并以非零退出码结束; 语义等价于 assert, 但不依赖 NDEBUG。
[[noreturn]] inline void fail(const char *file, int line, const char *expression,
                              const std::string &detail) {
    std::cerr << "FAIL " << file << ':' << line << ": " << expression;
    if(!detail.empty())
        std::cerr << " | " << detail;
    std::cerr << '\n';
    std::cerr.flush();
    std::exit(1);
}

} // namespace test
} // namespace tsnhub

// 断言 condition 为真, 失败时打印 file:line 与表达式原文并立即退出。
#define TSNHUB_TEST_REQUIRE(condition)                                              \
    do {                                                                            \
        ++::tsnhub::test::assertionCount();                                         \
        if(!static_cast<bool>(condition))                                           \
            ::tsnhub::test::fail(__FILE__, __LINE__, #condition, std::string{});    \
    } while(false)

// 与 TSNHUB_TEST_REQUIRE 相同, 额外打印一段说明失败原因的字符串(可为 const char*)。
#define TSNHUB_TEST_REQUIRE_MESSAGE(condition, detail)                              \
    do {                                                                            \
        ++::tsnhub::test::assertionCount();                                         \
        if(!static_cast<bool>(condition))                                           \
            ::tsnhub::test::fail(__FILE__, __LINE__, #condition, detail);           \
    } while(false)

// 用例结束时汇报: 打印用例名与实际执行的断言条数, 便于在 CI 日志里核对。
#define TSNHUB_TEST_REPORT(name)                                                    \
    do {                                                                            \
        std::cout << name << " (" << ::tsnhub::test::assertionCount()               \
                  << " assertions)\n";                                              \
    } while(false)
