// 全局快捷键测试
//
// 用一个**消息窗口**（HWND_MESSAGE）来收 WM_HOTKEY —— 它不需要桌面、不需要可见，
// 所以这个测试可以在没有显示器的环境里跑。
//
// 这里能测的是"注册这件事本身"：
//   · 合法的组合注册得上、撤得掉
//   · **被占用时报出是哪一条**（这正是选 RegisterHotKey 而不是键盘钩子的理由）
//   · 改绑定时不会留下旧组合（否则用户按两个键都会响）
//   · id ↔ WM_HOTKEY 的映射
//
// 测不到的（写在最前面，免得后来人以为漏了）：**真的按一下键**。
// 那需要人坐在机器前，或者一个能把按键注入到系统输入队列的工具。
// 自动派发 WM_HOTKEY 只能证明我们的消息处理对，不能证明注册本身有效 ——
// 后者正是这个模块的成败所在，所以不拿它冒充验证。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

#include "core/path_utf8.h"
#include "platform/global_hotkeys.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;

namespace {

// 消息窗口：只用来收消息，不出现在桌面上。
// 快捷键的注册不需要窗口可见，这一点让这个测试可以 headless 跑。
class MessageWindow final {
public:
    [[nodiscard]] bool create() {
        // 注册一个最小窗口类。shared_ptr 里的计数器是给 thunk 用的，
        // 这里只需要它把 this 传进来。
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &MessageWindow::proc;
        window_class.hInstance = ::GetModuleHandleW(nullptr);
        window_class.lpszClassName = kClassName;
        if (::RegisterClassExW(&window_class) == 0 &&
            ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        window_ = ::CreateWindowExW(0, kClassName, L"hotkey-test", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                    nullptr, ::GetModuleHandleW(nullptr), this);
        return window_ != nullptr;
    }

    ~MessageWindow() {
        if (window_ != nullptr) {
            ::DestroyWindow(window_);
        }
        ::UnregisterClassW(kClassName, ::GetModuleHandleW(nullptr));
    }

    [[nodiscard]] HWND handle() const noexcept { return window_; }
    [[nodiscard]] int received_hotkeys() const noexcept { return received_; }
    [[nodiscard]] int last_hotkey_id() const noexcept { return last_id_; }

    // 把队列里已经排到的消息处理掉（不阻塞）
    void pump() {
        MSG message{};
        while (::PeekMessageW(&message, window_, 0, 0, PM_REMOVE) != FALSE) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }

private:
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            ::SetWindowLongPtrW(window, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<MessageWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self != nullptr && message == WM_HOTKEY) {
            ++self->received_;
            self->last_id_ = static_cast<int>(wparam);
            return 0;
        }
        return ::DefWindowProcW(window, message, wparam, lparam);
    }

    static constexpr wchar_t kClassName[] = L"MusicBoard.HotkeyTestWindow";
    HWND window_ = nullptr;
    int received_ = 0;
    int last_id_ = -1;
};

}  // namespace

