#pragma once

#include "platform/transparent_window.h"

namespace musicboard {

// ============================================================================
// 主面板窗口
//
// 在 TransparentWindow 之上只多做一件事：**自绘标题栏的拖动走系统**。
// `WM_NCHITTEST` 命中标题栏区域时返回 HTCAPTION，于是拖动、双击最大化、
// 窗口吸附全部由系统提供，不用自己算鼠标偏移。
//
// 这是 Godot 版最刺眼的问题的正解：那里透明窗口配了却没生效，
// 系统标题栏与自绘标题栏同时存在。这里根本没有系统标题栏（WS_POPUP），
// 而拖动的手感又完全是系统的。
// ============================================================================

// 不是 final：正式程序需要派生出 PanelWindow 来处理自己的消息
// （消息处理本来就是窗口的派生行为，用回调会让消息路径变成一次隐式间接调用）。
class CompositionWindow : public TransparentWindow {
public:
    struct Options final {
        TransparentWindow::Options window;
        // 自绘标题栏的高度（逻辑像素）。<= 0 表示不会有标题栏拖动热区。
        int titlebar_logical_height = 42;
    };

    [[nodiscard]] bool create(const Options& options, RenderCallback callback, Failure& failure);

protected:
    [[nodiscard]] const wchar_t* window_class_name() const noexcept override;
    [[nodiscard]] LRESULT hit_test_client(POINT client_point) const override;

private:
    Options panel_options_;
};

}  // namespace musicboard
