#pragma once

#include <windows.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <wrl/client.h>

#include <filesystem>
#include <functional>
#include <string>

#include "platform/dpi.h"

namespace musicboard {

// ============================================================================
// 透明窗口基类：一块由 DirectComposition 合成的、真正逐像素透明的绘制表面
//
// 为什么要有这个基类：主面板与悬浮球是两个窗口，但它们踩的是同一批坑 ——
//   · 少了 WS_EX_NOREDIRECTIONBITMAP，DirectComposition 的内容根本看不见
//   · render target 的 DPI 必须钉在 96，缩放由窗口按显示器 DPI 做；
//     让 D2D 也按 DPI 缩一次，布局会缩两遍
//   · 必须是预乘 alpha，否则半透明边缘发白
//   · 窗口不能有背景刷，否则启动瞬间闪一下白
//   · 释放顺序：DComp 相关要先于 D2D 设备，否则 surface 还引用着 d2d bitmap
//
// 这些约定只该有一份。两个窗口的差别只在窗口样式与消息处理，那部分留给子类。
//
// ⚠️ 这条是实测结论，不是理论：Godot 版最刺眼的问题就是"透明窗口配了却没生效，
//    系统标题栏与自绘标题栏同时存在"。上面五条里任何一条漏掉，症状都是
//    "窗口在，但看不到内容"或"边缘一圈白边"，而且报错位置离原因很远。
//
// 坐标约定（本类最重要的一条）：
//
//   绘制回调拿到的是**逻辑坐标**（96 DPI 下的像素）。窗口负责按当前显示器 DPI
//   设置缩放变换，并把 D2D render target 的 DPI 固定为 96。
//   于是回调里永远不需要考虑物理像素。
// ============================================================================

// 传给绘制回调的上下文
struct DrawContext final {
    float logical_width = 0.0f;    // 96 DPI 下的逻辑宽
    float logical_height = 0.0f;
    UINT dpi = kBaseDpi;
};

class TransparentWindow {
public:
    // 在逻辑坐标下绘制。render_target 的 DPI 已经固定为 96，直接用逻辑值画。
    using RenderCallback =
        std::function<void(ID2D1RenderTarget& render_target, const DrawContext& context)>;

    struct Options final {
        std::wstring title;
        int logical_width = 0;
        int logical_height = 0;
        // 创建时的位置（物理像素）。CW_USEDEFAULT 表示交给系统。
        int initial_x = CW_USEDEFAULT;
        int initial_y = CW_USEDEFAULT;
        bool topmost = false;
        bool initially_visible = true;
        // 不出现在任务栏与 Alt+Tab 里。悬浮球必须是这个。
        bool tool_window = false;
        // 点击不让窗口获得焦点。悬浮球也是这个 ——
        // 点一下球就把用户正在打字的窗口顶掉，是跑团现场最烦的事。
        bool no_activate = false;
    };

    TransparentWindow() = default;
    virtual ~TransparentWindow();

    TransparentWindow(const TransparentWindow&) = delete;
    TransparentWindow& operator=(const TransparentWindow&) = delete;

    [[nodiscard]] bool create(const Options& options, RenderCallback callback, Failure& failure);
    // 整个窗口的不透明度（0..1）。走 DirectComposition 的 visual 属性 ——
    // 由合成器做淡入淡出，不必每帧重画一张内容一模一样的位图。
    // 面板显隐动效需要它：只靠缩放，收起时会显得"啪"地消失。
    void set_opacity(float opacity);


    // 跑消息循环直到窗口关闭。返回进程退出码。
    int run_message_loop();

    // 按当前尺寸重画一帧。悬浮球每帧都要调（它在转），主面板一般不需要。
    [[nodiscard]] bool redraw_frame(Failure& failure);

    // 把当前一帧按逻辑尺寸渲染到 PNG。
    //
    // 这是给验收用的：透明窗口在桌面上的观感靠人眼，但"中文渲染对不对、
    // 圆角与配色是不是设计稿的样子、alpha 通道对不对"可以先靠文件自证。
    // 它走的是同一份绘制回调，所以画出来的就是屏幕上那一帧。
    [[nodiscard]] bool save_frame_png(const std::filesystem::path& path, Failure& failure) const;

