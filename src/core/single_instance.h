#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 单实例守卫
//
// 要解决的具体行为：程序已经在跑时，再双击一次 EXE 不应该起第二套覆盖层与音频实例，
// 而应该把已有的那个叫到前面来。
//
// 做法：命名互斥体 + 一块共享内存携带已有进程的 PID。
// 只用互斥体只能知道"已经有一个在跑"，拿不到 PID，也就没法把那个窗口叫出来；
// 所以我们额外开一块小的文件映射把 PID 放进去。
//
// 名字用 Local\ 前缀：同一用户会话内唯一即可，不跨会话抢。
// 互斥体名字里带一个固定 GUID，避免与其他程序撞名。
// ============================================================================

inline constexpr std::wstring_view product_single_instance_guard_name =
    L"Local\\MusicBoard.SingleInstance.{7E4C1A93-2B58-4F17-9C6E-3A8D5B2E14F0}";

// 第二个实例用它把已有实例的面板叫出来。
//
// 用**注册消息**而不是自定义常量：RegisterWindowMessage 按名字注册，
// 两个进程各自注册同一个名字会拿到同一个 id —— 自定义常量只在本进程内成立。
inline constexpr wchar_t product_show_panel_message_name[] =
    L"MusicBoard.ShowPanel.{3C9E7D21-5A64-4E8B-9F02-71B6C4A8D53E}";

[[nodiscard]] inline unsigned int show_panel_message() noexcept {
    static const unsigned int id = ::RegisterWindowMessageW(product_show_panel_message_name);
    return id;
}

enum class SingleInstanceState : std::uint8_t {
    acquired,          // 本进程拿到了所有权，继续启动
    already_running,   // 已有实例在跑，existing_process_id 有效
};

struct SingleInstanceFailure final {
    std::string code;
    std::string context;
    DWORD win32_error = ERROR_SUCCESS;
};

class SingleInstanceGuard final {
public:
    SingleInstanceGuard() = default;
    ~SingleInstanceGuard();

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    [[nodiscard]] bool initialize(std::wstring_view name, SingleInstanceState& state,
                                  Failure& failure);

    [[nodiscard]] bool acquired() const noexcept { return mutex_ != nullptr && is_owner_; }
    [[nodiscard]] DWORD existing_process_id() const noexcept { return existing_process_id_; }

private:
    void release() noexcept;

    HANDLE mutex_ = nullptr;
    bool is_owner_ = false;
    HANDLE owner_mapping_ = nullptr;
    void* owner_view_ = nullptr;
    DWORD existing_process_id_ = 0;
};

}  // namespace musicboard
