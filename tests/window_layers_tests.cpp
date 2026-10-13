#include "test_harness.h"
#include "platform/window_layers.h"

namespace {
bool above(HWND first, HWND second) {
    for (HWND window = ::GetTopWindow(nullptr); window; window = ::GetWindow(window, GW_HWNDNEXT)) {
        if (window == first) return true;
        if (window == second) return false;
    }
    return false;
}
HWND fixture(const wchar_t* title, bool topmost) {
    return ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (topmost ? WS_EX_TOPMOST : 0),
        L"STATIC", title, WS_POPUP | WS_VISIBLE, -31000, -31000, 16, 16,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}
}

int wmain() {
    musicboard::test::Registry registry;
    const HWND foreground = ::GetForegroundWindow();
    HWND panel = fixture(L"Owned layer test panel", true);
    HWND orb = fixture(L"Owned layer test orb", true);
    HWND external = fixture(L"Owned layer test external blocker", true);
    registry.check(panel && orb && external, "native layer fixtures created");
    if (!panel || !orb || !external) return 1;
    RECT before{}; ::GetWindowRect(panel, &before);
    musicboard::Failure failure;
    registry.check(musicboard::restore_pinned_window_layers(orb, panel, true, failure), "restore pinned layers succeeds");
    registry.check(above(orb, panel) && above(panel, external), "orb and pinned panel recover above external blocker");
    RECT after{}; ::GetWindowRect(panel, &after);
    registry.check(::EqualRect(&before, &after), "restoration preserves window placement");
    registry.check(::GetForegroundWindow() == foreground, "restoration does not take foreground focus");
    for (int i = 0; i < 20; ++i)
        registry.check(musicboard::restore_pinned_window_layers(orb, panel, true, failure) && above(orb, panel), "repeated restoration keeps stable ordering");
    registry.check(::SetWindowPos(panel, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE, "fixture panel can unpin");
    registry.check(musicboard::restore_pinned_window_layers(orb, panel, false, failure), "restore respects unpinned panel");
    registry.check((::GetWindowLongPtrW(panel, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0, "unpinned panel stays unpinned");
    ::DestroyWindow(external); ::DestroyWindow(orb); ::DestroyWindow(panel);
    return registry.finish("window_layers_tests");
}
