#pragma once

// ============================================================================
// 极简测试脚手架
//
// 刻意不引入任何测试框架：一个计数器、一个断言函数就够了，
// 而"零第三方依赖"本身就是这个项目要守住的东西。
// 输出格式固定为 "PASS/FAIL <名字>"，方便用 grep 统计。
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

// check_invocation 要收 Failure，所以这里必须能看到它的定义。
// 脚手架本来就长在这个项目里，直接 include 比前向声明更不容易出错。
#include "core/failure.h"

namespace musicboard::test {

class Registry final {
public:
    // 构造时修好输出：不缓冲，并把断言报告改写到 stderr 而不是弹窗。
    //
    // 这不是洁癖。Debug CRT 断言失败会 abort()，而重定向到文件时 stdout 是块缓冲的，
    // 崩溃前打印的每一条 PASS/FAIL 都会丢掉；同时断言默认弹一个模态对话框，
    // 在没人看的自动化里就是永久挂起。这两条合起来的效果是"现场什么都没有"。
    // 踩过一次，写进这里当默认行为。
    Registry() {
        static_cast<void>(std::setvbuf(stdout, nullptr, _IONBF, 0));
#if defined(_MSC_VER)
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    }

    void check(bool condition, std::string_view name) {
        ++total_;
        if (condition) {
            std::printf("PASS %.*s\n", static_cast<int>(name.size()), name.data());
            return;
        }
        ++failed_;
        std::printf("FAIL %.*s\n", static_cast<int>(name.size()), name.data());
    }

    // 带上下文信息的断言：失败时把实际值打出来，省一次复现
    void check_eq(bool condition, std::string_view name, std::string_view detail) {
        ++total_;
        if (condition) {
            std::printf("PASS %.*s\n", static_cast<int>(name.size()), name.data());
            return;
        }
        ++failed_;
        std::printf("FAIL %.*s  [%.*s]\n", static_cast<int>(name.size()), name.data(),
                    static_cast<int>(detail.size()), detail.data());
    }

    // 调用 + 断言，**顺序由这里保证**。
    //
    // 不要写 `check_eq(call(...), "...", failure.to_string())`：
    // C++ 不规定实参求值顺序，detail 完全可能在调用之前就算好了 ——
    // 于是失败时打印的是调用**之前**的 failure（通常是空的），
    // 也就是最需要原因的时候它恰恰是空的。
    // 写成这个形式之后，先调用、后取原因，是确定的。
    template <typename Callable>
    bool check_invocation(Callable&& call, std::string_view name, const Failure& failure) {
        const bool ok = call();
        check_eq(ok, name, failure.to_string());
        return ok;
    }

    void skip(std::string_view name) {
        std::printf("SKIP %.*s\n", static_cast<int>(name.size()), name.data());
    }

    [[nodiscard]] int failed() const noexcept { return failed_; }
    [[nodiscard]] int total() const noexcept { return total_; }

    [[nodiscard]] int finish(std::string_view suite) const {
        std::printf("---- %.*s: %d/%d 通过，%d 失败 ----\n",
                    static_cast<int>(suite.size()), suite.data(), total_ - failed_, total_, failed_);
        return failed_ == 0 ? 0 : 1;
    }

private:
    int total_ = 0;
    int failed_ = 0;
};

}  // namespace musicboard::test