int main() {
    Registry registry;

    MessageWindow window;
    registry.check(window.create(), "消息窗口建起来了（不需要桌面）");
    if (window.handle() == nullptr) {
        registry.skip("全部快捷键用例（消息窗口创建失败）");
        return registry.finish("global_hotkey_tests");
    }

    // =======================================================================
    // 文本渲染（纯函数）
    // =======================================================================
    {
        registry.check(describe_hotkey(MOD_CONTROL | MOD_ALT, '1') == "Ctrl+Alt+1",
                       "Ctrl+Alt+1 的文本");
        registry.check(describe_hotkey(MOD_CONTROL | MOD_ALT | MOD_SHIFT, 'A') == "Ctrl+Alt+Shift+A",
                       "三个修饰键的顺序固定");
        registry.check(describe_hotkey(MOD_WIN | MOD_CONTROL, VK_F5) == "Ctrl+Win+F5",
                       "Win 键排在最后");
        registry.check(describe_hotkey(0, '5') == "5", "没有修饰键时只有按键本身");
        // MOD_NOREPEAT 是行为标志，不该出现在给人看的文本里
        registry.check(describe_hotkey(MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, '2') == "Ctrl+Alt+2",
                       "MOD_NOREPEAT 不出现在文本里");

        registry.check(virtual_key_label('7') == "7", "数字键标签");
        registry.check(virtual_key_label('Q') == "Q", "字母键标签");
        registry.check(virtual_key_label(VK_F12) == "F12", "功能键标签");
        registry.check(virtual_key_label(VK_SPACE) == "空格", "空格键标签是中文");
        registry.check(virtual_key_label(VK_NUMPAD3) == "小键盘 3", "小键盘标签");
        registry.check(virtual_key_label(0) == "(无)", "没有按键时给一句说明，不是空串");
        registry.check(!virtual_key_label(0xFE).empty(), "认不出来的键也有可读标签");
    }

    // =======================================================================
    // 默认绑定
    // =======================================================================
    {
        const std::vector<HotkeyBinding> bindings = default_sfx_hotkeys(4);
        registry.check(bindings.size() == 4, "四条默认绑定（四个音效台）");
        registry.check(bindings[0].virtual_key == '1' && bindings[3].virtual_key == '4',
                       "默认是 1..4");
        registry.check(bindings[0].modifiers == (MOD_CONTROL | MOD_ALT | MOD_NOREPEAT),
                       "默认修饰键是 Ctrl+Alt，且带 MOD_NOREPEAT");
        registry.check(bindings[2].description == "音效台 3", "说明里带台位号");
        registry.check(bindings[0].id == 0 && bindings[3].id == 3, "id 是 0..3");
        // MOD_NOREPEAT 很重要：按住不放时音效被连点几十次是灾难
        for (const HotkeyBinding& binding : bindings) {
            registry.check((binding.modifiers & MOD_NOREPEAT) != 0,
                           "每条都带 MOD_NOREPEAT（按住不放只触发一次）");
        }
        registry.check(default_sfx_hotkeys(0).empty(), "0 个台位时没有绑定");
    }

    // =======================================================================
    // 注册与撤销
    // =======================================================================
    {
        GlobalHotkeySet hotkeys;
        // 用一组很少撞车的组合，避免和这台机器上别的软件抢
        std::vector<HotkeyBinding> bindings{
            {101, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F13, "测试 1"},
            {102, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F14, "测试 2"},
        };

        std::vector<HotkeyRegistration> results;
        Failure failure;
        registry.check_invocation(
            [&] {
                return hotkeys.register_all(window.handle(), bindings, results, failure);
            },
            "两条都注册成功", failure);

        registry.check(results.size() == 2, "每条都有结果记录");
        registry.check(results[0].registered && results[1].registered, "两条都标了 registered");
        registry.check(results[0].reason.empty(), "成功时不写原因");
        registry.check(hotkeys.registered_count() == 2, "注册表里两条");
        registry.check(hotkeys.is_registered(101) && hotkeys.is_registered(102), "按 id 查得到");
        registry.check(!hotkeys.is_registered(999), "没注册过的 id 查不到");

        hotkeys.unregister_all();
        registry.check(hotkeys.registered_count() == 0, "撤销之后注册表空了");
        registry.check(!hotkeys.is_registered(101), "撤销之后查不到");
        // 撤销之后同一个组合还能被别人注册 —— 证明真的撤掉了
        GlobalHotkeySet second;
        std::vector<HotkeyRegistration> second_results;
        Failure second_failure;
        registry.check(second.register_all(window.handle(), bindings, second_results,
                                           second_failure),
                       "撤销之后同一组合可以再次注册（说明真的放开了）");
    }

    // =======================================================================
    // 冲突：必须报出是哪一条，不能静默跳过
    // =======================================================================
    {
        // 同一个进程里注册两次同一个组合，第二次必然失败 ——
        // 这条**不依赖机器上装了什么**，所以是可靠的。
        GlobalHotkeySet first;
        std::vector<HotkeyRegistration> first_results;
        Failure first_failure;
        const std::vector<HotkeyBinding> bindings{
            {201, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F15, "占用测试"},
        };
        registry.check(first.register_all(window.handle(), bindings, first_results, first_failure),
                       "第一条先占住 VK_F15");

        GlobalHotkeySet second;
        std::vector<HotkeyRegistration> second_results;
        Failure second_failure;
        const bool all_ok =
            second.register_all(window.handle(), bindings, second_results, second_failure);

        registry.check(!all_ok, "被占用时 register_all 返回 false（不假装成功）");
        registry.check(second_results.size() == 1, "仍然给出了这一条的结果");
        registry.check(!second_results[0].registered, "这一条标为未注册");
        registry.check(second_results[0].error_code == ERROR_HOTKEY_ALREADY_REGISTERED,
                       "错误码是 ERROR_HOTKEY_ALREADY_REGISTERED");
        registry.check(second_results[0].reason.find("已经被别的程序占用") != std::string::npos,
                       "原因说清了是被占用（而不是一句笼统的失败）");
        registry.check(second_results[0].reason.find("Ctrl+Alt+Shift+F15") != std::string::npos,
                       "原因里带上了具体是哪个组合 —— 用户才知道该改哪一个");
        registry.check(second.registered_count() == 0, "没注册上的不进注册表");
        registry.check(second_failure.empty(),
                       "整体不算致命失败（一条冲突不该让程序报错）");

        first.unregister_all();
    }

    // =======================================================================
    // 部分失败：其余的要照样注册
    // =======================================================================
    {
        GlobalHotkeySet blocker;
        std::vector<HotkeyRegistration> blocker_results;
        Failure blocker_failure;
        const std::vector<HotkeyBinding> taken{
            {301, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F16, "占了"},
        };
        registry.check(blocker.register_all(window.handle(), taken, blocker_results,
                                            blocker_failure),
                       "先占住 VK_F16");

        GlobalHotkeySet hotkeys;
        const std::vector<HotkeyBinding> mixed{
            {401, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F17, "空闲的 1"},
            {402, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F16, "被占的"},
            {403, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F18, "空闲的 2"},
        };
        std::vector<HotkeyRegistration> results;
        Failure failure;
        const bool all_ok = hotkeys.register_all(window.handle(), mixed, results, failure);

        registry.check(!all_ok, "有冲突时整体返回 false");
        registry.check(results.size() == 3, "三条都有结果");
        registry.check(results[0].registered && !results[1].registered && results[2].registered,
                       "被占的那条失败，其余两条照常注册 —— 不是一失败就全放弃");
        registry.check(hotkeys.registered_count() == 2, "注册表里是那两条成功的");
        registry.check(hotkeys.is_registered(401) && hotkeys.is_registered(403), "按 id 查得到成功的那两条");
        registry.check(!hotkeys.is_registered(402), "失败的那条不在注册表里");

        blocker.unregister_all();
    }

    // =======================================================================
    // 改绑定：不能留下旧组合
    // =======================================================================
    {
        GlobalHotkeySet hotkeys;
        HotkeyRegistration result;
        Failure failure;
        registry.check(hotkeys.register_one(
                           window.handle(),
                           {501, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F19, "旧"},
                           result),
                       "先注册旧组合");
        registry.check(result.registered, "旧组合注册上了");

        HotkeyRegistration changed;
        registry.check(hotkeys.register_one(
                           window.handle(),
                           {501, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F20, "新"},
                           changed),
                       "同一个 id 改注册新组合");
        registry.check(changed.registered, "新组合注册上了");
        registry.check(hotkeys.registered_count() == 1,
                       "注册表里还是一条（旧的被撤掉了，不是两条并存）");
        registry.check(hotkeys.bindings().front().virtual_key == VK_F20, "留下的是新组合");

        // 旧组合应该已经放开：换一个集合去注册它必须成功
        GlobalHotkeySet other;
        HotkeyRegistration reclaim;
        registry.check(other.register_one(
                           window.handle(),
                           {502, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F19, "回收"},
                           reclaim),
                       "旧组合已经被放开（别的绑定能用它）");
        other.unregister_all();
    }

    // =======================================================================
    // 参数校验：这些必须明确拒绝
    // =======================================================================
    {
        GlobalHotkeySet hotkeys;
        HotkeyRegistration result;
        registry.check(!hotkeys.register_one(window.handle(),
                                             {601, MOD_CONTROL, 0, "没有按键"}, result),
                       "没有指定按键 → 拒绝");
        registry.check(result.reason.find("没有指定按键") != std::string::npos,
                       "原因说清了是没给按键");
        registry.check(hotkeys.registered_count() == 0, "被拒的不进注册表");

        HotkeyRegistration no_window;
        registry.check(!hotkeys.register_one(nullptr,
                                             {602, MOD_CONTROL | MOD_ALT, VK_F21, "没有窗口"},
                                             no_window),
                       "没有消息窗口 → 拒绝");
        registry.check(no_window.reason.find("消息窗口") != std::string::npos,
                       "原因说清了是窗口的问题");

        std::vector<HotkeyRegistration> results;
        Failure failure;
        registry.check(!hotkeys.register_all(nullptr, default_sfx_hotkeys(4), results, failure),
                       "register_all 没有窗口时整体失败");
        registry.check(failure.code == fail_code::invalid_argument, "错误码是 invalid_argument");
    }

    // =======================================================================
    // id ↔ WM_HOTKEY 映射
    // =======================================================================
    {
        GlobalHotkeySet hotkeys;
        std::vector<HotkeyRegistration> results;
        Failure failure;
        registry.check(hotkeys.register_all(window.handle(), default_sfx_hotkeys(4), results,
                                            failure),
                       "注册四条默认绑定");

        registry.check(hotkeys.id_from_message(0) == 0, "wparam 0 → id 0");
        registry.check(hotkeys.id_from_message(3) == 3, "wparam 3 → id 3");
        registry.check(hotkeys.id_from_message(9) == -1,
                       "没注册过的 id → -1（不是瞎认一个）");

        registry.check(window.received_hotkeys() == 0,
                       "没有人真的按键，所以没有收到 WM_HOTKEY —— 这条如实记录，不冒充验证");
        window.pump();
        registry.check(window.received_hotkeys() == 0, "pump 之后依然没有（确实没人按键）");
    }

    return registry.finish("global_hotkey_tests");
}
