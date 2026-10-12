#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace musicboard {

// ============================================================================
// 失败契约
//
// 全项目不用异常。所有可能失败的函数返回 bool 并填写 Failure，
// 全部标注 [[nodiscard]] 让编译器强制检查"有没有人处理它"。
//
// 为什么这样做：这个项目要避免的头号问题就是"静默失败"。
// 在 Godot 原型里，文件操作失败只写控制台、下载取消只是把标志位置回 false、
// 进度条永远不会动 —— 用户看到的是"点了没反应"。用返回值强制处理，
// 这类问题在编译期就没有藏身之处。
//
// code    稳定的机器可读标识（用于测试断言与日志检索），如 "audio.decode_failed"
// context 给人看的一句话，含足够定位问题的信息（哪个文件、什么状态）
// ============================================================================

struct Failure final {
    std::string code;
    std::string context;

    [[nodiscard]] bool empty() const noexcept { return code.empty(); }

    // 已经填过就不覆盖：最内层的原因比外层包装更有价值
    void set(std::string_view new_code, std::string new_context) {
        if (!code.empty()) {
            return;
        }
        code.assign(new_code);
        context = std::move(new_context);
    }

    void clear() noexcept {
        code.clear();
        context.clear();
    }

    [[nodiscard]] std::string to_string() const {
        if (code.empty()) {
            return "ok";
        }
        return code + ": " + context;
    }
};

// 统一的错误码。集中定义是为了测试可以断言具体值，而不是匹配字符串字面量。
namespace fail_code {
inline constexpr std::string_view invalid_argument = "common.invalid_argument";
inline constexpr std::string_view file_not_found = "io.file_not_found";
inline constexpr std::string_view file_open_failed = "io.file_open_failed";
inline constexpr std::string_view file_read_failed = "io.file_read_failed";
inline constexpr std::string_view file_too_large = "io.file_too_large";
inline constexpr std::string_view cancelled = "io.cancelled";
inline constexpr std::string_view unsupported_format = "audio.unsupported_format";
inline constexpr std::string_view decode_failed = "audio.decode_failed";
inline constexpr std::string_view backend_unavailable = "audio.backend_unavailable";
inline constexpr std::string_view platform_api_failed = "platform.api_failed";
}  // namespace fail_code

}  // namespace musicboard
