#include "platform/global_hotkeys.h"

#include <algorithm>

namespace musicboard {
namespace {

// 把 MOD_* 拆成人话。顺序固定，免得同一个组合在界面上有两种写法。
void append_modifier(std::string& out, bool present, const char* name) {
    if (!present) {
        return;
    }
    if (!out.empty()) {
        out += "+";
    }
    out += name;
}

}  // namespace

std::string describe_hotkey(UINT modifiers, UINT virtual_key) {
    const UINT mods = modifiers & ~static_cast<UINT>(MOD_NOREPEAT);
    std::string out;
    append_modifier(out, (mods & MOD_CONTROL) != 0, "Ctrl");
    append_modifier(out, (mods & MOD_ALT) != 0, "Alt");
    append_modifier(out, (mods & MOD_SHIFT) != 0, "Shift");
    append_modifier(out, (mods & MOD_WIN) != 0, "Win");
    if (!out.empty()) {
        out += "+";
    }
    out += virtual_key_label(virtual_key);
    return out;
}

std::string virtual_key_label(UINT virtual_key) {
    if (virtual_key == 0) {
        return "(无)";
    }
    if (virtual_key >= '0' && virtual_key <= '9') {
        return std::string(1, static_cast<char>(virtual_key));
    }
    if (virtual_key >= 'A' && virtual_key <= 'Z') {
        return std::string(1, static_cast<char>(virtual_key));
    }
    if (virtual_key >= VK_F1 && virtual_key <= VK_F24) {
        return "F" + std::to_string(virtual_key - VK_F1 + 1);
    }
    if (virtual_key >= VK_NUMPAD0 && virtual_key <= VK_NUMPAD9) {
        return "小键盘 " + std::to_string(virtual_key - VK_NUMPAD0);
    }
    switch (virtual_key) {
        case VK_SPACE:
            return "空格";
        case VK_RETURN:
            return "回车";
        case VK_TAB:
            return "Tab";
        case VK_ESCAPE:
            return "Esc";
        case VK_BACK:
            return "退格";
        case VK_INSERT:
            return "Ins";
        case VK_DELETE:
            return "Del";
        case VK_HOME:
            return "Home";
        case VK_END:
            return "End";
        case VK_PRIOR:
            return "PgUp";
        case VK_NEXT:
            return "PgDn";
        case VK_LEFT:
            return "←";
        case VK_RIGHT:
            return "→";
        case VK_UP:
            return "↑";
        case VK_DOWN:
            return "↓";
        case VK_OEM_3:
            return "`";
        case VK_OEM_MINUS:
            return "-";
        case VK_OEM_PLUS:
            return "=";
        case VK_OEM_4:
            return "[";
        case VK_OEM_6:
            return "]";
        case VK_OEM_5:
            return "\\";
        case VK_OEM_1:
            return ";";
        case VK_OEM_7:
            return "'";
        case VK_OEM_COMMA:
            return ",";
        case VK_OEM_PERIOD:
            return ".";
        case VK_OEM_2:
            return "/";
        default:
            break;
    }
    // 认不出来的键就给个数字，总比空白好（界面还能看出"这是哪个键"）
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "VK 0x%02X", virtual_key);
    return buffer;
}

std::vector<HotkeyBinding> default_sfx_hotkeys(std::size_t deck_count) {
    std::vector<HotkeyBinding> bindings;
    bindings.reserve(deck_count);
    for (std::size_t i = 0; i < deck_count; ++i) {
        HotkeyBinding binding;
        binding.id = static_cast<int>(i);
        // MOD_NOREPEAT：按住不放时只触发一次。音效被连点几十次是灾难。
        binding.modifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
        binding.virtual_key = static_cast<UINT>('1' + i);
        binding.description = "音效台 " + std::to_string(i + 1);
        bindings.push_back(std::move(binding));
    }
    return bindings;
}

GlobalHotkeySet::~GlobalHotkeySet() {
    unregister_all();
}

bool GlobalHotkeySet::register_all(HWND message_window,
                                  const std::vector<HotkeyBinding>& bindings,
                                  std::vector<HotkeyRegistration>& results, Failure& failure) {
    results.clear();
    if (message_window == nullptr) {
        failure.set(fail_code::invalid_argument, "没有消息窗口，快捷键收不到 WM_HOTKEY");
        return false;
    }
    message_window_ = message_window;

    results.reserve(bindings.size());
    bool all_ok = true;
    for (const HotkeyBinding& binding : bindings) {
        HotkeyRegistration result;
        if (!register_one(message_window, binding, result)) {
            all_ok = false;
        }
        // 一条失败不影响其余的：其余三条音效台该能用还是要能用
        results.push_back(std::move(result));
    }
    return all_ok;
}

bool GlobalHotkeySet::register_one(HWND message_window, const HotkeyBinding& binding,
                                   HotkeyRegistration& result) {
    result = HotkeyRegistration{};
    result.binding = binding;

    if (message_window == nullptr) {
        result.reason = "没有消息窗口";
        return false;
    }
    message_window_ = message_window;

    if (binding.virtual_key == 0) {
        // Windows 允许"只有修饰键"的组合（MOD_ALT 单独按），但那对音效触发没有意义，
        // 而且很容易误触。明确拒绝，而不是让它注册上去。
        result.reason = "没有指定按键";
        return false;
    }

    // 已经注册过的同一个 id 先撤掉，再按新组合注册 ——
    // 否则改快捷键时会变成"旧组合还占着、新组合也注册上"，用户按两个键都响。
    unregister_one(binding.id);

    if (::RegisterHotKey(message_window, binding.id, binding.modifiers, binding.virtual_key) ==
        FALSE) {
        const DWORD error = ::GetLastError();
        result.error_code = error;
        if (error == ERROR_HOTKEY_ALREADY_REGISTERED) {
            result.reason = describe_hotkey(binding.modifiers, binding.virtual_key) +
                            " 已经被别的程序占用了";
        } else {
            result.reason = "注册 " + describe_hotkey(binding.modifiers, binding.virtual_key) +
                            " 失败（Win32 错误 " + std::to_string(error) + "）";
        }
        return false;
    }

    result.registered = true;
    registered_.push_back(binding);
    return true;
}

void GlobalHotkeySet::unregister_all() {
    if (message_window_ != nullptr) {
        for (const HotkeyBinding& binding : registered_) {
            ::UnregisterHotKey(message_window_, binding.id);
        }
    }
    registered_.clear();
}

void GlobalHotkeySet::unregister_one(int id) {
    const auto it = std::find_if(registered_.begin(), registered_.end(),
                                 [id](const HotkeyBinding& b) { return b.id == id; });
    if (it == registered_.end()) {
        return;
    }
    if (message_window_ != nullptr) {
        ::UnregisterHotKey(message_window_, id);
    }
    registered_.erase(it);
}

bool GlobalHotkeySet::is_registered(int id) const noexcept {
    return std::any_of(registered_.begin(), registered_.end(),
                       [id](const HotkeyBinding& b) { return b.id == id; });
}

int GlobalHotkeySet::id_from_message(WPARAM wparam) const noexcept {
    const int id = static_cast<int>(wparam);
    return is_registered(id) ? id : -1;
}

}  // namespace musicboard
