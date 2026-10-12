#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 崩溃最小转储
//
// 目标不是"恢复"，而是**留下一份可诊断的现场**：
//   · 未处理异常时写一份最小转储（MiniDumpNormal：线程栈 + 模块列表，不含完整内存）
//   · 转储写不出来时，至少写一个明确的失败标记文件说明原因
//   · 不尝试带病恢复，不吞掉异常
//
// 两点实现上的约束值得写下来：
//
//   1. **过滤器跑在一个已经出问题的进程里。** 所以目录路径在 initialize 时就存进静态变量，
//      崩溃路径上不做任何 C++ 容器分配；时间用 GetLocalTime 而不是 localtime_s。
//   2. **转储不含完整进程内存，但仍可能含线程栈与少量敏感片段。**
//      这件事必须告诉用户（见 README 的说明），不能默默收集。
// ============================================================================

inline constexpr std::size_t kCrashPathCapacity = 512;

struct CrashReportOutcome final {
    // 实际写出来的文件（转储或失败标记）
    wchar_t path[kCrashPathCapacity]{};
    bool dump_written = false;      // true = 转储；false = 只写了失败标记
    DWORD win32_error = ERROR_SUCCESS;
};

class CrashReporter final {
public:
    CrashReporter() = default;
    ~CrashReporter();

    CrashReporter(const CrashReporter&) = delete;
    CrashReporter& operator=(const CrashReporter&) = delete;

    // 指定转储目录并装上顶层异常过滤器。目录会被创建。
    [[nodiscard]] bool initialize(const std::filesystem::path& directory, Failure& failure);

    // 卸载过滤器（测试用）。正式运行时不调用。
    void shutdown() noexcept;

    [[nodiscard]] bool installed() const noexcept;

    // 上一次报告的结论。崩溃之后没机会读，主要给测试与日志用。
    [[nodiscard]] static const CrashReportOutcome& last_outcome() noexcept;
};

}  // namespace musicboard