    [[nodiscard]] HWND handle() const noexcept { return window_; }
    [[nodiscard]] UINT dpi() const noexcept { return dpi_; }
    [[nodiscard]] DrawContext draw_context() const noexcept;
    [[nodiscard]] RECT window_rect() const noexcept;
    // 逻辑尺寸 → 物理像素
    [[nodiscard]] int physical_width() const noexcept { return static_cast<int>(physical_width_); }
    [[nodiscard]] int physical_height() const noexcept { return static_cast<int>(physical_height_); }

protected:
    // 子类的窗口类名。必须唯一 —— 两个窗口用同一个类名会把 WNDPROC 换掉。
    //
    // 注意：这个函数**不能在析构里调**。虚函数在基类析构期间已经不指向子类，
    // 而纯虚函数根本没有定义 —— 链接期报"无法解析的外部符号"就是这么来的。
    // 所以 create() 会把它抄进 registered_class_name_，析构用那一份。
    [[nodiscard]] virtual const wchar_t* window_class_name() const noexcept = 0;
    [[nodiscard]] virtual DWORD window_style() const noexcept { return WS_POPUP; }
    [[nodiscard]] virtual DWORD window_ex_style() const noexcept {
        return WS_EX_NOREDIRECTIONBITMAP;
    }

    // 客户区的命中测试。返回 HTTRANSPARENT 表示"这个点不属于我，交给下面的窗口"。
    // 默认整块都是我的。
    [[nodiscard]] virtual LRESULT hit_test_client(POINT /*client_point*/) const {
        return HTCLIENT;
    }

    // 子类自己的消息。返回 true 表示已处理。
    virtual bool on_message(UINT /*message*/, WPARAM /*wparam*/, LPARAM /*lparam*/,
                            LRESULT& /*result*/) {
        return false;
    }

    // 窗口被用户移动/缩放结束（WM_EXITSIZEMOVE）。悬浮球在这里存位置。
    virtual void on_moved() {}
    // 窗口位置/尺寸变化之后（每次 WM_SIZE 之后），子类可以在这里同步状态
    virtual void on_size_changed(UINT /*physical_width*/, UINT /*physical_height*/) {}

    [[nodiscard]] const Options& options() const noexcept { return options_; }

    // 子类的附加窗口样式（在 create 之前设置才生效）
    void set_extra_window_styles(DWORD style, DWORD ex_style) noexcept {
        extra_style_ = style;
        extra_ex_style_ = ex_style;
    }

private:
    static LRESULT CALLBACK window_proc_thunk(HWND window, UINT message, WPARAM wparam,
                                             LPARAM lparam);
    LRESULT window_proc(UINT message, WPARAM wparam, LPARAM lparam);

    [[nodiscard]] bool create_device_resources(Failure& failure);
    [[nodiscard]] bool create_composition_target(Failure& failure);
    [[nodiscard]] bool resize_composition_surface(UINT physical_width, UINT physical_height,
                                                 Failure& failure);
    [[nodiscard]] bool paint_composition_surface(Failure& failure);
    void discard_composition_surface() noexcept;
    [[nodiscard]] bool draw_into(ID2D1RenderTarget& render_target, Failure& failure) const;
    [[nodiscard]] DrawContext make_draw_context() const noexcept;

    Options options_;
    RenderCallback render_callback_;
    HWND window_ = nullptr;
    UINT dpi_ = kBaseDpi;
    DWORD extra_style_ = 0;
    DWORD extra_ex_style_ = 0;
    // create() 时抄下来的类名。析构要用它 —— 那时虚函数已经不指向子类了。
    const wchar_t* registered_class_name_ = nullptr;

    // 物理像素尺寸（= 逻辑尺寸 × dpi / 96）
    UINT physical_width_ = 0;
    UINT physical_height_ = 0;

    // 一个 D3D11 设备贯穿 D2D 与 DirectComposition。
    // 不各建一套 —— 那样会多一份显存与一次跨设备拷贝。
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2d_context_;

    Microsoft::WRL::ComPtr<IDCompositionDevice> composition_device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> composition_target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> composition_visual_;
    // 面板显隐要用 SetOpacity —— 它声明在 IDCompositionVisual3 上
    // （Visual / Visual2 都没有这个方法，翻过 dcomp.h）。懒取，见 .cpp。
    Microsoft::WRL::ComPtr<IDCompositionVisual3> composition_visual3_;
    Microsoft::WRL::ComPtr<IDCompositionSurface> composition_surface_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> surface_bitmap_;
};

}  // namespace musicboard
