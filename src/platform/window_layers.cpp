#include "platform/window_layers.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace musicboard {
bool restore_pinned_window_layers(HWND orb, HWND panel, bool panel_pinned, Failure& failure) {
    struct Layer { HWND window; int rank; bool owned; };
    std::vector<Layer> layers;
    const DWORD owner = ::GetCurrentProcessId();
    for (HWND window = ::GetTopWindow(nullptr); window; window = ::GetWindow(window, GW_HWNDNEXT)) {
        if (!::IsWindowVisible(window)) continue;
        DWORD process = 0;
        ::GetWindowThreadProcessId(window, &process);
        wchar_t text[128]{};
        ::GetClassNameW(window, text, static_cast<int>(std::size(text)));
        const std::wstring_view name(text);
        if ((name == L"#32768" || name == L"#32770") && process == owner) return true;
        if (name == L"MusicBoard.LinkImport" &&
            (::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
            layers.push_back({window, 5, process == owner}); continue;
        }
        const bool our_orb = window == orb && process == owner;
        const bool our_panel = window == panel && process == owner && panel_pinned;
        if (our_orb || our_panel) { layers.push_back({window, our_orb ? 6 : 4, true}); continue; }
        if (name == L"MusicBoard.OrbWindow") { layers.push_back({window, 6, false}); continue; }
        if (name == L"MusicBoard.CompositionWindow" &&
            (::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
            layers.push_back({window, 4, false}); continue;
        }
        if (!name.starts_with(L"DesktopDice.")) continue;
        int rank = 4;
        if (name == L"DesktopDice.FloatingBubble") rank = 0;
        if (name == L"DesktopDice.FloatingBubbleInput") rank = 1;
        if (name == L"DesktopDice.RenderOverlay") rank = 2;
        if (name == L"DesktopDice.ResultEffects") rank = 3;
        if (name == L"DesktopDice.QuickInput") rank = 5;
        if (name == L"DesktopDice.FloatingOrb") rank = 6;
        layers.push_back({window, rank, false});
    }
    std::stable_sort(layers.begin(), layers.end(), [](const Layer& a, const Layer& b) { return a.rank > b.rank; });
    HWND anchor = HWND_TOPMOST;
    HDWP transaction = nullptr;
    bool predecessor_moved = false;
    for (const auto& layer : layers) {
        if (!::IsWindow(layer.window) || !::IsWindowVisible(layer.window)) continue;
        HWND previous = ::GetWindow(layer.window, GW_HWNDPREV);
        while (previous && !::IsWindowVisible(previous)) previous = ::GetWindow(previous, GW_HWNDPREV);
        const bool misplaced = predecessor_moved || (::GetWindowLongPtrW(layer.window, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0 ||
            (anchor == HWND_TOPMOST ? previous != nullptr : previous != anchor);
        if (layer.owned && misplaced) {
            if (!transaction) transaction = ::BeginDeferWindowPos(static_cast<int>(layers.size()));
            if (!transaction) { failure.set("ui.window_layers.begin", "BeginDeferWindowPos Win32=" + std::to_string(::GetLastError())); return false; }
            transaction = ::DeferWindowPos(transaction, layer.window, anchor, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            if (!transaction) { failure.set("ui.window_layers.move", "DeferWindowPos Win32=" + std::to_string(::GetLastError())); return false; }
            predecessor_moved = true;
        }
        anchor = layer.window;
    }
    if (transaction && !::EndDeferWindowPos(transaction)) { failure.set("ui.window_layers.commit", "EndDeferWindowPos Win32=" + std::to_string(::GetLastError())); return false; }
    return true;
}
}
