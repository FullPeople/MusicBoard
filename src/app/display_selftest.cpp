#include "app/application.h"

#include <cmath>
#include <cstdio>

namespace musicboard::app {
extern HWND panel_window_handle();

int Application::self_test_display() {
    int total = 0;
    int failures = 0;
    const auto check = [&](bool ok, const char* label, UINT dpi, long long actual) {
        ++total;
        if (!ok) ++failures;
        std::printf("%s dpi=%u %s actual=%lld\n", ok ? "PASS" : "FAIL", dpi, label, actual);
    };
    const HWND window = panel_window_handle();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (loading_ && std::chrono::steady_clock::now() < deadline) { on_frame(); ::Sleep(5); }
    check(!loading_, "library ready", 96, loading_);
    if (loading_) return 1;
    check(::AreDpiAwarenessContextsEqual(::GetWindowDpiAwarenessContext(window),
          DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "native window is PerMonitorV2", 96,
          static_cast<int>(::GetAwarenessFromDpiAwarenessContext(::GetWindowDpiAwarenessContext(window))));
    check(!product_.show_all_categories, "new profile defaults All off", 96, product_.show_all_categories);
    check(state_.bgm_library.chips == bgm_.categories && state_.sfx_library.chips == sfx_.categories,
          "default labels contain only actual categories", 96, state_.bgm_library.chips.size());
    const UINT original_dpi = ::GetDpiForWindow(window);
    RECT original{}; ::GetWindowRect(window, &original);
    // Inject valid notifications in-process (RECT pointers cannot cross processes).
    // This exercises the real renderer and WindowProc; it is not a hardware DPI test.
    for (const UINT dpi : {96u, 120u, 144u, 168u, 192u, 96u}) {
        const LONG left = dpi == 144 ? -400 : original.left;
        RECT suggested{left, original.top,
            left + static_cast<LONG>(std::lround(metrics_.width * dpi / 96.0f)),
            original.top + static_cast<LONG>(std::lround(metrics_.height * dpi / 96.0f))};
        ::SendMessageW(window, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
        check(slider_drag_target_ < 0 && !dragging_ && !seeking_ && ::GetCapture() != window,
              "DPI change cancels stale pointer capture", dpi, slider_drag_target_);
        on_frame();
        const auto physical = [dpi](ui::Point point) {
            return POINT{static_cast<LONG>(std::lround(point.x * dpi / 96.0f)),
                         static_cast<LONG>(std::lround(point.y * dpi / 96.0f))};
        };
        const auto click = [&](ui::Point point, int expected) {
            const POINT client = physical(point);
            const LPARAM packed = MAKELPARAM(client.x, client.y);
            ::SendMessageW(window, WM_MOUSEMOVE, 0, packed);
            const int actual = hit_.hovered();
            check(actual == expected, "physical control hit", dpi, actual);
            if (actual != expected) return;
            ::SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, packed);
            ::SendMessageW(window, WM_LBUTTONUP, 0, packed);
            on_frame();
        };
        for (const auto& area : {layout_.title_cn, layout_.title_en}) {
            POINT screen = physical(area.center()); ::ClientToScreen(window, &screen);
            const auto result = ::SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(screen.x, screen.y));
            check(result == HTCLIENT, "language tabs accept native titlebar input", dpi, result);
        }
        click(layout_.title_en.center(), 12);
        check(state_.english && state_.title == "MusicBoard" && state_.bgm_library.search_placeholder == "Search music…",
              "English changes authored labels immediately", dpi, state_.english);
        check(state_.bgm_library.chips == bgm_.categories, "English preserves user category names", dpi, state_.bgm_library.chips.size());
        const auto english_options = layout_.option_rects(layout_.bgm_deck_line, {"Loop", "Fade", "Mix"});
        for (std::size_t index=0; index<english_options.size(); ++index) {
            const POINT client = physical(english_options[index].center());
            ::SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(client.x,client.y));
            check(hit_.hovered() == 104 + static_cast<int>(index), "English control labels match hit regions", dpi, hit_.hovered());
        }
        ProductSettingsStore language_store; ProductSettingsLoadResult saved_language; Failure language_failure;
        const bool saved = language_store.load_or_initialize(settings_path_, saved_language, language_failure);
        check(saved && saved_language.settings.ui_language == Language::english, "English is persisted", dpi, saved);
        click(layout_.title_cn.center(), 11);
        check(!state_.english && state_.title == "音乐板", "Chinese restores immediately", dpi, state_.english);
        for (const auto& area : {layout_.title_pin, layout_.title_minimize, layout_.title_close}) {
            POINT screen = physical(area.center()); ::ClientToScreen(window, &screen);
            const LRESULT result = ::SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(screen.x, screen.y));
            check(result == HTCLIENT, "title button accepts native input", dpi, result);
        }
        const POINT search = physical(layout_.bgm_search_input.center());
        ::SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(search.x, search.y));
        check(hit_.hovered() == 300, "physical search hover", dpi, hit_.hovered());
        // Avoid dispatching a misrouted click into a dialog when reproducing the bug.
        if (hit_.hovered() == 300) {
            ::SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(search.x, search.y));
            ::SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(search.x, search.y));
            check(bgm_.search_focused, "physical search click", dpi, bgm_.search_focused);
        }
        click(layout_.sfx_search_input.center(), 301);
        check(sfx_.search_focused && !bgm_.search_focused, "SFX search focuses correct column", dpi, sfx_.search_focused);
        const bool pinned = product_.panel.topmost;
        click(layout_.title_pin.center(), 3);
        check(product_.panel.topmost != pinned &&
              ((::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) == !pinned,
              "pin changes actual window style", dpi, product_.panel.topmost);
        click(layout_.title_pin.center(), 3);
        const auto slider = ui::PanelLayout::volume_track(layout_.bgm_volume_slider);
        for (const int percent : {25, 75}) {
            click({slider.x + slider.width * percent / 100.0f, slider.center().y}, 103);
            check(std::abs(static_cast<int>(product_.bgm_volume_percent) - percent) <= 1,
                  "volume follows physical pointer", dpi, product_.bgm_volume_percent);
        }
        click(layout_.busbar_show_all.center(), 10);
        check(product_.show_all_categories && state_.bgm_library.chips.front() == "全部" &&
              state_.sfx_library.chips.front() == "全部", "switch adds All to both columns", dpi, product_.show_all_categories);
        for (const bool sfx : {false, true}) {
            auto& column = sfx ? sfx_ : bgm_;
            const auto& library = sfx ? state_.sfx_library : state_.bgm_library;
            const auto rects = layout_.chip_rects(sfx ? layout_.sfx_chips : layout_.bgm_chips, library.chip_widths);
            if (rects.size() > 2) {
                click(rects[2].center(), sfx ? 502 : 402);
                check(column.active_chip == 1, "category index includes optional All offset", dpi, column.active_chip);
            }
        }
        const int bgm_selected = bgm_.active_chip, sfx_selected = sfx_.active_chip;
        click(layout_.busbar_show_all.center(), 10);
        check(!product_.show_all_categories && bgm_.active_chip == bgm_selected && sfx_.active_chip == sfx_selected,
              "hiding All preserves specific category selection", dpi, bgm_.active_chip);
        check(state_.bgm_library.chips == bgm_.categories && state_.sfx_library.chips == sfx_.categories,
              "hidden All leaves no stale chip labels", dpi, state_.bgm_library.chips.size());
        for (const bool sfx : {false, true}) {
            auto& column = sfx ? sfx_ : bgm_;
            const auto& library = sfx ? state_.sfx_library : state_.bgm_library;
            const auto rects = layout_.chip_rects(sfx ? layout_.sfx_chips : layout_.bgm_chips, library.chip_widths);
            if (!rects.empty()) {
                click(rects[0].center(), sfx ? 500 : 400);
                check(column.active_chip == 0, "first visible chip selects first actual category", dpi, column.active_chip);
            }
        }
        click({layout_.sfx_mode.x + 120, layout_.sfx_mode.center().y}, 7);
        check(product_.sfx_dj_mode, "DJ mode uses scaled button location", dpi, product_.sfx_dj_mode);
        if (!sfx_.visible.empty()) {
            click(layout_.card_rect(layout_.sfx_grid, 0, sfx_.scroll.offset()).center(), 2000);
            const auto index = sfx_.visible.front();
            check(engine_.slot_state(AudioBus::sfx, sfx_slot_count + index).in_use,
                  "scaled DJ pad starts intended voice", dpi, engine_.slot_state(AudioBus::sfx, sfx_slot_count + index).in_use);
        }
        clear_playback(true);
        click({layout_.sfx_mode.x + 30, layout_.sfx_mode.center().y}, 7);
        click(layout_.busbar_fold.center(), 9);
        RECT folded{}; ::GetClientRect(window, &folded);
        check(metrics_.sfx_collapsed && std::abs(folded.right - static_cast<LONG>(std::lround(746.0f*dpi/96.0f))) <= 1,
              "folded window uses current DPI", dpi, folded.right);
        click(layout_.busbar_show_all.center(), 10);
        click(layout_.busbar_show_all.center(), 10);
        check(layout_.busbar_show_all.right() < layout_.busbar_library.x &&
              layout_.busbar_library.right() < layout_.busbar_change.x,
              "folded footer controls and path do not overlap", dpi, static_cast<long long>(layout_.busbar_library.width));
        click(layout_.busbar_fold.center(), 9);
        // Start a gesture and cross the DPI boundary before releasing it.
        const POINT held = physical(ui::PanelLayout::volume_track(layout_.bgm_volume_slider).center());
        ::SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(held.x, held.y));
    }
    ::SendMessageW(window, WM_DPICHANGED, MAKELONG(original_dpi, original_dpi), reinterpret_cast<LPARAM>(&original));
    set_panel_visible(true); on_frame();
    const UINT orb_dpi = orb_.dpi();
    const auto x = static_cast<LONG>(std::lround(92.0f * orb_dpi / 96.0f));
    const auto y = static_cast<LONG>(std::lround(85.0f * orb_dpi / 96.0f));
    ::SendMessageW(orb_.handle(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    ::SendMessageW(orb_.handle(), WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    check(!::IsWindowVisible(window), "orb hides panel before click returns", orb_dpi, ::IsWindowVisible(window));
    // Reopening during the old 420ms close interval must not be hidden by a stale timer.
    ::SendMessageW(orb_.handle(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    ::SendMessageW(orb_.handle(), WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    ::Sleep(450); on_frame();
    check(::IsWindowVisible(window) && panel_visible_, "rapid reopen survives close interval", orb_dpi, ::IsWindowVisible(window));
    ProductSettingsStore store; ProductSettingsLoadResult loaded; Failure failure;
    const bool read = store.load_or_initialize(settings_path_, loaded, failure);
    check(read && !loaded.settings.show_all_categories, "All off persists to disk", original_dpi, loaded.settings.show_all_categories);
    set_show_all_categories(true);
    bgm_.active_chip = -1; sfx_.active_chip = -1;
    rebuild_visible(bgm_, false); rebuild_visible(sfx_, true);
    set_show_all_categories(false);
    check((bgm_.categories.empty() || bgm_.active_chip == 0) && (sfx_.categories.empty() || sfx_.active_chip == 0),
          "hiding active All selects first actual category", original_dpi, bgm_.active_chip);
    std::printf("DISPLAY_SELFTEST %d/%d passed, %d failed\n", total - failures, total, failures);
    return failures ? 1 : 0;
}
}  // namespace musicboard::app
