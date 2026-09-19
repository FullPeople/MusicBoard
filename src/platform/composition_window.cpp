#include "platform/composition_window.h"

namespace musicboard {
namespace {

constexpr wchar_t kWindowClassName[] = L"MusicBoard.CompositionWindow";

}  // namespace

const wchar_t* CompositionWindow::window_class_name() const noexcept {
    return kWindowClassName;
}

bool CompositionWindow::create(const Options& options, RenderCallback callback, Failure& failure) {
    panel_options_ = options;
    return TransparentWindow::create(options.window, std::move(callback), failure);
}

LRESULT CompositionWindow::hit_test_client(POINT client_point) const {
    if (panel_options_.titlebar_logical_height <= 0) {
        return HTCLIENT;
    }
    // client_point 是物理像素；标题栏高度是逻辑像素，要换算过来比。
    // 不换算的话在 150% 缩放下拖动热区会只有实际标题栏的三分之二高。
    const int titlebar_physical = scale_for_dpi(panel_options_.titlebar_logical_height, dpi());
    if (client_point.y >= 0 && client_point.y < titlebar_physical) {
        return HTCAPTION;
    }
    return HTCLIENT;
}

}  // namespace musicboard
