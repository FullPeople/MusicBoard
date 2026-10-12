#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "platform/transparent_window.h"

namespace musicboard {

// ============================================================================
// 悬浮球窗口
//
// 一枚常驻的圆形悬浮窗：拖动移动、左键展开面板、右键菜单、播放时自转。
//
// 与主面板的差别（这就是它值得单独一个类的原因）：
//
//   1. **圆形命中测试。** 窗口是方的（球 + 四周围给辉光的余量），
//      但只有圆内那一片属于它。圆外的点返回 HTTRANSPARENT，
//      点击直接落到桌面或下面的窗口上 ——
//      否则桌子角上会有一块看不见的方形在吃掉点击。
//      这是覆盖层最容易让人恼火的问题，必须从第一版就做对。
//
//   2. **拖动与点击要分得开。** 整颗球既是拖动柄又是"展开面板"按钮。
//      按下后位移超过阈值才算拖动；没超过就按点击处理。
//      不做这个区分的话，手抖一下就会以为用户想展开。
//
//   3. **不出现在任务栏与 Alt+Tab**（WS_EX_TOOLWINDOW），
//      点击也不抢焦点（WS_EX_NOACTIVATE）。跑团现场点一下球就把用户
//      正在打字的窗口顶掉，是最烦的事。
//
//   4. **位置要记住。** 拖动结束（WM_EXITSIZEMOVE）与关闭时通过回调交出去。
//
// 关于技术路线：设计稿原本写的是 `WS_EX_LAYERED` + `UpdateLayeredWindow`。
// 实现改成了与主面板同一条 DirectComposition 路线，理由是：
//   · 逐像素透明这件事已经在阶段 2 被实测过（含四个角的 alpha=0），
//     再用一套新技术等于把已验证的东西换成一个没验证的；
//   · UpdateLayeredWindow 每帧要把 GPU 的结果读回 CPU 位图，
//     而球是每帧都在转的（60 fps）。124px 的球读回量不大，
//     但那是白付的成本，且两套渲染路径迟早会漂。
//   这个偏离记在验收记录里，不是悄悄改的。
// ============================================================================

// 悬浮球的设计尺寸（逻辑像素）。取自 design/DESIGN.md：
// 球 104px，进度环外径 124px，环宽 4px；外面再留一圈给辉光。
inline constexpr int kOrbLogicalDiameter = 104;
inline constexpr int kOrbRingOuterDiameter = 124;
inline constexpr float kOrbRingWidth = 4.0f;
inline constexpr int kOrbGlowMargin = 30;
inline constexpr int kOrbWindowLogicalSize = kOrbRingOuterDiameter + kOrbGlowMargin * 2;

class OrbWindow final : public TransparentWindow {
public:
    struct Options final {
        TransparentWindow::Options window;
        // 初始位置（物理像素）。CW_USEDEFAULT 表示由系统放。
        int initial_x = CW_USEDEFAULT;
        int initial_y = CW_USEDEFAULT;
    };

    // 左键单击（不是拖动）。界面上对应"展开面板"。
    using ActivateCallback = std::function<void()>;
    // 右键单击。参数是屏幕坐标，用来弹出菜单。
    using ContextMenuCallback = std::function<void(POINT screen_point)>;
    // 拖动结束。参数是窗口左上角的屏幕坐标，用来持久化。
    using MovedCallback = std::function<void(POINT top_left)>;

    [[nodiscard]] bool create(const Options& options, RenderCallback callback, Failure& failure);

    void set_activate_callback(ActivateCallback callback) { on_activate_ = std::move(callback); }
    void set_playback_callback(ActivateCallback callback) { on_playback_ = std::move(callback); }
    void set_hover_callback(std::function<void(int)> callback) { on_hover_ = std::move(callback); }
    [[nodiscard]] int action_zone(POINT client) const noexcept;
    void set_context_menu_callback(ContextMenuCallback callback) {
        on_context_menu_ = std::move(callback);
    }
    void set_moved_callback(MovedCallback callback) { on_moved_ = std::move(callback); }

    // 把球移到指定的屏幕坐标（物理像素）。用于恢复上次的位置。
    void move_to(POINT screen_top_left);

    // 命中测试的真实实现。**公开出来是为了能被直接断言** ——
    // "圆外返回 HTTRANSPARENT"这件事靠模拟鼠标点桌面很难可靠地测，
    // 直接问窗口"这个点算谁的"才是准的（阶段 2 的标题栏自检同理）。
    [[nodiscard]] LRESULT hit_test_client(POINT client_point) const override;

    // 拖动判定阈值（物理像素）。超过它才算拖动。
    static constexpr int kDragThresholdPhysical = 4;

protected:
    [[nodiscard]] const wchar_t* window_class_name() const noexcept override;
    [[nodiscard]] DWORD window_style() const noexcept override;
    [[nodiscard]] DWORD window_ex_style() const noexcept override;
    [[nodiscard]] bool on_message(UINT message, WPARAM wparam, LPARAM lparam,
                                  LRESULT& result) override;
    void on_moved() override;
    void on_size_changed(UINT physical_width, UINT physical_height) override;

private:
    // 球的圆心与半径（物理像素，客户区坐标）
    [[nodiscard]] POINT circle_center_physical() const noexcept;
    [[nodiscard]] int circle_radius_physical() const noexcept;

    ActivateCallback on_activate_;
    ActivateCallback on_playback_;
    std::function<void(int)> on_hover_;
    int pressed_zone_ = 0;
    ContextMenuCallback on_context_menu_;
    MovedCallback on_moved_;

    bool dragging_ = false;
    bool moved_while_pressed_ = false;
    POINT drag_origin_cursor_{};      // 按下时的屏幕坐标
    POINT drag_origin_window_{};      // 按下时的窗口左上角
};

}  // namespace musicboard
