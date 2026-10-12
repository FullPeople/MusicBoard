#include "platform/orb_window.h"

#include <windowsx.h>

#include <cmath>

namespace musicboard {
namespace {

constexpr wchar_t kWindowClassName[] = L"MusicBoard.OrbWindow";

[[nodiscard]] int distance_squared(POINT a, POINT b) noexcept {
    const int dx = a.x - b.x;
    const int dy = a.y - b.y;
    return dx * dx + dy * dy;
}

}  // namespace

const wchar_t* OrbWindow::window_class_name() const noexcept {
    return kWindowClassName;
}

DWORD OrbWindow::window_style() const noexcept {
    // 没有标题栏、不进任务栏；球靠拖动自己移动，不需要系统的边框行为
    return WS_POPUP;
}

DWORD OrbWindow::window_ex_style() const noexcept {
    // TOOLWINDOW：不进任务栏与 Alt+Tab。NOACTIVATE：点击不抢焦点。
    // ball 是常驻的，抢焦点会让用户正在打字的窗口失去输入。
    return WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
}

bool OrbWindow::create(const Options& options, RenderCallback callback, Failure& failure) {
    Options orb_options = options;
    orb_options.window.logical_width = kOrbWindowLogicalSize;
    orb_options.window.logical_height = kOrbWindowLogicalSize;
    orb_options.window.topmost = true;
    orb_options.window.tool_window = true;
    orb_options.window.no_activate = true;
    if (options.initial_x != CW_USEDEFAULT) {
        orb_options.window.initial_x = options.initial_x;
        orb_options.window.initial_y = options.initial_y;
    }
    return TransparentWindow::create(orb_options.window, std::move(callback), failure);
}

POINT OrbWindow::circle_center_physical() const noexcept {
    // 圆心在客户区正中（外圈环的圆心）。辉光余量是四周等宽的，所以正中就是圆心。
    const int half = physical_width() / 2;
    return POINT{half, physical_height() / 2};
}

int OrbWindow::circle_radius_physical() const noexcept {
    // 命中半径取进度环的外半径：环也是球的一部分，悬停/点击它该有反应
    return scale_for_dpi(kOrbRingOuterDiameter / 2, dpi());
}

LRESULT OrbWindow::hit_test_client(POINT client_point) const {
    const POINT center = circle_center_physical();
    const int radius = circle_radius_physical();
    if (distance_squared(client_point, center) <= radius * radius) {
        return HTCLIENT;
    }
    // 圆外一律让路。少了这一句，桌面左上角会多出一块看不见的方形
    // 吃掉点击 —— 而且因为它是最顶层窗口，用户完全不知道为什么点不动。
    return HTTRANSPARENT;
}

void OrbWindow::move_to(POINT screen_top_left) {
    if (handle() == nullptr) {
        return;
    }
    ::SetWindowPos(handle(), HWND_TOPMOST, screen_top_left.x, screen_top_left.y, 0, 0,
                   SWP_NOSIZE | SWP_NOACTIVATE);
}

void OrbWindow::on_moved() {
    if (on_moved_ && handle() != nullptr) {
        RECT rect{};
        ::GetWindowRect(handle(), &rect);
        on_moved_(POINT{rect.left, rect.top});
    }
}

void OrbWindow::on_size_changed(UINT /*physical_width*/, UINT /*physical_height*/) {
    // 尺寸变了圆心就变了，不需要额外缓存 —— circle_center_physical() 现算。
}

int OrbWindow::action_zone(POINT client) const noexcept {
    const POINT center = circle_center_physical();
    const int radius = scale_for_dpi(kOrbLogicalDiameter / 2, dpi());
    if (distance_squared(client, center) > radius * radius) return 0;
    return client.y < center.y + static_cast<int>(static_cast<float>(radius) * 0.4f) ? 1 : 2;
}

bool OrbWindow::on_message(UINT message, WPARAM, LPARAM lparam, LRESULT& result) {
    switch (message) {
        case WM_SETCURSOR: {
            POINT client{}; ::GetCursorPos(&client); ::ScreenToClient(handle(), &client);
            ::SetCursor(::LoadCursorW(nullptr, action_zone(client) ? IDC_HAND : IDC_ARROW));
            result = TRUE; return true;
        }
        case WM_MOUSELEAVE:
            if (on_hover_) on_hover_(0);
            result = 0; return true;
        case WM_LBUTTONDOWN: {
            pressed_zone_ = action_zone(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            // 光标位置用 GetCursorPos 而不是 lparam：拖动过程中 lparam 是相对坐标，
            // 窗口一移动它就跟着变，累计起来会飘。
            POINT cursor{};
            ::GetCursorPos(&cursor);
            RECT rect{};
            ::GetWindowRect(handle(), &rect);

            drag_origin_cursor_ = cursor;
            drag_origin_window_ = POINT{rect.left, rect.top};
            dragging_ = true;
            moved_while_pressed_ = false;
            ::SetCapture(handle());
            result = 0;
            return true;
        }

        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, handle(), 0};
            ::TrackMouseEvent(&track);
            if (on_hover_) on_hover_(action_zone(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}));
            if (!dragging_) {
                break;
            }
            POINT cursor{};
            ::GetCursorPos(&cursor);
            const int dx = cursor.x - drag_origin_cursor_.x;
            const int dy = cursor.y - drag_origin_cursor_.y;
            if (!moved_while_pressed_ &&
                dx * dx + dy * dy >= kDragThresholdPhysical * kDragThresholdPhysical) {
                moved_while_pressed_ = true;
            }
            if (moved_while_pressed_) {
                ::SetWindowPos(handle(), HWND_TOPMOST, drag_origin_window_.x + dx,
                               drag_origin_window_.y + dy, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            }
            result = 0;
            return true;
        }

        case WM_LBUTTONUP: {
            if (!dragging_) {
                break;
            }
            dragging_ = false;
            ::ReleaseCapture();
            if (moved_while_pressed_) {
                // 是拖动，不是点击 —— 存位置，不展开面板
                on_moved();
            } else if (pressed_zone_ == action_zone(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
                if (pressed_zone_ == 1 && on_activate_) on_activate_();
                else if (pressed_zone_ == 2 && on_playback_) on_playback_();
            }
            result = 0;
            return true;
        }

        case WM_CAPTURECHANGED: {
            // 系统可能在 WM_LBUTTONUP 之前就收走捕获（比如按了 Esc）。
            // 不重置的话 dragging_ 会一直是 true，下次移动鼠标球就自己跑。
            dragging_ = false;
            result = 0;
            return true;
        }

        case WM_RBUTTONUP: {
            POINT cursor{};
            ::GetCursorPos(&cursor);
            if (on_context_menu_) {
                on_context_menu_(cursor);
            }
            result = 0;
            return true;
        }

        default:
            break;
    }
    return false;
}

}  // namespace musicboard
