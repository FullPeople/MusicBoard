#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 全局快捷键
//
// 用途：跑团现场按 Ctrl+Alt+1..4 瞬时触发四个音效台。**应用没有焦点时也要能用**，
// 这正是"全局"的意思。
//
// 为什么是 RegisterHotKey 而不是 WH_KEYBOARD_LL：
//
//   1. **系统认可的做法。** 低级键盘钩子会让每个按键都经过我们的回调（有延迟），
//      被安全软件当作键盘记录器告警；更糟的是回调一旦慢了，系统会**静默摘掉**它 ——
//      用户以为快捷键还在，其实早就没了。那是本项目最怕的失败方式。
//   2. **失败是响亮的。** 组合键被别的程序占用时，RegisterHotKey 返回
//      ERROR_HOTKEY_ALREADY_REGISTERED。于是《哪一条没注册上》可以说清楚，
//      而不是让快捷键悄悄失效。
//
// 代价：注册成功的组合键会被从当前前台程序手里拿走。对一个用户明确设置的
// 快捷键来说，这正是"快捷键"的定义。所以默认组合用 Ctrl+Alt 起手
// （很少与别的软件撞），并且可以整体关掉。
//
// 一条**已知且必须说清**的边界：RegisterHotKey 注册的是"修饰键 + 虚拟键"的组合，
// 它不区分左右修饰键，也不参与键盘布局的重映射。这对数字键触发音效完全够用。
// ============================================================================

// 一个快捷键的绑定。modifiers 用 MOD_* 常量（MOD_CONTROL 等）。
struct HotkeyBinding final {
    // 调用方给的稳定标识。收到 WM_HOTKEY 时用 wparam 换回来。
    int id = 0;
    UINT modifiers = 0;
    UINT virtual_key = 0;
    // 给人看的说明（"音效台 1"），用于冲突提示
    std::string description;
};

// 单条绑定的注册结果。注册失败时 reason 非空 —— **不静默跳过**。
struct HotkeyRegistration final {
    HotkeyBinding binding;
    bool registered = false;
    DWORD error_code = 0;
    std::string reason;
};

// 把修饰键与虚拟键渲染成 "Ctrl+Alt+1" 这样给人看的文本。
[[nodiscard]] std::string describe_hotkey(UINT modifiers, UINT virtual_key);

// 把虚拟键渲染成短标签（"1" / "F5" / "Space"）。界面上的台位角标用它。
[[nodiscard]] std::string virtual_key_label(UINT virtual_key);

class GlobalHotkeySet final {
public:
    GlobalHotkeySet() = default;
    ~GlobalHotkeySet();

    GlobalHotkeySet(const GlobalHotkeySet&) = delete;
    GlobalHotkeySet& operator=(const GlobalHotkeySet&) = delete;

    // 注册一批绑定。message_window 用来接收 WM_HOTKEY（可以是一个消息窗口，
    // 不需要可见）。**整体不会因为某一条失败而放弃其余的** ——
    // 但每一条的成败都在 results 里说清楚，由界面决定怎么提示。
    [[nodiscard]] bool register_all(HWND message_window,
                                    const std::vector<HotkeyBinding>& bindings,
                                    std::vector<HotkeyRegistration>& results, Failure& failure);

    // 只注册一条（设置界面改一条快捷键时用）
    [[nodiscard]] bool register_one(HWND message_window, const HotkeyBinding& binding,
                                    HotkeyRegistration& result);

    void unregister_all();
    void unregister_one(int id);

    [[nodiscard]] bool is_registered(int id) const noexcept;
    [[nodiscard]] std::size_t registered_count() const noexcept { return registered_.size(); }
    // 当前注册成功的绑定（界面显示角标用）
    [[nodiscard]] const std::vector<HotkeyBinding>& bindings() const noexcept { return registered_; }
    // 从 WM_HOTKEY 的 wparam 换回绑定 id。不认识则返回 -1。
    [[nodiscard]] int id_from_message(WPARAM wparam) const noexcept;
    [[nodiscard]] HWND message_window() const noexcept { return message_window_; }

private:
    HWND message_window_ = nullptr;
    std::vector<HotkeyBinding> registered_;
};

// 默认绑定：Ctrl+Alt+1..4 → 音效台 1..4。
// 选 Ctrl+Alt 是因为它很少与别的软件撞，而数字键正好四个。
[[nodiscard]] std::vector<HotkeyBinding> default_sfx_hotkeys(std::size_t deck_count);

}  // namespace musicboard
