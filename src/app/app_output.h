#pragma once

#include <string>
#include <string_view>

#include "core/logger.h"

namespace musicboard::app {

// ============================================================================
// 启动与运行期的消息往哪里写
//
// 程序必须是 **GUI 子系统**：双击时不能弹出一个常驻的黑色控制台窗口，
// 更不能让那个窗口一关就把程序带走。但这样一来 printf 就没有了去处 ——
// 而"没有输出"和"输出丢失"是两件事，不能假装没发生。
//
// 所以规则是：
//   · 从终端启动（父进程有控制台）→ 附加到它，输出照常打在终端里
//   · 双击启动（没有控制台）→ 什么都不打印，但**同时写一份纯文本日志到
//     %LOCALAPPDATA%\MusicBoard\logs\app-<日期>.log**，并记住它的路径
//
// 这样"用户双击之后什么都没有"这件事是可查的，而不是只能靠猜。
// 实时状态仍然在界面里（状态栏），日志只负责"出了什么事"。
// ============================================================================

// 尽早调用：附加父控制台（如果有），打开日志文件。
// 返回日志文件路径；打不开时返回空路径（这时只有控制台可用）。
[[nodiscard]] std::filesystem::path initialize_output();

// 一行消息：控制台 + 日志文件。level 只影响日志里的标记。
void report(LogLevel level, std::string_view event, std::string_view context);

// 纯文本一行（给"曲库: 473 首"这类给人看的输出用）
void report_line(std::string_view text);

// printf 风格的版本。**这就是原来那些 std::printf 的去处** ——
// GUI 子系统下没有控制台时它们本来会消失，现在同时进日志文件。
void report_line_fmt(const char* format, ...);

// 当前日志文件路径（没打开时为空）
[[nodiscard]] const std::filesystem::path& output_log_path() noexcept;

}  // namespace musicboard::app
