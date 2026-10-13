#include "core/localization.h"
#include "app/application.h"

#include "app/app_output.h"
#include "app/update_client.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>   // GET_X_LPARAM

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "audio/waveform.h"
#include "audio/preload_cache.h"
#include "core/path_utf8.h"
#include "core/single_instance.h"
#include "library/import_transaction.h"
#include "library/library_ops.h"
#include "library/library_scanner.h"
#include "library/staging.h"
#include "platform/dpi.h"
#include "platform/window_layers.h"
#include "ui/palette.h"

// shell32 与 ole32 由 CMake 链接（见 CMakeLists 的 musicboard 目标）

namespace musicboard::app {
namespace {

constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT kFrameIntervalMs = 16;
// 通知区域图标的回调消息
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;

[[nodiscard]] bool choose_library_folder(std::filesystem::path& path, Failure& failure) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(dialog.GetAddressOf()));
    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(hr)) hr = dialog->GetOptions(&options);
    if (SUCCEEDED(hr)) hr = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (SUCCEEDED(hr)) hr = dialog->SetTitle(tr(L"选择音乐板曲库（包含 BGM / SFX 的文件夹）", L"Select a MusicBoard library (folder containing BGM / SFX)"));
    if (SUCCEEDED(hr)) hr = dialog->Show(nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        failure.set("app.cancelled", tr("已取消选择曲库", "Library selection cancelled"));
        return false;
    }
    Microsoft::WRL::ComPtr<IShellItem> selected;
    if (SUCCEEDED(hr)) hr = dialog->GetResult(selected.GetAddressOf());
    PWSTR selected_path = nullptr;
    if (SUCCEEDED(hr)) hr = selected->GetDisplayName(SIGDN_FILESYSPATH, &selected_path);
    if (FAILED(hr) || selected_path == nullptr) {
        ::CoTaskMemFree(selected_path);
        failure.set(fail_code::platform_api_failed,
            tr("选择曲库失败 HRESULT=", "Library selection failed HRESULT=") + std::to_string(static_cast<unsigned long>(hr)));
        return false;
    }
    path = std::filesystem::path(selected_path);
    ::CoTaskMemFree(selected_path);
    return true;
}
// 拖动判定阈值（物理像素）：超过它才算拖动，否则算点击
constexpr int kDragThreshold = 5;
// 音量滑块的逻辑宽度（与布局里的 volume_slider_width 一致）
// 面板显隐动效时长（秒）。与原型一致（--t-panel: 420ms）。
constexpr double kRevealSeconds = 0.42;
// 悬停过渡时长（秒）。原型 --t-micro: 120ms。
constexpr float kHoverSeconds = 0.12f;

constexpr float kVolumeSliderWidth = 104.0f;

[[nodiscard]] std::string format_time(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    const int total = static_cast<int>(seconds + 0.5);
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;
    char buffer[32]{};
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hours, minutes, secs);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, secs);
    }
    return buffer;
}

[[nodiscard]] std::string container_badge(AudioContainer container) {
    switch (container) {
        case AudioContainer::mp3:
            return "MP3";
        case AudioContainer::ogg:
            return "OGG";
        case AudioContainer::wav:
            return "WAV";
        case AudioContainer::flac:
            return "FLAC";
        case AudioContainer::mp4:
            return "M4A";
        case AudioContainer::wma:
            return "WMA";
        case AudioContainer::aiff:
            return "AIFF";
        case AudioContainer::unknown:
            break;
    }
    return "?";
}



// 通知区域图标：程序自己的"球"标记，而不是系统的默认图标。
//
// 生成而不是内置一个 .ico：程序是单 exe 便携的，多一个资源文件就多一处
// 可能与安装包不同步的东西。32bpp 带 alpha 的位图手工画一个圆足够。
[[nodiscard]] HICON make_orb_icon(int size) {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;   // 负数表示自上而下
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    const HDC screen = ::GetDC(nullptr);
    const HBITMAP color = ::CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                                             DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (color == nullptr || bits == nullptr) {
        return nullptr;
    }

    auto* pixels = static_cast<std::uint32_t*>(bits);
    const float radius = static_cast<float>(size) * 0.5f - 0.5f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = static_cast<float>(x) - radius;
            const float dy = static_cast<float>(y) - radius;
            const float distance = std::sqrt(dx * dx + dy * dy);
            // 边缘 1px 抗锯齿：小图标上差这一像素就看得出锯齿
            float alpha = (radius - distance) + 0.5f;
            alpha = std::clamp(alpha, 0.0f, 1.0f);
            // 中心亮、边缘暗，读起来像个球而不是一个扁圆片
            const float t = std::clamp(distance / (radius > 0.0f ? radius : 1.0f), 0.0f, 1.0f);
            const auto shade = [t](float base) {
                return static_cast<std::uint8_t>(std::clamp(base * (1.0f - 0.35f * t), 0.0f, 255.0f));
            };
            const auto red = shade(242.0f);
            const auto green = shade(160.0f);
            const auto blue = shade(61.0f);
            const auto a = static_cast<std::uint32_t>(alpha * 255.0f);
            // 内存顺序是 BGRA
            pixels[static_cast<std::size_t>(y) * size + x] =
                (a << 24) | (static_cast<std::uint32_t>(red) << 16) |
                (static_cast<std::uint32_t>(green) << 8) | static_cast<std::uint32_t>(blue);
        }
    }

    const HBITMAP mask = ::CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    const HICON icon = ::CreateIconIndirect(&info);
    ::DeleteObject(color);
    ::DeleteObject(mask);
    return icon;
}

}  // namespace

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------

bool Application::initialize(const std::filesystem::path& library_override, Failure& failure) {
    started_at_ = std::chrono::steady_clock::now();

    // ---- 设置 ----
    settings_path_ = default_settings_path();
    if (!settings_path_.empty()) {
        ProductSettingsStore store;
        ProductSettingsLoadResult result;
        Failure load_failure;
        if (store.load_or_initialize(settings_path_, result, load_failure)) {
            product_ = result.settings;
            set_language(product_.ui_language);
            for (const std::string& note : result.notes) {
                report_line_fmt(tr("设置提示: %s\n", "Settings note: %s\n"), note.c_str());
            }
            if (result.state == ProductSettingsLoadState::corrupt_isolated) {
                // 损坏的文件已经被挪到一边，现场留着 —— 明确说出来，
                // 不能让用户以为"设置莫名其妙清了"。
                report_line_fmt(tr("设置文件损坏，已隔离到 %s（原因: %s）\n", "Invalid settings moved to %s (reason: %s)\n"),
                            path_to_utf8(result.isolated_path).c_str(),
                            result.isolation_reason.c_str());
                set_status(tr("设置文件损坏，已隔离并从默认值开始", "Invalid settings isolated; defaults restored"));
            }
        } else {
            failure = load_failure;
            return false;
        }
    }

    // ---- 曲库 ----
    if (!library_override.empty()) {
        library_root_ = library_override;
    } else if (!product_.library_folder.empty()) {
        // 设置文件里存的是 UTF-8，必须经 path_from_utf8 转 —— 不能直接
        // std::filesystem::path(窄字符串)，那走活动代码页（AGENTS.md 第 2 条）。
        Failure convert_failure;
        if (path_from_utf8(product_.library_folder, library_root_, convert_failure)) {
            // 成功
        } else {
            failure = convert_failure;
            return false;
        }
    }
    if (library_root_.empty()) {
        if (!choose_library_folder(library_root_, failure)) return false;
    }

    // ---- 清掉上次运行留下的旧暂存目录 ----
    //
    // 导入失败会**故意**留着暂存目录当证据，但证据不该永久留着：跑几十次导入
    // 之后暂存区会堆满残片。留七天足够排查，再久就是垃圾。
    // （这一步以前没接 —— prune_staging 写好了却没人调。）
    if (!default_staging_root().empty()) {
        Failure prune_failure;
        std::size_t removed = 0;
        if (prune_staging(default_staging_root(), std::chrono::hours(24 * 7), removed,
                          prune_failure)) {
            if (removed > 0) {
                report_line_fmt(tr("清理了 %zu 个旧的导入暂存目录\n", "Removed %zu old import staging folders\n"), removed);
            }
        } else {
            // 清不掉不算致命，但要说出来：不然用户看到的只是"暂存区越来越大"
            report_line_fmt(tr("清理旧暂存目录时有问题: %s\n", "Staging cleanup issue: %s\n"), prune_failure.to_string().c_str());
        }
    }

    // ---- 音频 ----
    Failure engine_failure;
    if (engine_.initialize(engine_failure)) {
        audio_ready_ = true;
        // 把设置里的音量落到引擎上
        for (std::size_t i = 0; i < sfx_slot_count; ++i) {
            Failure ignored;
            static_cast<void>(engine_.set_slot_volume(
                AudioBus::sfx, i,
                static_cast<float>(product_.sfx_slots[i].volume_percent) / 100.0f, ignored));
        }
        Failure ignored;
        static_cast<void>(engine_.set_bus_volume(
            AudioBus::bgm, static_cast<float>(product_.bgm_volume_percent) / 100.0f, ignored));
        static_cast<void>(engine_.set_bus_volume(
            AudioBus::sfx, static_cast<float>(product_.sfx_volume_percent) / 100.0f, ignored));
    } else {
        // 没有音频设备不该让程序起不来 —— 但必须说清楚，
        // 否则用户会以为"点了没声音"是界面坏了。
        report_line_fmt(tr("音频引擎不可用（界面仍可使用）: %s\n", "Audio engine unavailable (UI remains available): %s\n"), engine_failure.to_string().c_str());
        set_status(tr("音频设备不可用: ", "Audio device unavailable: ") + engine_failure.context);
    }

    rebuild_visible(bgm_, false);
    rebuild_visible(sfx_, true);
    loading_ = true;
    metrics_.dj_mode = product_.sfx_dj_mode;
    metrics_.sfx_collapsed = product_.sfx_collapsed;
    if (metrics_.sfx_collapsed) metrics_.width = 746.0f;
    if (!loader_.start(library_root_, failure, product_data_root() / L"preload-v1")) return false;
    // Loading continues on a worker while the real progress panel is responsive.
    return true;
}

// ---------------------------------------------------------------------------
// 窗口
// ---------------------------------------------------------------------------

// 面板窗口：把消息转给 Application。
// 不用回调而用子类，是因为"程序要处理消息"本来就该是窗口的派生行为，
// 而回调会让消息路径变成一条隐式的间接调用。
class PanelWindow final : public CompositionWindow {
public:
    explicit PanelWindow(Application& app) noexcept : app_(app) {}

protected:
    [[nodiscard]] bool on_message(UINT message, WPARAM wparam, LPARAM lparam,
                                 LRESULT& result) override {
        if (message == WM_DPICHANGED) {
            app_.cancel_pointer_gesture();
            app_.dirty_ = true;
        }
        return app_.handle_message(message, wparam, lparam, result);
    }

    void on_size_changed(UINT, UINT) override {
        POINT cursor{};
        if (::GetCursorPos(&cursor) && ::ScreenToClient(handle(), &cursor)) {
            app_.cursor_client_ = cursor;
            app_.hit_.set_cursor(app_.to_ui_point(cursor));
        }
        app_.dirty_ = true;
    }

    // 标题栏上的按钮要拿到点击，其余标题栏区域继续走 HTCAPTION 拖动。
    // 基类只认"标题栏高度"这一条，会把按钮一起吃掉 —— 结果是按钮画得出来、
    // 点上去却变成拖窗口（实测扫过一遍，那片区域从来收不到鼠标消息）。
    [[nodiscard]] LRESULT hit_test_client(POINT client_point) const override {
        if (app_.is_titlebar_button_point(client_point)) {
            return HTCLIENT;
        }
        return CompositionWindow::hit_test_client(client_point);
    }

private:
    Application& app_;
};

namespace {
// 面板窗口的实例要活到消息循环结束；Application 持有它的唯一指针。
std::unique_ptr<PanelWindow> g_panel_window;
// 悬浮球窗口的句柄。它由 Application 的成员持有，这里只留一份给自检用 ——
// 自检要往它发鼠标消息，而不是通过 Application 的私有方法（那样测的就不是
// 真正的窗口过程了）。
void* g_orb_window_handle = nullptr;
}  // namespace

// 给自检用的几个小查询：不把窗口指针暴露出去，只回答"有没有 / 可见不可见"。
bool panel_window_has_handle() { return g_panel_window != nullptr; }
HWND panel_window_handle() { return g_panel_window ? g_panel_window->handle() : nullptr; }
bool orb_window_handle_exists() { return g_orb_window_handle != nullptr; }
void* orb_window_handle() { return g_orb_window_handle; }
bool panel_window_visible() {
    return g_panel_window != nullptr && ::IsWindowVisible(g_panel_window->handle()) != FALSE;
}

ui::Point Application::to_ui_point(POINT client) const noexcept {
    // Inverse of the render transform. Preserve fractions at control edges.
    const float scale = static_cast<float>(g_panel_window->dpi()) / 96.0f;
    return {static_cast<float>(client.x) / scale, static_cast<float>(client.y) / scale};
}

Application::~Application() {
    loader_.shutdown();
    // Window callbacks must end while every model/render/audio member is alive.
    // The previous global window was destroyed after Application had gone away.
    if (g_panel_window && ::IsWindow(g_panel_window->handle())) {
        ::KillTimer(g_panel_window->handle(), kFrameTimer);
        cancel_pointer_gesture();
        ::DestroyWindow(g_panel_window->handle());
    }
    destroy_tray_icon();
    hotkeys_.unregister_all();
    orb_.set_activate_callback({});
    orb_.set_playback_callback({}); orb_.set_hover_callback({});
    orb_.set_context_menu_callback({});
    orb_.set_moved_callback({});
    if (::IsWindow(orb_.handle())) ::DestroyWindow(orb_.handle());
    g_orb_window_handle = nullptr;
    g_panel_window.reset();
}

bool Application::create_windows(Failure& failure) {
    layout_ = ui::compute_panel_layout(metrics_);
    // 布局到手了，立刻把滚动几何算出来 —— 否则启动时滚动范围是 0，
    // 用户必须先点一次分类（触发 rebuild_visible）才滚得动。
    refresh_scroll_geometry();

    // ---- 悬浮球 ----
    {
        OrbWindow::Options orb_options;
        orb_options.window.title = tr(L"音乐板 · 悬浮球", L"MusicBoard · Orb");
        if (product_.orb.has_position()) {
            orb_options.initial_x = product_.orb.x;
            orb_options.initial_y = product_.orb.y;
        } else {
            // 第一次运行：默认放右下角。
            // 不给默认位置的话系统会给左上角 (0,0) —— 而面板也在 (0,0)，
            // 面板开着时球整个被压在下面，收起面板之后它才从左上角冒出来，
            // 看起来像"球不知道从哪蹦出来的"。
            RECT work{};
            if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
                constexpr int kOrbSize = kOrbWindowLogicalSize;
                orb_options.initial_x = work.right - kOrbSize - 48;
                orb_options.initial_y = work.bottom - kOrbSize - 96;
            }
        }
        // 球上只画一次：它不转（球在转是阶段 6 的可选项），
        // 但窗口需要那一帧的绘制回调。
        const bool orb_created = orb_.create(
            orb_options,
            [this](ID2D1RenderTarget& target, const DrawContext& context) {
                // 这里是**唯一**画球的地方。原来留空（注释还写着"由 OrbWindow
                // 自己画"—— 而 OrbWindow 只处理消息、不画东西），于是球是个
                // 完全透明的窗口：点得到、看不见。
                if (!orb_view_ready_) {
                    Microsoft::WRL::ComPtr<ID2D1DeviceContext> device_context;
                    Failure orb_view_failure;
                    if (target.QueryInterface(IID_PPV_ARGS(device_context.GetAddressOf())) ==
                            S_OK &&
                        orb_view_.initialize(target, device_context.Get(), orb_view_failure)) {
                        orb_view_ready_ = true;
                    } else {
                        report_line_fmt(tr("悬浮球绘制初始化失败: %s\n", "Orb rendering initialization failed: %s\n"),
                                        orb_view_failure.to_string().c_str());
                    }
                }
                if (orb_view_ready_) {
                    orb_view_.draw(target, context.logical_width, context.logical_height,
                                   orb_state_);
                }
            },
            failure);
        if (!orb_created) {
            report_line_fmt(tr("悬浮球创建失败（继续跑主面板）: %s\n", "Orb creation failed (panel still available): %s\n"), failure.to_string().c_str());
        } else {
            g_orb_window_handle = orb_.handle();
            // Upper 70% toggles the panel; lower 30% pauses/resumes BGM.
            orb_.set_activate_callback([this] { set_panel_visible(!panel_visible_, true); });
            orb_.set_playback_callback([this] { if (!loading_) toggle_bgm_pause(); });
            orb_.set_hover_callback([this](int zone) { orb_hover_zone_ = zone; dirty_ = true; });
            orb_.set_context_menu_callback([this](POINT point) { show_tray_menu(point); });
            orb_.set_moved_callback([this](POINT top_left) {
                product_.orb.x = top_left.x;
                product_.orb.y = top_left.y;
                save_settings();
            });
        }
    }

    // ---- 主面板 ----
    g_panel_window = std::make_unique<PanelWindow>(*this);
    CompositionWindow::Options panel_options;
    panel_options.window.title = tr(L"音乐板", L"MusicBoard");
    panel_options.window.logical_width = static_cast<int>(metrics_.width);
    panel_options.window.logical_height = static_cast<int>(metrics_.height);
    panel_options.titlebar_logical_height = static_cast<int>(metrics_.titlebar_height);
    // **默认置顶。** 跑团时它要浮在浏览器与 PDF 之上，不置顶等于每次切回来都被挡在后面。
    // 设置里可以关，托盘菜单也能切换。
    panel_options.window.topmost = product_.panel.topmost;
    panel_options.window.initially_visible = false;
    panel_options.window.tool_window = true;
    if (product_.panel.has_position()) {
        panel_options.window.initial_x = product_.panel.x;
        panel_options.window.initial_y = product_.panel.y;
    }

    const bool panel_created = g_panel_window->create(
        panel_options,
        [this](ID2D1RenderTarget& target, const DrawContext&) {
            if (!view_ready_) {
                Microsoft::WRL::ComPtr<ID2D1DeviceContext> device_context;
                if (SUCCEEDED(target.QueryInterface(
                        IID_PPV_ARGS(device_context.GetAddressOf())))) {
                    Failure view_failure;
                    if (view_.initialize(target, device_context.Get(), view_failure)) {
                        // 芯片宽度按**每一列自己的**分类列表量：两列的分类不同，
                        // 共用一份宽度会让右列的芯片排布错位。
                        view_.measure_chips(state_.bgm_library);
                        view_.measure_chips(state_.sfx_library);
                        view_ready_ = true;
                    } else {
                        report_line_fmt(tr("面板绘制初始化失败: %s\n", "Panel rendering initialization failed: %s\n"),
                                    view_failure.to_string().c_str());
                    }
                }
            }
            if (view_ready_) {
                view_.draw(target, layout_, state_);
            }
        },
        failure);
    if (!panel_created) {
        return false;
    }

    // 拖动整个窗口走系统的 HTCAPTION（阶段 2 已验证），
    // 这里只需要接收文件投放。
    ::DragAcceptFiles(g_panel_window->handle(), TRUE);

    // ---- 全局快捷键 ----
    // 注册与冲突上报都在 re_register_hotkeys 里 —— 启动、改键、开关共用同一条路，
    // 免得"启动时报冲突、改键时忘了报"这种不一致。
    re_register_hotkeys();

    // ---- 通知区域 ----
    Failure tray_failure;
    if (!create_tray_icon(tray_failure)) {
        // 托盘建不起来不该让程序起不来，但要让用户知道"关闭会直接退出"
        report_line_fmt(tr("通知区域图标创建失败: %s\n", "Tray icon creation failed: %s\n"), tray_failure.to_string().c_str());
    }

    ::SetTimer(g_panel_window->handle(), kFrameTimer, kFrameIntervalMs, nullptr);
    panel_visible_ = false;
    if (loading_) set_panel_visible(true);
    return true;
}

int Application::run() {
    if (g_panel_window == nullptr) {
        return 1;
    }
    return g_panel_window->run_message_loop();
}

bool Application::toggle_panel_topmost(Failure& failure) {
    if (!g_panel_window || !::IsWindow(g_panel_window->handle())) {
        failure.set("ui.topmost", "Cannot toggle topmost: main panel window is unavailable");
        return false;
    }
    const HWND window = g_panel_window->handle();
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR style = ::GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (style == 0 && ::GetLastError() != ERROR_SUCCESS) {
        failure.set("ui.topmost", "GetWindowLongPtrW(main panel) Win32=" + std::to_string(::GetLastError()));
        return false;
    }
    const bool pinned = (style & WS_EX_TOPMOST) == 0;
    if (!::SetWindowPos(window, pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
        failure.set("ui.topmost", "SetWindowPos(main panel) Win32=" + std::to_string(::GetLastError()));
        return false;
    }
    product_.panel.topmost = pinned;
    state_.always_on_top = pinned;
    dirty_ = true;
    if (orb_.handle() && !::SetWindowPos(orb_.handle(), HWND_TOPMOST, 0, 0, 0, 0,
                                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
        failure.set("ui.topmost", "SetWindowPos(orb after main panel toggle) Win32=" + std::to_string(::GetLastError()));
        return false;
    }
    if (!settings_path_.empty()) {
        ProductSettingsStore store;
        if (!store.save_atomic(settings_path_, product_, failure)) return false;
    }
    return true;
}

void Application::set_panel_visible(bool visible, bool immediate) {
    if (!visible) cancel_pointer_gesture();
    if (panel_visible_ == visible && !panel_hide_pending_) {
        return;
    }
    panel_visible_ = visible;
    if (visible) {
        // 展开：窗口先出来，再从 0 走到 1（缩放 + 上移 + 淡入）
        if (g_panel_window != nullptr) {
            ::ShowWindow(g_panel_window->handle(), SW_SHOW);
            ::SetForegroundWindow(g_panel_window->handle());
            ::SetWindowPos(orb_.handle(), HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        begin_reveal(true);
    } else if (immediate) {
        reveal_showing_ = false;
        reveal_stamp_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        panel_hide_pending_ = false;
        state_.reveal = 0.0f;
        if (g_panel_window) ::ShowWindow(g_panel_window->handle(), SW_HIDE);
    } else {
        // 收起：**先不藏窗口**，让 420ms 的动效走完（update_reveal 里再藏）。
        // 直接 SW_HIDE 会变成瞬间消失，看不出收起这个动作。
        begin_reveal(false);
    }
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// 消息
// ---------------------------------------------------------------------------

bool Application::handle_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) {
    POINT cursor{};

    // 又双击了一次图标：把面板显出来并拿到前台，而不是再起一套音频与覆盖层。
    // 收进通知区域之后这个动作尤其需要 —— 否则用户要先去托盘里找。
    if (message != 0 && message == show_panel_message()) {
        set_panel_visible(true);
        set_status(tr("面板已呼出", "Panel opened"));
        if (g_panel_window != nullptr) {
            ::SetForegroundWindow(g_panel_window->handle());
        }
        result = 0;
        return true;
    }

    switch (message) {
        case WM_ACTIVATE:
            if (LOWORD(wparam) != WA_INACTIVE && orb_.handle())
                ::SetWindowPos(orb_.handle(), HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return false;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lparam) != g_panel_window->handle() &&
                (dragging_ || seeking_ || slider_drag_target_ >= 0)) cancel_pointer_gesture();
            result = 0;
            return true;
        case WM_CANCELMODE:
        case WM_KILLFOCUS:
            cancel_pointer_gesture();
            result = 0;
            return true;
        case WM_TIMER:
            if (wparam == kFrameTimer) {
                on_frame();
                result = 0;
                return true;
            }
            break;

        case WM_MOUSELEAVE:
            hit_.set_cursor({-100, -100}); dirty_ = true; result = 0; return true;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, g_panel_window->handle(), 0};
            ::TrackMouseEvent(&tracking);
            cursor.x = GET_X_LPARAM(lparam);
            cursor.y = GET_Y_LPARAM(lparam);
            on_mouse_move(cursor);
            trace_input("move", cursor);
            result = 0;
            return true;
        }

        case WM_LBUTTONDOWN: {
            cursor.x = GET_X_LPARAM(lparam);
            cursor.y = GET_Y_LPARAM(lparam);
            on_left_down(cursor);
            trace_input("left_down", cursor);
            result = 0;
            return true;
        }

        case WM_LBUTTONUP: {
            cursor.x = GET_X_LPARAM(lparam);
            cursor.y = GET_Y_LPARAM(lparam);
            on_left_up(cursor);
            trace_input("left_up", cursor);
            result = 0;
            return true;
        }

        case WM_RBUTTONUP: {
            cursor.x = GET_X_LPARAM(lparam);
            cursor.y = GET_Y_LPARAM(lparam);
            on_right_up(cursor);
            result = 0;
            return true;
        }

        case WM_MOUSEWHEEL: {
            // 滚轮的坐标是屏幕坐标，要转成客户区
            POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ::ScreenToClient(g_panel_window->handle(), &screen);
            on_wheel(screen, GET_WHEEL_DELTA_WPARAM(wparam));
            result = 0;
            return true;
        }

        case WM_SETCURSOR: {
            // 指针形状是"这块东西能不能点 / 能不能拖"的第一提示。
            // 原型里靠 CSS 的 cursor: grab / pointer，原生得自己按位置设。
            if (LOWORD(lparam) != HTCLIENT) {
                break;   // 标题栏与边框交给系统
            }
            POINT cursor_client{};
            ::GetCursorPos(&cursor_client);
            ::ScreenToClient(g_panel_window->handle(), &cursor_client);
            ::SetCursor(cursor_for_client_point(cursor_client));
            result = TRUE;
            return true;
        }

        case WM_CHAR:
            on_char(static_cast<wchar_t>(wparam));
            result = 0;
            return true;

        case WM_KEYDOWN:
            on_key_down(static_cast<UINT>(wparam));
            result = 0;
            return true;

        case WM_HOTKEY: {
            if (loading_) { result = 0; return true; }
            if (product_.sfx_dj_mode) {
                const auto pad = hotkeys_.id_from_message(wparam);
                if (pad >= 0 && static_cast<std::size_t>(pad) < sfx_.visible.size())
                    static_cast<void>(trigger_pad(sfx_.visible[static_cast<std::size_t>(pad)]));
                result = 0; return true;
            }
            const int id = hotkeys_.id_from_message(wparam);
            if (id >= 0 && static_cast<std::size_t>(id) < sfx_slot_count) {
                const std::size_t deck = static_cast<std::size_t>(id);
                std::size_t index = sfx_has_track_[deck] ? sfx_index_[deck] : selected_sfx_index_;
                const auto pending = std::find_if(pending_plays_.begin(), pending_plays_.end(),
                    [id](const PendingPlay& item) { return item.sfx_deck == id; });
                if (pending != pending_plays_.end()) index = pending->index;
                if (index >= entries_.size() && !sfx_.visible.empty()) index = sfx_.visible.front();
                if (index < entries_.size()) {
                    static_cast<void>(trigger_sfx(static_cast<std::size_t>(id), index));
                } else {
                    set_status(tr("快捷键 ", "Shortcut ") + hotkey_labels_[static_cast<std::size_t>(id)] +
                               tr(" 没有可播放的曲目", " has no track to play"));
                }
            }
            result = 0;
            return true;
        }

        case kTrayMessage: {
            // lparam 是通知区域的事件类型
            if (lparam == WM_LBUTTONUP) {
                set_panel_visible(!panel_visible_);
            } else if (lparam == WM_RBUTTONUP) {
                ::GetCursorPos(&cursor);
                show_tray_menu(cursor);
            }
            result = 0;
            return true;
        }

        case WM_DROPFILES: {
            on_drop_files(reinterpret_cast<HDROP>(wparam));
            result = 0;
            return true;
        }

        case WM_COMMAND: {
            // 托盘菜单
            switch (LOWORD(wparam)) {
                case 1:
                    set_panel_visible(!panel_visible_);
                    break;
                case 2: {
                    Failure failure;
                    if (!toggle_panel_topmost(failure)) set_status(tr("切换置顶失败: ", "Could not change pin state: ") + failure.to_string());
                    else set_status(product_.panel.topmost ? tr("已置顶", "Always on top") : tr("取消置顶", "Always on top off"));
                    break;
                }
                case 3:
                    // 真的退出：先立旗标，再走 WM_CLOSE 那条路。
                    // 只发 WM_CLOSE 的话会被下面那个"关闭 = 收进通知区域"接手，
                    // 于是菜单里的"退出"点了没反应。
                    quitting_ = true;
                    ::PostMessageW(g_panel_window->handle(), WM_CLOSE, 0, 0);
                    break;
                case 10:
                    if (!loading_) toggle_bgm_pause();
                    break;
                case 11:
                    clear_playback(false, true);
                    break;
                case 20:
                    show_link_import();
                    break;
                case 21: {
                    std::string error;
                    if (!fulltools_update_client::launch(L"MusicBoard", MUSICBOARD_PRODUCT_VERSION, false, error))
                        set_status(tr("无法检查更新：", "Update check unavailable: ") + error);
                    break;
                }
                case 1002:
                    product_.hotkeys.enabled = !product_.hotkeys.enabled;
                    save_settings();
                    if (product_.hotkeys.enabled) {
                        re_register_hotkeys();
                        set_status(tr("全局快捷键已启用", "Global shortcuts enabled"));
                    } else {
                        hotkeys_.unregister_all();
                        set_status(tr("全局快捷键已关闭（不再抢别的程序的按键）", "Global shortcuts disabled"));
                    }
                    break;
                case 1007:
                    product_.hotkeys = HotkeySettings{};
                    save_settings();
                    re_register_hotkeys();
                    set_status(tr("快捷键已恢复默认（Ctrl+Alt+1..4）", "Default shortcuts restored (Ctrl+Alt+1..4)"));
                    break;
                default:
                    if (LOWORD(wparam) >= 1100 && LOWORD(wparam) < 1105) {
                        const auto i=static_cast<std::size_t>(LOWORD(wparam)-1100);
                        if (!loading_ && i<recent_menu_indices_.size()) {
                            const auto index=recent_menu_indices_[i];
                            if (index<entries_.size()) queue_playback(index,-1);
                        }
                    }
                    if (LOWORD(wparam) >= 1003 &&
                        LOWORD(wparam) < 1003 + static_cast<int>(sfx_slot_count)) {
                        begin_hotkey_capture(LOWORD(wparam) - 1003);
                    }
                    break;
            }
            result = 0;
            return true;
        }

        case WM_CLOSE:
            // 关闭按钮 / 标题栏的 × → 收进通知区域（面板隐藏，悬浮球还在），
            // 不是退出。用户明确要求"关闭也要能看见悬浮球"。
            // 真退出只有一条路：托盘（或球的右键）菜单里的"退出"，
            // 它会把 quitting_ 立起来再走这里。
            if (quitting_) {
                result = 0;
                return false;   // 交给基类去销毁窗口并退出消息循环
            }
            if (orb_.handle() || tray_ready_) {
                set_panel_visible(false);
                set_status(tr("已收起，点击悬浮球展开", "Panel hidden; click the orb to open"));
            } else {
                ::PostMessageW(g_panel_window->handle(), WM_DESTROY, 0, 0);
            }
            result = 0;
            return true;

        case WM_DESTROY: {
            // 保存现场：窗口位置、球位置、音量、快捷键
            if (g_panel_window != nullptr) {
                const RECT rect = g_panel_window->window_rect();
                product_.panel.x = rect.left;
                product_.panel.y = rect.top;
                product_.panel.width = rect.right - rect.left;
                product_.panel.height = rect.bottom - rect.top;
                product_.panel.visible = panel_visible_;
            }
            save_settings();
            destroy_tray_icon();
            hotkeys_.unregister_all();
            static_cast<void>(::KillTimer(g_panel_window->handle(), kFrameTimer));
            result = 0;
            return false;   // 交给基类去 PostQuitMessage
        }

        default:
            break;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 帧
// ---------------------------------------------------------------------------

void Application::on_frame() {
    pump_link_import();
    const auto layer_now = std::chrono::steady_clock::now();
    if (!update_check_started_ && layer_now - started_at_ >= std::chrono::seconds(10)) {
        update_check_started_ = true;
        std::string error;
        if (!fulltools_update_client::launch(L"MusicBoard", MUSICBOARD_PRODUCT_VERSION, true, error))
            report(LogLevel::warning, "Update check", error);
    }
    if (layer_now - last_layer_repair_ >= std::chrono::milliseconds(200)) {
        last_layer_repair_ = layer_now;
        Failure layer_failure;
        if (!restore_pinned_window_layers(orb_.handle(), panel_window_handle(), product_.panel.topmost, layer_failure))
            report(LogLevel::error, "Window layers", layer_failure.to_string());
    }
    collect_library_load();
    // 动效推进必须走**真实流逝时间**：dt 由两个绝对时刻相减得到，
    // 所以掉帧只是跳一小步，不会让悬停过渡越来越慢（AGENTS.md 第 3 条）。
    {
        const auto now = std::chrono::steady_clock::now();
        const float dt = have_last_frame_stamp_
                             ? static_cast<float>(
                                   std::chrono::duration<double>(now - last_frame_stamp_).count())
                             : 0.0f;
        last_frame_stamp_ = now;
        have_last_frame_stamp_ = true;
        update_hover_animations(std::clamp(dt, 0.0f, 0.1f));
        update_reveal();
    }

    if (audio_ready_) {
        engine_.update(
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at_).count());
        if (!engine_.slot_state(AudioBus::bgm, bgm_slot_).in_use && engine_.slot_state(AudioBus::bgm, 1u-bgm_slot_).in_use) {
            Failure failure;
            if (!engine_.stop(AudioBus::bgm, 1u-bgm_slot_, .1, failure))
                report(LogLevel::error, "BGM tail cleanup", failure.to_string());
        }
    }

    Failure stream_failure;
    while (engine_.take_failure(stream_failure)) {
        set_status(tr("连续播放失败: ", "Streaming playback failed: ") + stream_failure.to_string());
        stream_failure = {};
    }
    collect_envelopes();
    collect_pending_playback();
    sync_state();
    // Input must describe the state being drawn this frame. Immediate prepared
    // playback otherwise leaves clear/seek disabled for one extra frame.
    build_regions();

    // 只在"画面真的变了"时重画。
    // 播放中每帧都要重画（进度在动），但**拖音波图时不能按旧位置重画** ——
    // 那样会和拖拽预览互相抢，看起来就是"松手时闪两下才到位"。
    bool needs_redraw = dirty_;

    // 任何一路还在出声就每帧重画。
    //
    // 原来只盯 BGM 的进度，于是**音效台的波形图与唱片只在鼠标动的时候才转** ——
    // 因为鼠标移动是唯一还会把 dirty_ 立起来的东西。看上去就是"不动鼠标就不转"。
    // 唱片的角度是由播放位置算的（不是每帧累加），所以每帧重画不会让角度漂。
    if (audio_ready_ && !seeking_) {
        if (engine_.active_count(AudioBus::bgm) > 0 || engine_.active_count(AudioBus::sfx) > 0) {
            needs_redraw = true;
        }
        const VoiceSlotState bgm_now = engine_.slot_state(AudioBus::bgm, bgm_slot_);
        if (bgm_now.in_use) {
            last_drawn_progress_ = bgm_now.position_seconds;
        }
    }

    // 动效没走完也要继续重画（悬停过渡、显隐、唱针），否则它们会停在半路
    if (animations_active()) {
        needs_redraw = true;
    }

    if (input_trace_enabled()) {
        ++frame_counter_;
        if (frame_counter_ % 60 == 0) {
            report(LogLevel::info, "frame",
                   tr("第 ", "Frame ") + std::to_string(frame_counter_) + tr(" 帧 needs_redraw=", " needs_redraw=") +
                       (needs_redraw ? "1" : "0"));
        }
    }

    // ---- 悬浮球：只在它的画面真的变了时才重画 ----
    // 球是常驻窗口，每秒 60 帧无脑重画纯属浪费（它多半时候是静止的）。
    if (orb_view_ready_ && orb_.handle() != nullptr) {
        const float kProgressEpsilon = 0.0005f;
        const float kAmplitudeEpsilon = 0.02f;
        const bool changed = animations_active() || orb_state_.playing || dirty_ || orb_warmup_frames_ > 0 ||
                             std::fabs(orb_state_.progress - orb_last_progress_) >
                                 kProgressEpsilon ||
                             std::fabs(orb_state_.amplitude - orb_last_amplitude_) >
                                 kAmplitudeEpsilon ||
                             orb_state_.playing != orb_last_playing_;
        if (changed) {
            if (orb_warmup_frames_ > 0) {
                --orb_warmup_frames_;
            }
            orb_last_progress_ = orb_state_.progress;
            orb_last_amplitude_ = orb_state_.amplitude;
            orb_last_playing_ = orb_state_.playing;
            Failure orb_failure;
            if (!orb_.redraw_frame(orb_failure)) {
                report(LogLevel::error, tr("悬浮球重画失败", "Orb redraw failed"), orb_failure.to_string());
            }
        }
    }

    if (needs_redraw && g_panel_window != nullptr && ::IsWindowVisible(g_panel_window->handle())) {
        dirty_ = false;
        Failure redraw_failure;
        if (!g_panel_window->redraw_frame(redraw_failure)) {
            // **必须进日志。** 原来只写 OutputDebugString —— 那是调试器才看得到的地方，
            // 于是"面板不刷新"在现场什么都查不到（连不刷新这件事本身都看不到）。
            // 连续失败时每 60 次记一条，免得刷爆日志。
            ++redraw_failure_count_;
            if (redraw_failure_count_ == 1 || redraw_failure_count_ % 60 == 0) {
                report(LogLevel::error, tr("面板重画失败", "Panel redraw failed"),
                       tr("第 ", "Frame ") + std::to_string(redraw_failure_count_) + tr(" 次: ", " times: ") +
                           redraw_failure.to_string());
            }
        } else if (redraw_failure_count_ > 0) {
            report(LogLevel::info, tr("面板重画恢复", "Panel rendering recovered"),
                   tr("之前连续失败 ", "Previous consecutive failures: ") + std::to_string(redraw_failure_count_) + tr(" 次", " times"));
            redraw_failure_count_ = 0;
        }
    }
}

bool Application::section_of_id(int id) noexcept {
    // 编号段决定它属于哪一列。两列的段错开，段本身不重叠，
    // 所以"是不是音效列"这一条判断就够，不需要再查可见列表。
    if (id >= kSfxCardBase) {
        return true;                                    // 音效卡 2000+
    }
    if (id >= kCardBase && id < kSfxCardBase) {
        return false;                                   // 音乐卡 1000..1999
    }
    if (id >= kSfxChipBase && id < kSfxChipBase + 100) {
        return true;                                    // 音效芯片 500..599
    }
    if (id >= kChipBase && id < kChipBase + 100) {
        return false;                                   // 音乐芯片 400..499
    }
    if (id == kSearchIdSfx) {
        return true;
    }
    return false;
}

bool Application::chip_of_id(int id, bool& is_sfx, int& index) noexcept {
    if (id >= kSfxChipBase && id < kSfxChipBase + 100) {
        is_sfx = true;
        index = id - kSfxChipBase;
        return true;
    }
    if (id >= kChipBase && id < kChipBase + 100) {
        is_sfx = false;
        index = id - kChipBase;
        return true;
    }
    return false;
}

bool Application::card_of_id(int id, bool& is_sfx, int& index) noexcept {
    if (id >= kSfxCardBase) {
        is_sfx = true;
        index = id - kSfxCardBase;
        return true;
    }
    // 只认 1000..1999：音效台的面板控件在 200..299，不能落进来
    if (id >= kCardBase && id < kCardBase + 1000) {
        is_sfx = false;
        index = id - kCardBase;
        return true;
    }
    return false;
}

Application::Column& Application::column_of_id(int id) noexcept {
    return section_of_id(id) ? sfx_ : bgm_;
}

Application::Column& Application::active_search_column() noexcept {
    return sfx_.search_focused ? sfx_ : bgm_;
}

void Application::rebuild_visible(Column& column, bool section_is_sfx) {
    if (!product_.show_all_categories && column.active_chip < 0 && !column.categories.empty())
        column.active_chip = 0;
    column.visible.clear();
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].is_sfx != section_is_sfx) {
            continue;   // 另一栏的条目
        }
        if (column.active_chip >= 0 &&
            entries_[i].category != column.categories[static_cast<std::size_t>(column.active_chip)]) {
            continue;
        }
        if (!column.search_text.empty() &&
            entries_[i].track.name.find(column.search_text) == std::string::npos) {
            continue;
        }
        column.visible.push_back(i);
    }
    const ui::Rect& grid = section_is_sfx ? layout_.sfx_grid : layout_.bgm_grid;
    column.scroll.set_view_height(grid.height);
    column.scroll.set_content_height(layout_.card_content_height(grid, column.visible.size()));
    column.scroll.set_offset(column.scroll.offset());
    // 内容长度变了，滚动目标也要夹回合法范围（否则换分类之后会停在空白处）
    float& scroll_target = section_is_sfx ? sfx_scroll_target_ : bgm_scroll_target_;
    scroll_target = std::clamp(scroll_target, 0.0f, column.scroll.maximum_offset());
    dirty_ = true;
}

void Application::set_status(std::string text) {
    if (input_trace_enabled()) {
        report(LogLevel::info, "status", text);
    }
    status_ = std::move(text);
    dirty_ = true;
}

void Application::begin_library_load(const std::filesystem::path& root, bool startup) {
    if (loading_) return;
    cancel_pointer_gesture();
    Failure failure;
    if (!loader_.start(root, failure, product_data_root() / L"preload-v1")) { set_status(tr("加载失败: ", "Loading failed: ") + failure.to_string()); return; }
    startup_loading_ = startup;
    loading_ = true;
    set_panel_visible(true);
    set_status(tr("正在准备曲库", "Preparing library"));
}

void Application::choose_new_library() {
    std::filesystem::path chosen;
    Failure failure;
    if (choose_library_folder(chosen, failure)) begin_library_load(chosen);
    else if (failure.code != "app.cancelled") set_status(tr("选择曲库失败: ", "Library selection failed: ") + failure.to_string());
}

void Application::collect_library_load() {
    if (!loading_) return;
    dirty_ = true;
    LibraryLoadResult result;
    if (!loader_.take(result)) return;
    loading_ = false;
    if (result.cancelled || !result.failure.empty()) {
        set_status(result.cancelled ? tr("加载已取消，原曲库保持可用", "Loading cancelled; previous library is available") : tr("加载失败: ", "Loading failed: ") + result.failure.to_string());
        startup_loading_ = false;
        set_panel_visible(true);
        return;
    }
    Failure failure;
    if (audio_ready_ && !engine_.configure_sfx_slots(sfx_slot_count + result.tracks.size(), failure)) {
        set_status(tr("准备音效按钮失败: ", "Could not prepare effect pads: ") + failure.to_string()); return;
    }
    clear_playback(false);
    pending_plays_.clear();
    for (std::size_t i = 0; i < entries_.size(); ++i) envelopes_.forget(i);
    envelope_watch_.clear();
    entries_.clear(); bgm_ = {}; sfx_ = {};
    bgm_.categories = std::move(result.bgm_categories);
    sfx_.categories = std::move(result.sfx_categories);
    state_.bgm_library = {}; state_.sfx_library = {};
    history_.clear(); history_index_ = -1;
    std::fill(std::begin(sfx_has_track_), std::end(sfx_has_track_), false);
    bgm_scroll_target_ = sfx_scroll_target_ = 0.0f;
    for (auto& item : result.tracks) {
        LibraryEntry entry;
        entry.track = std::move(item.track); entry.label = ui::parse_track_label(entry.track.name); entry.category = std::move(item.category);
        entry.is_sfx = item.is_sfx; entry.prepared = std::move(item.audio);
        entry.preload_failure = std::move(item.failure);
        entry.envelope_attempted = !entry.prepared || !entry.prepared->envelope.empty();
        if (entry.prepared) entry.envelope = entry.prepared->envelope;
        auto& categories = entry.is_sfx ? sfx_.categories : bgm_.categories;
        if (std::find(categories.begin(), categories.end(), entry.category) == categories.end())
            categories.push_back(entry.category);
        entries_.push_back(std::move(entry));
    }
    library_root_ = std::move(result.root);
    product_.library_folder = path_to_utf8(library_root_);
    bgm_index_ = selected_sfx_index_ = entries_.size();
    view_.covers().clear();
    orb_view_.clear_covers();
    rebuild_visible(bgm_, false); rebuild_visible(sfx_, true);
    refresh_scroll_geometry();
    apply_volume_changes();
    save_settings();
    load_details_.clear();
    for (const auto& warning : result.warnings) { report(LogLevel::warning, tr("曲库加载", "Library load"), warning); load_details_ += warning + "\n"; }
    const auto memory = loader_.progress().resident_bytes;
    report_line_fmt("PRELOAD_READY tracks=%zu seconds=%.3f resident_mb=%.2f warnings=%zu cache_hits=%zu\n",
        entries_.size(), result.elapsed_seconds, static_cast<double>(memory) / 1048576.0, result.warnings.size(), result.cache_hits);
    set_status(result.warnings.empty() ? tr("曲库已准备 · ", "Library ready · ") + std::to_string(entries_.size()) + tr(" 首，可直接播放", " tracks ready to play")
        : tr("曲库已加载，", "Library loaded; ") + std::to_string(result.warnings.size()) + tr(" 项问题: ", " issues: ") + result.warnings.front());
    if (startup_loading_) set_panel_visible(true);
    startup_loading_ = false;
}

void Application::set_dj_mode(bool enabled) {
    if (loading_ || product_.sfx_dj_mode == enabled) return;
    cancel_pointer_gesture();
    product_.sfx_dj_mode = enabled; metrics_.dj_mode = enabled;
    layout_ = ui::compute_panel_layout(metrics_);
    sfx_.scroll.set_offset(0.0f); sfx_scroll_target_ = 0.0f;
    refresh_scroll_geometry(); save_settings();
    set_status(enabled ? tr("DJ 按钮 · 点按播放一次，右键停止，可同时触发多条", "DJ Pads · Click for one shot, right-click to stop; multiple pads can play together") : tr("音效唱盘 · 拖入分配，点击唱片暂停/继续", "Effect decks · Drag to assign, click a record to pause/resume"));
}

void Application::set_show_all_categories(bool enabled) {
    if (product_.show_all_categories == enabled || loading_) return;
    cancel_pointer_gesture();
    product_.show_all_categories = enabled;
    bgm_.scroll.set_offset(0); sfx_.scroll.set_offset(0);
    bgm_scroll_target_ = 0; sfx_scroll_target_ = 0;
    bgm_chip_amounts_.clear(); sfx_chip_amounts_.clear();
    rebuild_visible(bgm_, false); rebuild_visible(sfx_, true);
    sync_state(); update_chip_rows(); sync_state(); build_regions();
    save_settings(); dirty_ = true;
}

void Application::set_ui_language(Language selected) {
    if (product_.ui_language == selected) return;
    cancel_pointer_gesture();
    product_.ui_language = selected;
    set_language(selected);
    state_.bgm_library.chip_widths.clear(); state_.sfx_library.chip_widths.clear();
    sync_state(); update_chip_rows(); sync_state(); build_regions();
    if (!::SetWindowTextW(g_panel_window->handle(), tr(L"音乐板", L"MusicBoard")))
        report(LogLevel::error, "Window title update", "Win32=" + std::to_string(::GetLastError()));
    save_settings();
    set_status(tr("界面语言：中文", "Interface language: English"));
}

void Application::set_sfx_collapsed(bool collapsed) {
    if (metrics_.sfx_collapsed == collapsed || loading_) return;
    cancel_pointer_gesture();
    const auto previous_metrics = metrics_;
    metrics_.sfx_collapsed = collapsed; product_.sfx_collapsed = collapsed;
    metrics_.width = collapsed ? 746.0f : 1256.0f;
    if (collapsed) sfx_.search_focused = false;
    layout_ = ui::compute_panel_layout(metrics_);
    refresh_scroll_geometry();
    if (g_panel_window) {
        const auto scale = static_cast<float>(g_panel_window->dpi()) / 96.0f;
        if (!::SetWindowPos(g_panel_window->handle(), nullptr, 0, 0,
            static_cast<int>(metrics_.width * scale), static_cast<int>(metrics_.height * scale),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
            const auto error = ::GetLastError();
            metrics_ = previous_metrics; product_.sfx_collapsed = previous_metrics.sfx_collapsed;
            layout_ = ui::compute_panel_layout(metrics_); refresh_scroll_geometry();
            set_status(tr("调整面板尺寸失败，保留原布局 Win32=", "Resize failed; previous layout kept. Win32=") + std::to_string(error)); return;
        }
    }
    save_settings(); set_status(collapsed ? tr("已收起音效栏", "Effects panel hidden") : tr("已展开音效栏", "Effects panel shown"));
}

std::vector<std::size_t> Application::ranked_recent_bgm() const {
    std::vector<std::pair<std::size_t, const RecentBgm*>> found;
    for (const auto& recent : recent_bgm_) {
        for (std::size_t i=0; i<entries_.size(); ++i) if (entries_[i].track.path == recent.path) {
            found.emplace_back(i, &recent); break;
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.second->count != b.second->count ? a.second->count > b.second->count : a.second->order > b.second->order;
    });
    std::vector<std::size_t> result;
    for (std::size_t i=0; i<std::min<std::size_t>(5,found.size()); ++i) result.push_back(found[i].first);
    return result;
}

bool Application::trigger_pad(std::size_t index) {
    if (loading_ || !audio_ready_ || index >= entries_.size()) return false;
    const auto& entry = entries_[index];
    if (!entry.prepared) { set_status(tr("该音效未就绪: ", "Effect not ready: ") + entry.preload_failure.to_string()); return false; }
    Failure failure;
    const auto start = std::chrono::steady_clock::now();
    if (!engine_.play_prepared(AudioBus::sfx, sfx_slot_count + index, entry.prepared, entry.track.name, 0.0, false, failure)) {
        set_status(tr("音效播放失败: ", "Effect playback failed: ") + failure.to_string()); return false;
    }
    engine_.update(std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at_).count());
    report_line_fmt("PAD_START index=%zu submit_ms=%.3f complete=%d\n", index,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), entry.prepared->complete ? 1 : 0);
    set_status(tr("单次播放  ", "One shot  ") + entry.track.name);
    return true;
}

void Application::update_hover_animations(float dt_seconds) {
    // 先把"每张卡现在的目标"换算成"该往哪走"，再按时间推进一步。
    const auto advance = [dt_seconds](std::vector<float>& amounts,
                                      std::size_t count, int hovered) {
        if (amounts.size() != count) {
            amounts.assign(count, 0.0f);
        }
        const float step = dt_seconds / kHoverSeconds;
        for (std::size_t i = 0; i < amounts.size(); ++i) {
            const float target = (static_cast<int>(i) == hovered) ? 1.0f : 0.0f;
            if (amounts[i] < target) {
                amounts[i] = std::min(target, amounts[i] + step);
            } else if (amounts[i] > target) {
                amounts[i] = std::max(target, amounts[i] - step);
            }
        }
    };

    // 悬停索引从命中 id 反解（两列的编号段不同，不能共用一套换算）
    bool hover_is_sfx = false;
    int hover_index = -1;
    if (!dragging_ && card_of_id(hit_.hovered(), hover_is_sfx, hover_index)) {
        const Column& column = hover_is_sfx ? sfx_ : bgm_;
        if (hover_index < 0 || static_cast<std::size_t>(hover_index) >= column.visible.size()) {
            hover_index = -1;
        }
    } else {
        hover_index = -1;
    }

    advance(bgm_hover_amounts_, bgm_.visible.size(), hover_is_sfx ? -1 : hover_index);
    advance(sfx_hover_amounts_, sfx_.visible.size(), hover_is_sfx ? hover_index : -1);

    // 芯片同理。芯片的"第 0 枚"是"全部"，所以它的下标要 +1。
    bool chip_is_sfx = false;
    int chip_index = -1;
    const bool on_chip = !dragging_ && chip_of_id(hit_.hovered(), chip_is_sfx, chip_index);
    advance(bgm_chip_amounts_, bgm_.categories.size() + (product_.show_all_categories ? 1u : 0u),
            (on_chip && !chip_is_sfx) ? chip_index : -1);
    advance(sfx_chip_amounts_, sfx_.categories.size() + (product_.show_all_categories ? 1u : 0u),
            (on_chip && chip_is_sfx) ? chip_index : -1);

    // 平滑滚动：朝目标追。用"按时间追"而不是每帧固定步长，
    // 这样滚轮连滚多次会像一条连续的滑动，而不是一格一格地跳。
    const auto glide = [dt_seconds](ui::ScrollState& scroll, float& target) {
        const float maximum = scroll.maximum_offset();
        target = std::clamp(target, 0.0f, maximum);
        const float current = scroll.offset();
        const float delta = target - current;
        if (std::fabs(delta) < 1.5f) {
            if (current != target) {
                scroll.set_offset(target);
            }
            return;
        }
        // 指数逼近：每帧吃掉剩余距离的一部分，帧率变化由 dt 归一化
        const float factor = 1.0f - std::exp(-dt_seconds * 18.0f);
        scroll.set_offset(current + delta * factor);
    };
    glide(bgm_.scroll, bgm_scroll_target_);
    glide(sfx_.scroll, sfx_scroll_target_);


}

float Application::reveal_progress() const {
    // 从**绝对时刻**算进度，不做增量累加（AGENTS.md 第 3 条）：
    // 掉帧只会让它跳一小步，不会让整段动效变慢。
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - reveal_stamp_).count();
    const float t = static_cast<float>(elapsed / kRevealSeconds);
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return reveal_showing_ ? clamped : 1.0f - clamped;
}

void Application::begin_reveal(bool showing) {
    reveal_showing_ = showing;
    reveal_stamp_ = std::chrono::steady_clock::now();
    // 收起：窗口先留着，等动效走完再藏（见 update_reveal）
    panel_hide_pending_ = !showing;
    dirty_ = true;
}

void Application::update_reveal() {
    const float progress = reveal_progress();
    state_.reveal = progress;
    // 窗口整体淡入淡出交给合成器（DComp 的 visual 不透明度），
    // 比例留一点底：0 的时候已经藏起来了，没必要真的画到全透明。
    panel_.set_opacity(0.35f + 0.65f * progress);
    if (!reveal_showing_ && progress <= 0.0f && panel_hide_pending_) {
        panel_hide_pending_ = false;
        if (g_panel_window != nullptr) {
            ::ShowWindow(g_panel_window->handle(), SW_HIDE);
        }
    }
}

bool Application::animations_active() const noexcept {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at_).count();
    for (std::size_t i=0; i<disc_motion_.size(); ++i)
        if (disc_motion_[i].moving(now) || disc_hover_[i].moving(now)) return true;
    if (orb_hover_.moving(now)) return true;
    if (state_.bgm_library.search_focused || state_.sfx_library.search_focused) return true;
    if (state_.reveal < 1.0f) {
        return true;
    }
    const auto any_moving = [](const std::vector<float>& amounts) {
        for (const float amount : amounts) {
            if (amount > 0.01f && amount < 0.99f) {
                return true;
            }
        }
        return false;
    };
    if (any_moving(bgm_hover_amounts_) || any_moving(sfx_hover_amounts_) ||
        any_moving(bgm_chip_amounts_) || any_moving(sfx_chip_amounts_)) {
        return true;
    }
    // 平滑滚动还没到目标
    if (std::fabs(bgm_.scroll.offset() - bgm_scroll_target_) > 0.5f ||
        std::fabs(sfx_.scroll.offset() - sfx_scroll_target_) > 0.5f) {
        return true;
    }
    return false;
}

void Application::set_search(std::string text) {
    active_search_column().search_edit_time = std::chrono::duration<double>(std::chrono::steady_clock::now()-started_at_).count();
    // 搜索作用于**当前有焦点的那一列**。两列各自有搜索词，互不影响。
    if (sfx_.search_focused) {
        sfx_.search_text = std::move(text);
        rebuild_visible(sfx_, true);
    } else {
        bgm_.search_text = std::move(text);
        rebuild_visible(bgm_, false);
    }
}

void Application::build_regions() {
    hit_.begin_frame();

    const auto add = [this](const ui::Rect& rect, int id, int group = 0, bool enabled = true) {
        if (rect.empty()) return;
        ui::HitRegion region;
        if (id == 100 || (id >= 200 && id < 240 && (id-200)%10 == 0)) {
            region.circle_center = rect.center(); region.circle_radius = ui::PanelLayout::disc_radius(rect);
            if (!dragging_) enabled = enabled && (id == 100 ? state_.bgm.has_track : state_.sfx[static_cast<std::size_t>((id-200)/10)].has_track);
        }
        region.bounds = rect;
        region.id = id;
        region.group = group;
        region.enabled = enabled;
        hit_.add(region);
    };

    // 标题栏按钮（最上层，先登记的最先被"后加优先"规则压住 —— 它们最后加不了，
    // 所以这里先加，让后加的曲目卡永远在它们上面：曲目卡不会盖到标题栏）。
    add(layout_.busbar_reload, 5); add(layout_.busbar_change, 6); add(layout_.busbar_fold, 9);
    add(layout_.busbar_show_all, 10);
    add(layout_.title_cn, 11); add(layout_.title_en, 12);
    add(layout_.sfx_mode, 7);
    add(layout_.title_close, 2); add(layout_.title_pin, 3); add(layout_.title_minimize, 4);
    if (loading_) {
        add(ui::Rect{layout_.panel.width * 0.5f - 45.0f, layout_.panel.height * 0.5f + 76.0f, 90.0f, 32.0f}, 8);
        return;
    }

    // BGM 台。
    //
    // 唱片**始终**是合法命中区域：它是拖放的落点，而"拖到空盘上开始播"
    // 正是这个程序最主要的交互。一开始按"空盘不该有反应"把它禁用了，
    // 结果空盘谁都放不上去（自检抓到的）。
    // "空盘上点击没有反应"由 toggle_bgm_pause 里的 in_use 判断保证，
    // 不该用禁用命中区域去实现 —— 那会把拖放一起禁掉。
    // 整个 BGM 台都是落点（用户要求：拖到那个圆角矩形上就行，不必对准唱片）。
    // 登记在唱片之前 —— 后加的在上层，所以唱片与各控件仍然优先命中。
    if (dragging_) add(layout_.bgm_deck, 120, 1);
    add(layout_.bgm_disc, 100, 1);
    // 音波图要有曲目才有意义（没曲目就没有进度可跳）
    add(layout_.bgm_waveform, 101, 2, state_.bgm.has_track);
    // 音量条只登记**真正可拖的那一段**（右侧百分比文字不该能拖 ——
    // 用户明确指出来了）。绘制用的也是同一个矩形。
    add(ui::PanelLayout::volume_track(layout_.bgm_volume_slider), 103);
    add(ui::PanelLayout::volume_track(layout_.sfx_volume_slider), 107);
    // BGM 台那一行开关：循环 / 淡入淡出 / 混音。
    // 它们一直**画得出来但点不着** —— 因为命中区从来没登记过。
    // 矩形与绘制层同源（PanelLayout::option_rects），不是各算一份。
    {
        // 与绘制层**同一份标签列表**（顺序、数量都必须一致，否则框和点击区会错位）
        std::vector<std::string> labels;
        if (state_.bgm.has_track) {
            labels.push_back(state_.bgm.category.empty() ? std::string(tr("未分类", "Uncategorized"))
                                                         : state_.bgm.category);
            if (!state_.bgm.format.empty()) {
                labels.push_back(state_.bgm.format);
            }
        }
        labels.push_back(tr("循环", "Loop"));
        labels.push_back(tr("淡入淡出", "Fade"));
        labels.push_back(tr("混音", "Mix"));
        const std::vector<ui::Rect> items = layout_.option_rects(layout_.bgm_deck_line, labels);
        if (!dragging_) {
            for (std::size_t i = 0; i < items.size(); ++i) {
                const std::size_t from_end = items.size() - 1 - i;
                if (from_end < 3) {
                    add(items[i], 106 - static_cast<int>(from_end));
                }
            }
        }
    }

    // ---- BGM 台里的小按钮：必须在台身**之后**登记 ----
    //
    // 命中规则是"后加的在上层"，而台身（120）就是一块覆盖整个台的落点。
    // 先登记它们再登记台身的话，台身会把它们全盖住 —— 点上一首会变成
    // 落在台身上（自检里立刻红了）。
    //
    // 上一首 / 下一首**始终**登记：命中区按上一帧的 state_ 建，
    // 用 can_prev/can_next 去禁用会带来一帧延迟（"刚变成可点"的那一帧点不动）。
    // 边界情况交给 step_bgm 自己判断并说明。
    // **拖动中不登记这些按钮**：它们压在台身（落点）上面，会让"拖到台上"
    // 在划过按钮时变成"落在按钮上"，松手什么都不发生。
    // 拖动中这些按钮本来也没有用武之地。
    if (!dragging_) {
        add(layout_.bgm_prev, 110, 0);
        add(layout_.bgm_next, 111, 0);
        add(layout_.bgm_clear, 130, 0, state_.can_clear_bgm);
        add(layout_.sfx_clear, 131, 0, state_.can_clear_sfx);
    }

    // 音效台。整个台位也是落点（+5 这个编号留给"台身"），
    // 先登记台身，再登记里面的控件 —— 后者盖在上面，点它们仍然优先。
    for (std::size_t i = 0; i < sfx_slot_count; ++i) {
        if (dragging_) add(layout_.sfx_decks[i], 200 + static_cast<int>(i) * 10 + 5, 3);
    }
    for (std::size_t i = 0; i < sfx_slot_count; ++i) {
        const int base = 200 + static_cast<int>(i) * 10;
        // 拖动中只留唱片与音波图（它们是落点旁的可视物），按钮与音量条不登记：
        // 否则拖过音效台时落点会跳到按钮上，松手什么都不发生。
        add(layout_.sfx_discs[i], base + 0, 3);
        add(layout_.sfx_waveforms[i], base + 1, 4);
        if (!dragging_) {
            add(layout_.sfx_loop[i], base + 2);
            add(layout_.sfx_fade[i], base + 3);
            add(layout_.sfx_volumes[i], base + 4);
        }
    }

    // 两个搜索框
    add(layout_.bgm_search_input, 300);
    add(layout_.sfx_search_input, 301);

    // 分类芯片：两列各用自己的编号段
    {
        const std::vector<ui::Rect> bgm_chips = layout_.chip_rects(layout_.bgm_chips, state_.bgm_library.chip_widths);
        for (std::size_t i = 0; i < bgm_chips.size(); ++i) {
            if (bgm_chips[i].x > layout_.bgm_chips.right()) {
                break;
            }
            add(bgm_chips[i], 400 + static_cast<int>(i), 5);
        }
        const std::vector<ui::Rect> sfx_chips = layout_.chip_rects(layout_.sfx_chips, state_.sfx_library.chip_widths);
        for (std::size_t i = 0; i < sfx_chips.size(); ++i) {
            if (sfx_chips[i].x > layout_.sfx_chips.right()) {
                break;
            }
            add(sfx_chips[i], kSfxChipBase + static_cast<int>(i), 7);
        }
    }

    // 曲目卡：**最后加**，所以它们在层次最上面（与绘制顺序一致）。两列都要登记。
    for (std::size_t i = 0; i < bgm_.visible.size(); ++i) {
        const ui::Rect card = layout_.card_rect(layout_.bgm_grid, i, bgm_.scroll.offset());
        if (card.bottom() < layout_.bgm_grid.y || card.y > layout_.bgm_grid.bottom()) {
            continue;
        }
        add(card.intersect(layout_.bgm_grid), kCardBase + static_cast<int>(i), 6);
    }
    for (std::size_t i = 0; i < sfx_.visible.size(); ++i) {
        const ui::Rect card = layout_.card_rect(layout_.sfx_grid, i, sfx_.scroll.offset());
        if (card.bottom() < layout_.sfx_grid.y || card.y > layout_.sfx_grid.bottom()) {
            continue;
        }
        add(card.intersect(layout_.sfx_grid), kSfxCardBase + static_cast<int>(i), 8);
    }
}

bool Application::is_titlebar_button_point(POINT client) const noexcept {
    if (layout_.titlebar.height <= 0.0f) {
        return false;
    }
    const ui::Point point = to_ui_point(client);
    // 与 build_regions 里登记的三个矩形**同源**：这里按同样的规则重算，
    if (layout_.title_cn.contains(point) || layout_.title_en.contains(point)) return true;
    // 而不是各写一份坐标。按钮就三个，重算一遍比维护一份缓存更不容易错。
    const ui::Rect& bar = layout_.titlebar;

    for (int index = 0; index < 3; ++index) {
        const ui::Rect button{bar.right() - 40.0f - static_cast<float>(index) * 34.0f,
                              bar.y + 6.0f, 28.0f, bar.height - 12.0f};
        if (button.contains(point)) {
            return true;
        }
    }
    return false;
}

// 芯片要排几行？按实测宽度模拟一遍 flow。
//
// 布局的其余部分（网格起点、可视高度）都取决于这个行数，所以算出来之后
// 要重算一次布局 —— 不重算的话第二行芯片会压在第一排卡片上。
void Application::update_chip_rows() {
    const auto rows_for = [](const ui::Rect& container, const std::vector<float>& widths) {
        if (widths.empty() || container.width <= 0.0f) {
            return 1;
        }
        constexpr float kGap = 0.0f;
        int rows = 1;
        float x = 0.0f;
        for (const float width : widths) {
            const float w = std::min(container.width, width > 0.0f ? width : 70.0f);
            if (x > 0.0f && x + w > container.width) {
                ++rows;
                x = 0.0f;
            }
            x += w + kGap;
        }
        return rows;
    };

    const int bgm_rows = rows_for(layout_.bgm_chips, state_.bgm_library.chip_widths);
    const int sfx_rows = rows_for(layout_.sfx_chips, state_.sfx_library.chip_widths);
    if (bgm_rows == metrics_.bgm_chip_rows && sfx_rows == metrics_.sfx_chip_rows) {
        return;
    }
    metrics_.bgm_chip_rows = bgm_rows;
    metrics_.sfx_chip_rows = sfx_rows;
    layout_ = ui::compute_panel_layout(metrics_);
    // 布局变了 → 视口高变了 → 滚动范围也变了
    refresh_scroll_geometry();
    dirty_ = true;
}

void Application::refresh_scroll_geometry() {
    const auto apply = [this](Column& column, const ui::Rect& grid) {
        column.scroll.set_view_height(grid.height);
        column.scroll.set_content_height(
            layout_.card_content_height(grid, column.visible.size()));
    };
    apply(bgm_, layout_.bgm_grid);
    apply(sfx_, layout_.sfx_grid);
    // 内容高度变了，目标偏移要夹回合法范围
    bgm_scroll_target_ = std::clamp(bgm_scroll_target_, 0.0f, bgm_.scroll.maximum_offset());
    sfx_scroll_target_ = std::clamp(sfx_scroll_target_, 0.0f, sfx_.scroll.maximum_offset());
}

Application::Column& Application::column_at_point(ui::Point point) noexcept {
    // 按列矩形判断，**不按命中 id**。
    //
    // 原来是拿 topmost_at() 的结果去反解列，于是只在光标正好落在会话区/卡片/芯片
    // 上时才对；落在列的空白处（卡片之间的缝、网格下方）时 target 是 -1，
    // 于是一律滚左列 —— 用户在右列滚动却把左列滚了，就是"两列不独立"。
    if (layout_.sfx_column.width > 0.0f && point.x >= layout_.sfx_column.x) {
        return sfx_;
    }
    return bgm_;
}

void Application::sync_state() {
    state_.dj_mode = product_.sfx_dj_mode;
    state_.show_all_categories = product_.show_all_categories;
    state_.english = product_.ui_language == Language::english;
    state_.loading = loading_;
    const auto progress = loader_.progress();
    state_.load_fraction = progress.fraction;
    state_.load_title = progress.scanning ? tr("正在扫描曲库", "Scanning library") : tr("正在预加载音频", "Preloading audio");
    state_.load_current = progress.current;
    state_.load_detail = progress.scanning ? tr("正在读取曲目格式与时长", "Reading track formats and durations") :
        std::to_string(progress.done) + " / " + std::to_string(progress.total) +
        tr(" 首  ·  已准备 ", " tracks · Prepared ") + std::to_string(progress.resident_bytes / 1048576) + " MB";
    state_.sfx_library.pads = product_.sfx_dj_mode;
    state_.title = tr("音乐板", "MusicBoard");
    state_.status_chip = status_.empty() ? tr("就绪", "Ready") : status_;
    state_.library_label = path_to_utf8(library_root_);
    // 底栏历史条：把邻居的曲名填进去（按钮上写名字，才看得出"下一首会放到哪"）
    {
        const bool has_prev = history_index_ > 0;
        const bool has_next =
            !history_.empty() &&
            history_index_ + 1 < static_cast<std::ptrdiff_t>(history_.size());
        state_.can_prev = has_prev;
        state_.can_next = has_next;
        const auto name_at = [this](std::ptrdiff_t offset) {
            const std::ptrdiff_t at = history_index_ + offset;
            if (at < 0 || at >= static_cast<std::ptrdiff_t>(history_.size())) {
                return std::string();
            }
            const std::size_t index = history_[static_cast<std::size_t>(at)];
            return index < entries_.size() ? entries_[index].track.name : std::string();
        };
        state_.history_prev = has_prev ? name_at(-1) : std::string();
        state_.history_next = has_next ? name_at(1) : std::string();
    }
    state_.dragging = dragging_;
    // 拖拽幽灵跟着**当前**光标走，不是按下时的位置 —— 用按下位置的话它在原地不动，
    // 看起来就像拖动没生效。
    state_.drag_cursor = to_ui_point(cursor_client_);
    // 幽灵上要写"搬的是哪一首"。这一行原来漏了，于是 state_.drag_label 一直是空的，
    // 绘制层看到空标签就什么都不画 —— 表现就是"拖动时没有东西跟手"。
    state_.drag_label =
        (dragging_ && drag_index_ < entries_.size()) ? entries_[drag_index_].track.name : std::string();

    // 幽灵=被拖的那张卡本身，而且是**从抓取点**拎起来的：
    // 它的左上角 = 当前光标 - 按下时光标在卡片内的偏移。
    // 少了这个偏移，卡片会以光标为左上角（手感是"拽着卡片的一个角"）。
    state_.drag_origin_valid = false;
    if (dragging_ && drag_index_ < entries_.size()) {
        state_.drag_track = ui::TrackView{};
        state_.drag_track.name = entries_[drag_index_].label.title;
        state_.drag_track.tags = entries_[drag_index_].label.tags;
        state_.drag_track.category = entries_[drag_index_].category;
        state_.drag_track.badge = container_badge(entries_[drag_index_].track.container);
        state_.drag_track.duration_seconds = entries_[drag_index_].track.duration_seconds;
        state_.drag_track.path = entries_[drag_index_].track.path;
        state_.drag_track.playing = entries_[drag_index_].is_sfx ? false : (drag_index_ == bgm_index_);
        // 光标转逻辑坐标用现成的 to_ui_point（它已经处理了 DPI 缩放）
        const ui::Point cursor_logical = to_ui_point(cursor_client_);
        state_.drag_origin =
            ui::Point{cursor_logical.x - drag_grab_.x, cursor_logical.y - drag_grab_.y};
        // 尺寸也跟着被拖的那张卡（见按下的那一处说明）
        state_.drag_size = drag_size_;
        state_.drag_origin_valid = true;
    }
    // 动画时间：从启动算起的绝对秒数（不累加，掉帧不会让动画变慢）
    state_.always_on_top = product_.panel.topmost;
    state_.animation_time = static_cast<float>(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at_).count());

    // 芯片宽度是绘制层用 DirectWrite 量的；量好之后才知道要排几行
    update_chip_rows();

    // ---- 清空按钮：台上有东西才出现 ----
    state_.can_clear_bgm = engine_.slot_state(AudioBus::bgm, bgm_slot_).in_use ||
        std::any_of(pending_plays_.begin(), pending_plays_.end(),
                    [](const PendingPlay& item) { return item.sfx_deck < 0; });
    {
        bool any_sfx = false;
        for (const ui::DeckView& deck : state_.sfx) {
            if (deck.has_track) {
                any_sfx = true;
                break;
            }
        }
        state_.can_clear_sfx = engine_.active_count(AudioBus::sfx) > 0 || any_sfx ||
            std::any_of(pending_plays_.begin(), pending_plays_.end(),
                        [](const PendingPlay& item) { return item.sfx_deck >= 0; });
    }

    const int hovered = hit_.hovered();
    state_.hovered_id = hovered;
    // 悬停过渡量交给绘制层（卡片与芯片各一份）
    state_.bgm_library.hover_amounts = bgm_hover_amounts_;
    state_.sfx_library.hover_amounts = sfx_hover_amounts_;
    state_.bgm_library.chip_hover_amounts = bgm_chip_amounts_;
    state_.sfx_library.chip_hover_amounts = sfx_chip_amounts_;
    // 落点用的是**真实光标**下的区域；hit 的 hovered 在拖动时是拖动对象本身
    state_.drop_target_id = dragging_ ? hit_.topmost_at(to_ui_point(cursor_client_)) : -1;
    // 两列各自填自己的视图。
    // "正在播放"的高亮按**列**算：BGM 列看 bgm 总线，音效列看四个音效台。
    const auto fill_library = [this](ui::LibraryView& out, const Column& column,
                                     bool section_is_sfx, int chip_base, int card_base,
                                     int hovered_id) {
        out.search_text = column.search_text;
        out.search_placeholder = section_is_sfx ? tr("搜索音效…", "Search effects…") : tr("搜索音乐…", "Search music…");
        out.search_focused = column.search_focused;
        out.search_edit_time = column.search_edit_time;
        out.grid_scroll = column.scroll.offset();
        // 芯片（第一枚是"全部"）
        out.chips.clear();
        if (product_.show_all_categories) out.chips.push_back(tr("全部", "All"));
        for (const std::string& category : column.categories) {
            out.chips.push_back(category);
        }
        out.active_chip = column.active_chip + (product_.show_all_categories ? 1 : 0);
        if (out.chip_widths.size() != out.chips.size()) {
            out.chip_widths.assign(out.chips.size(), 0.0f);
            if (view_ready_) {
                view_.measure_chips(out);
            }
        }
        out.hovered_track = (hovered_id >= card_base && hovered_id < card_base + 1000)
                                ? hovered_id - card_base
                                : -1;
        out.hovered_chip = (hovered_id >= chip_base && hovered_id < chip_base + 100)
                               ? hovered_id - chip_base
                               : -1;

        out.tracks.clear();
        out.tracks.reserve(column.visible.size());
        for (const std::size_t index : column.visible) {
            ui::TrackView view;
            view.name = entries_[index].label.title;
            view.tags = entries_[index].label.tags;
            view.category = entries_[index].category;
            view.badge = container_badge(entries_[index].track.container);
            view.duration_seconds = entries_[index].track.duration_seconds;
            view.path = entries_[index].track.path;
            bool playing = false;
            if (section_is_sfx) {
                for (std::size_t deck = 0; deck < sfx_slot_count; ++deck) {
                    const VoiceSlotState slot = engine_.slot_state(AudioBus::sfx, deck);
                    if (audio_ready_ && slot.in_use && !slot.paused &&
                        sfx_index_[deck] == index) {
                        playing = true;
                    }
                }
            } else {
                const VoiceSlotState slot = engine_.slot_state(AudioBus::bgm, bgm_slot_);
                playing = audio_ready_ && slot.in_use && !slot.paused && index == bgm_index_;
            }
            if (section_is_sfx && product_.sfx_dj_mode) {
                const auto pad = engine_.slot_state(AudioBus::sfx, sfx_slot_count + index);
                playing = pad.playing;
                view.progress = pad.duration_seconds > 0 ? static_cast<float>(pad.position_seconds / pad.duration_seconds) : 0;
            }
            view.playing = playing;
            view.selected = dragging_ && drag_index_ == index && drag_from_sfx_ == section_is_sfx;
            out.tracks.push_back(std::move(view));
        }

        // 悬停预览：与搜索框同一个盒子里的那一段
        if (out.hovered_track >= 0 &&
            static_cast<std::size_t>(out.hovered_track) < column.visible.size()) {
            const std::size_t index =
                column.visible[static_cast<std::size_t>(out.hovered_track)];
            out.peek_active = true;
            out.peek_name = entries_[index].label.title;
            ensure_envelope(index);
            out.peek_envelope = entries_[index].envelope;
        } else {
            out.peek_active = false;
            out.peek_name.clear();
            out.peek_envelope.clear();
        }
    };

    fill_library(state_.bgm_library, bgm_, false, 400, 1000, hovered);
    fill_library(state_.sfx_library, sfx_, true, kSfxChipBase, kSfxCardBase, hovered);

    // BGM 台
    {
        ui::DeckView& deck = state_.bgm;
        const VoiceSlotState slot = engine_.slot_state(AudioBus::bgm, bgm_slot_);
        // **先清空再填。** state_ 是常驻的，只在"有曲目"分支里赋值的话，
        // 清空/放完/换曲时那些字段会留着上一首的值 —— 用户看到的就是
        // "清空了音波图还在"、"换曲后波形没重置"。这一条踩了两次，所以写在最前面。
        const bool had_track = deck.has_track;
        deck = ui::DeckView{};
        deck.has_track = audio_ready_ && slot.in_use;
        static_cast<void>(had_track);
        // **不用 slot.playing**：那个值要求 current_gain > 0，而淡入是从 0 开始的，
        // 于是拖上去之后的 1.2 秒里盘面不转、点击唱片也不像暂停（自检抓到的）。
        // 对界面来说"请求了播放且没暂停"就是播放中，音量大小由电平表单独表达。
        deck.playing = deck.has_track && !slot.paused;
        if (deck.has_track && bgm_index_ < entries_.size()) {
            deck.name = entries_[bgm_index_].label.title;
            deck.category = ui::tag_line(entries_[bgm_index_].label.tags);
            deck.format = container_badge(entries_[bgm_index_].track.container);
            deck.duration_seconds = entries_[bgm_index_].track.duration_seconds;
            deck.path = entries_[bgm_index_].track.path;
            deck.envelope = entries_[bgm_index_].envelope;
            // 拖动 seek 还没松手时，盘面显示**拖到的位置**，而不是播放位置 ——
            // 拖拽预览与播放位置抢着画就是"松手闪两下"的来源。
            deck.position_seconds = seeking_ && seek_slot_ < 0 && pending_seek_seconds_ >= 0.0
                                        ? pending_seek_seconds_
                                        : slot.position_seconds;
        }
        deck.looping = product_.bgm_loop;
        deck.fading = product_.bgm_fade;
        deck.mixing = product_.bgm_mix;
        deck.volume_percent = product_.bgm_volume_percent;
        // 只有真的落在这一台上才亮"可放下"的光圈；在别处拖着不亮。
        // 唱片与整块台身都算落点（120 是台身）
        deck.drop_target =
            state_.drop_target_id == 100 || state_.drop_target_id == 120;
        deck.level = deck.playing ? std::clamp(slot.current_gain, 0.0f, 1.0f) : 0.0f;
    }

    // 音效台
    for (std::size_t i = 0; i < sfx_slot_count; ++i) {
        ui::DeckView& deck = state_.sfx[i];
        const VoiceSlotState slot = engine_.slot_state(AudioBus::sfx, i);
        deck = ui::DeckView{};   // 同上：先清空再填，避免上一首的残留
        deck.has_track = audio_ready_ && slot.in_use;
        deck.playing = deck.has_track && !slot.paused;   // 同上：淡入期间也算在响
        if (deck.has_track && sfx_has_track_[i] && sfx_index_[i] < entries_.size()) {
            deck.name = entries_[sfx_index_[i]].label.title;
            deck.category = entries_[sfx_index_[i]].category;
            deck.format = container_badge(entries_[sfx_index_[i]].track.container);
            deck.duration_seconds = entries_[sfx_index_[i]].track.duration_seconds;
            deck.path = entries_[sfx_index_[i]].track.path;
            deck.envelope = entries_[sfx_index_[i]].envelope;
            deck.position_seconds =
                seeking_ && seek_slot_ == static_cast<int>(i) && pending_seek_seconds_ >= 0.0
                    ? pending_seek_seconds_
                    : slot.position_seconds;
        }
        deck.looping = product_.sfx_slots[i].loop;
        deck.fading = product_.sfx_slots[i].fade;
        deck.volume_percent = product_.sfx_slots[i].volume_percent;
        deck.drop_target = state_.drop_target_id == 200 + static_cast<int>(i) * 10 ||
                           state_.drop_target_id == 200 + static_cast<int>(i) * 10 + 5;
        deck.level = deck.playing ? std::clamp(slot.current_gain, 0.0f, 1.0f) : 0.0f;
    }

    state_.bgm_position = static_cast<float>(state_.bgm.position_seconds);
    state_.bgm_volume_percent = product_.bgm_volume_percent;
    state_.sfx_volume_percent = product_.sfx_volume_percent;
    state_.bgm_level = state_.bgm.level;
    state_.sfx_level = 0.0f;
    for (const ui::DeckView& deck : state_.sfx) {
        state_.sfx_level = std::max(state_.sfx_level, deck.level);
    }
    state_.sfx_hotkeys = hotkey_labels_;
    const double now = state_.animation_time;
    for (std::size_t i=0; i<disc_motion_.size(); ++i) {
        auto& deck = i == 0 ? state_.bgm : state_.sfx[i-1];
        disc_motion_[i].set_playing(deck.playing, now);
        deck.angle_degrees = static_cast<float>(disc_motion_[i].angle_at(now));
        deck.tonearm = disc_motion_[i].arm_at(now);
        const int disc_id = i == 0 ? 100 : 200 + static_cast<int>(i-1)*10;
        disc_hover_[i].set(deck.has_track && !dragging_ && hovered == disc_id, now);
        deck.hover = disc_hover_[i].at(now);
    }
    orb_hover_.set(orb_hover_zone_ != 0, now);
    orb_state_.hover = orb_hover_.at(now); orb_state_.hover_zone = orb_hover_zone_;
    orb_state_.has_track = state_.bgm.has_track; orb_state_.playing = state_.bgm.playing;
    orb_state_.progress = state_.bgm.duration_seconds > 0 ? static_cast<float>(state_.bgm.position_seconds / state_.bgm.duration_seconds) : 0;
    orb_state_.rotation_radians = state_.bgm.angle_degrees * 3.14159265f / 180.0f;
    orb_state_.amplitude = state_.bgm.level; orb_state_.accent = ui::kAmber; orb_state_.art_path = state_.bgm.path;
}

// ---------------------------------------------------------------------------
// 包络与播放
// ---------------------------------------------------------------------------

void Application::ensure_envelope(std::size_t index) {
    if (index >= entries_.size()) {
        return;
    }
    LibraryEntry& entry = entries_[index];
    if (entry.envelope_attempted) {
        return;   // 已经算过（成功就是有包络，失败也不再重试）
    }
    // **这里绝不解码。** 以前是同步解码整首歌再算包络，悬停一张没悬停过的卡
    // 会让界面卡几十到几百毫秒 —— 那就是"悬停有很大的延迟"。
    // 现在只排一个后台任务，算好了下一帧自然出现。
    //
    // 波形按需分块读取；播放请求会抢占正在提取的预览。
    envelopes_.request_urgent(index, entry.track.path);
    if (std::find(envelope_watch_.begin(), envelope_watch_.end(), index) ==
        envelope_watch_.end()) {
        envelope_watch_.push_back(index);
        if (envelope_watch_.size() > 64) {
            envelope_watch_.erase(envelope_watch_.begin());
        }
    }
    dirty_ = true;
}

void Application::collect_envelopes() {
    for (const std::size_t index : envelope_watch_) {
        if (index >= entries_.size() || entries_[index].envelope_attempted) continue;
        std::vector<float> envelope;
        bool success = false;
        Failure failure;
        if (!envelopes_.take(index, envelope, success, failure)) {
            envelopes_.request_urgent(index, entries_[index].track.path);
            continue;
        }
        entries_[index].envelope_attempted = true;
        if (success) entries_[index].envelope = std::move(envelope);
        else set_status(tr("波形提取失败: ", "Waveform extraction failed: ") + failure.to_string());
        dirty_ = true;
    }
    std::erase_if(envelope_watch_, [this](std::size_t index) {
        return index >= entries_.size() || entries_[index].envelope_attempted;
    });
}

void Application::cancel_pending_playback(int deck, std::size_t keep_index) {
    std::vector<std::size_t> removed;
    std::erase_if(pending_plays_, [&](const PendingPlay& item) {
        if (item.sfx_deck != deck) return false;
        removed.push_back(item.index);
        return true;
    });
    for (const std::size_t index : removed) {
        if (index != keep_index && std::none_of(pending_plays_.begin(), pending_plays_.end(),
                [index](const PendingPlay& item) { return item.index == index; })) {
            envelopes_.cancel_audio(index);
        }
    }
}

void Application::queue_playback(std::size_t index, int deck) {
    cancel_pending_playback(deck, index);
    if (loading_) { set_status(tr("曲库正在加载，请等待完成", "Library is loading; please wait")); return; }
    if (entries_[index].prepared) {
        ensure_envelope(index);
        static_cast<void>(start_playback(index, entries_[index].prepared->head,
            deck < 0 ? 0 : static_cast<std::size_t>(deck), deck >= 0));
        return;
    }
    if (!entries_[index].preload_failure.empty()) {
        set_status(tr("该曲目预加载失败: ", "Track preload failed: ") + entries_[index].preload_failure.to_string()); return;
    }
    pending_plays_.push_back(PendingPlay{index, deck});
    envelopes_.request_audio(index, entries_[index].track.path);
    if (!entries_[index].envelope_attempted &&
        std::find(envelope_watch_.begin(), envelope_watch_.end(), index) == envelope_watch_.end()) {
        envelope_watch_.push_back(index);
    }
    set_status(tr("正在载入  ", "Loading  ") + entries_[index].track.name);
    dirty_ = true;
}

void Application::clear_playback(bool is_sfx, bool fade) {
    const std::size_t count = is_sfx ? engine_.sfx_capacity() : AudioEngine::bgm_slot_count;
    for (std::size_t deck = 0; deck < count; ++deck) {
        cancel_pending_playback(is_sfx ? static_cast<int>(deck) : -1);
        Failure failure;
        const double duration = !fade ? 0.0 : (is_sfx ?
            (deck < sfx_slot_count && product_.sfx_slots[deck].fade ? 0.35 : 0.0) : (product_.bgm_fade ? 0.6 : 0.0));
        if (audio_ready_ && !engine_.stop(is_sfx ? AudioBus::sfx : AudioBus::bgm, deck, duration, failure)) {
            set_status(tr("清空失败: ", "Could not clear playback: ") + failure.to_string());
            return;
        }
        if (is_sfx && deck < sfx_slot_count && !fade) sfx_has_track_[deck] = false;
        else if (!is_sfx && !fade) { bgm_index_ = entries_.size(); bgm_paused_ = false; bgm_offset_ = 0.0; }
    }
    if (is_sfx) selected_sfx_index_ = entries_.size();
    set_status(is_sfx ? tr("音效台已清空", "Effects cleared") : tr("BGM 已清空", "BGM cleared"));
    dirty_ = true;
}

bool Application::play_bgm(std::size_t index) {
    if (index >= entries_.size()) {
        return false;
    }
    if (!audio_ready_) {
        set_status(tr("音频设备不可用，无法播放", "Audio device unavailable; cannot play"));
        return false;
    }
    const VoiceSlotState current = engine_.slot_state(AudioBus::bgm, bgm_slot_);
    if (index == bgm_index_ && current.in_use && !current.stopping) {
        cancel_pending_playback(-1);
        Failure failure;
        if (current.paused && !engine_.resume(AudioBus::bgm, bgm_slot_, failure, product_.bgm_fade ? 1.2 : 0.0)) {
            set_status(tr("继续失败: ", "Could not resume: ") + failure.to_string());
            return false;
        }
        bgm_paused_ = false;
        set_status(tr("继续播放  ", "Resuming  ") + entries_[index].track.name);
        return true;
    }
    queue_playback(index, -1);
    return true;
}

bool Application::start_playback(std::size_t index, const DecodedAudio& audio,
                                 std::size_t sfx_deck, bool is_sfx) {
    if (index >= entries_.size()) {
        return false;
    }
    if (is_sfx) {
        const double fade = product_.sfx_slots[sfx_deck].fade ? 0.35 : 0.0;
        Failure play_failure;
        const bool played = entries_[index].prepared
            ? engine_.play_prepared(AudioBus::sfx, sfx_deck, entries_[index].prepared, entries_[index].track.name,
                fade, product_.sfx_slots[sfx_deck].loop, play_failure)
            : engine_.play(AudioBus::sfx, sfx_deck, audio, entries_[index].track.name,
                fade, product_.sfx_slots[sfx_deck].loop, play_failure);
        if (!played) {
            set_status(tr("播放失败: ", "Playback failed: ") + play_failure.to_string());
            return false;
        }
        sfx_index_[sfx_deck] = index;
        sfx_has_track_[sfx_deck] = true;
        set_status(tr("音效 ", "Effect ") + std::to_string(sfx_deck + 1) + "  " + entries_[index].track.name);
        dirty_ = true;
        return true;
    }

    const auto previous = bgm_slot_;
    const auto incoming = 1u - previous;
    const bool has_outgoing = engine_.slot_state(AudioBus::bgm, previous).in_use;
    const double fade = has_outgoing && product_.bgm_mix ? 2.5 : (product_.bgm_fade ? 1.2 : 0.0);
    Failure play_failure;
    const bool played = entries_[index].prepared
        ? engine_.play_prepared(AudioBus::bgm, incoming, entries_[index].prepared, entries_[index].track.name,
            fade, product_.bgm_loop, play_failure)
        : engine_.play(AudioBus::bgm, incoming, audio, entries_[index].track.name, fade, product_.bgm_loop, play_failure);
    if (!played) {
        set_status(tr("播放失败: ", "Playback failed: ") + play_failure.to_string());
        return false;
    }

    const double transition = product_.bgm_mix ? 2.5 : (product_.bgm_fade ? 0.6 : 0.0);
    if (!engine_.set_end_fade(AudioBus::bgm, incoming, product_.bgm_fade ? 1.2 : 0.0, play_failure) ||
        !engine_.transition_bgm(previous, incoming, transition, product_.bgm_mix, play_failure)) {
        Failure stop_failure;
        if (!engine_.stop(AudioBus::bgm, incoming, 0, stop_failure))
            report(LogLevel::error, "Failed incoming cleanup", stop_failure.to_string());
        set_status(tr("混音切换失败: ", "Mix transition failed: ") + play_failure.to_string()); return false;
    }
    bgm_slot_ = incoming;
    report(LogLevel::info, "BGM_TRANSITION", "from=" + std::to_string(previous) + " to=" +
        std::to_string(incoming) + " mix=" + std::to_string(product_.bgm_mix) + " seconds=" + std::to_string(transition));

    // 记进播放历史（原型的 hist：按"实际放过的顺序"走，不是列表顺序）。
    //
    // 比较的是**当前位置那一条**，不是末尾那一条 —— 这一点很关键：
    // 点"上一首"回去时，要放的那首正好就是 history_[history_index_]，
    // 按"末尾"比较会把它当成新歌追加进去，于是历史越点越长、
    // "下一首"永远变回不可点（自检抓到的就是这个）。
    if (history_index_ < 0 ||
        history_[static_cast<std::size_t>(history_index_)] != index) {
        // 往回跳就截断之后的部分（浏览器式的历史）
        history_.resize(static_cast<std::size_t>(history_index_ + 1));
        history_.push_back(index);
        if (history_.size() > 50) {
            history_.erase(history_.begin());
        }
        history_index_ = static_cast<std::ptrdiff_t>(history_.size()) - 1;
    }

    auto recent = std::find_if(recent_bgm_.begin(), recent_bgm_.end(), [&](const auto& item) { return item.path == entries_[index].track.path; });
    if (recent == recent_bgm_.end()) { recent_bgm_.push_back({entries_[index].track.path, 0, 0}); recent = std::prev(recent_bgm_.end()); }
    ++recent->count; recent->order = ++recent_order_;
    bgm_index_ = index;
    bgm_paused_ = false;
    bgm_offset_ = 0.0;
    last_drawn_progress_ = -1.0;
    set_status(tr("正在播放  ", "Playing  ") + entries_[index].track.name);
    dirty_ = true;
    return true;
}

void Application::collect_pending_playback() {
    std::size_t cursor = 0;
    while (cursor < pending_plays_.size()) {
        const std::size_t index = pending_plays_[cursor].index;
        DecodedAudio audio;
        bool decoded = false;
        Failure failure;
        if (!envelopes_.take_audio(index, audio, decoded, failure)) { ++cursor; continue; }
        // One decode can serve all decks requesting this same track.
        std::vector<int> decks;
        std::erase_if(pending_plays_, [&](const PendingPlay& item) {
            if (item.index != index) return false;
            decks.push_back(item.sfx_deck);
            return true;
        });
        if (!decoded) set_status(tr("解码失败: ", "Decode failed: ") + failure.to_string());
        else for (const int deck : decks) {
            if (!start_playback(index, audio, deck < 0 ? 0 : static_cast<std::size_t>(deck), deck >= 0)) {
                // start_playback already reports the engine's original Failure.
                break;
            }
        }
        cursor = 0;
    }
}

void Application::toggle_bgm_pause() {
    if (!audio_ready_) {
        return;
    }
    const VoiceSlotState slot = engine_.slot_state(AudioBus::bgm, bgm_slot_);
    if (!slot.in_use) {
        return;   // 空盘上点击没有反应 —— 这正是设计稿要求的
    }
    Failure failure;
    if (slot.paused) {
        if (engine_.resume(AudioBus::bgm, bgm_slot_, failure, product_.bgm_fade ? 1.2 : 0.0)) {
            bgm_paused_ = false;
            set_status(tr("继续  ", "Resumed  ") + slot.name);
        }
    } else {
        if (!engine_.stop(AudioBus::bgm, 1u - bgm_slot_, product_.bgm_fade ? 0.6 : 0.0, failure)) {
            set_status(tr("暂停失败: ", "Could not pause: ") + failure.to_string()); return;
        }
        // 暂停时**记住当前位置**，恢复时从那里继续（不是回到 0）
        bgm_offset_ = slot.position_seconds;
        if (engine_.pause(AudioBus::bgm, bgm_slot_, failure, product_.bgm_fade ? 0.6 : 0.0)) {
            bgm_paused_ = true;
            set_status(tr("已暂停  ", "Paused  ") + slot.name);
        }
    }
}

void Application::step_bgm(int direction) {
    // 上一首 / 下一首走的是**播放历史**（原型的 hist）。
    // 列表里的相邻曲目不是"上一首" —— 用户想要的是"回到我刚才放的那首"。
    if (history_.empty()) {
        set_status(tr("还没有播放历史", "No playback history yet"));
        return;
    }
    const std::ptrdiff_t next = history_index_ + (direction > 0 ? 1 : -1);
    if (next < 0 || next >= static_cast<std::ptrdiff_t>(history_.size())) {
        set_status(direction > 0 ? tr("已经是最后一首", "Already at the last track") : tr("已经是第一首", "Already at the first track"));
        return;
    }
    history_index_ = next;
    const std::size_t index = history_[static_cast<std::size_t>(history_index_)];
    if (index >= entries_.size()) {
        set_status(tr("历史里的曲目已经不在曲库里了", "This history track is no longer in the library"));
        return;
    }
    static_cast<void>(play_bgm(index));
    scroll_visible_into_view(index);
}

void Application::scroll_visible_into_view(std::size_t index) {
    // 自动滚到当前曲目：换曲之后它可能已经滚出可见区，用户就看不到"在放的是哪首"
    const auto found = std::find(bgm_.visible.begin(), bgm_.visible.end(), index);
    if (found == bgm_.visible.end()) {
        return;
    }
    const auto position = static_cast<std::size_t>(std::distance(bgm_.visible.begin(), found));
    const std::size_t columns = layout_.card_columns(layout_.bgm_grid);
    if (columns == 0) {
        return;
    }
    const std::size_t row = position / columns;
    const float row_height = layout_.track_card_height + layout_.track_grid_gap;
    const float top = static_cast<float>(row) * row_height;
    // 目标是"滚到这一行的上下各留一行"，比严格要求完全可见更好用
    const float margin = row_height;
    float desired = top - margin;
    if (top + layout_.track_card_height > bgm_.scroll.offset() + layout_.bgm_grid.height) {
        desired = top + layout_.track_card_height + margin - layout_.bgm_grid.height;
    }
    bgm_scroll_target_ =
        std::clamp(desired, 0.0f, bgm_.scroll.maximum_offset());
    dirty_ = true;
}

bool Application::trigger_sfx(std::size_t deck, std::size_t index) {
    if (deck >= sfx_slot_count || index >= entries_.size()) {
        return false;
    }
    if (!audio_ready_) {
        set_status(tr("音频设备不可用，无法播放音效", "Audio device unavailable; cannot play effects"));
        return false;
    }
    queue_playback(index, static_cast<int>(deck));
    return true;
}

void Application::seek_bgm(double seconds) {
    if (!audio_ready_) {
        return;
    }
    Failure failure;
    if (!engine_.seek(AudioBus::bgm, bgm_slot_, seconds, failure)) {
        set_status(tr("跳转失败: ", "Seek failed: ") + failure.to_string());
        return;
    }
    bgm_offset_ = seconds;
    last_drawn_progress_ = -1.0;
    dirty_ = true;
}

void Application::commit_pending_seek() {
    if (pending_seek_seconds_ < 0.0) {
        return;
    }
    const double target = pending_seek_seconds_;
    pending_seek_seconds_ = -1.0;

    Failure failure;
    if (seek_slot_ < 0) {
        // BGM：松手才真正 seek（连续 seek 会爆音）
        if (engine_.seek(AudioBus::bgm, bgm_slot_, target, failure)) {
            bgm_offset_ = target;
        }
    } else if (seek_slot_ < static_cast<int>(sfx_slot_count)) {
        // 音效台：拖动中就能听到位置变化（音效很短，不存在爆音问题）
        static_cast<void>(
            engine_.seek(AudioBus::sfx, static_cast<std::size_t>(seek_slot_), target, failure));
    }
    last_drawn_progress_ = -1.0;
    dirty_ = true;
}

void Application::apply_volume_changes() {
    if (!audio_ready_) {
        return;
    }
    Failure ignored;
    static_cast<void>(engine_.set_bus_volume(
        AudioBus::bgm, static_cast<float>(product_.bgm_volume_percent) / 100.0f, ignored));
    static_cast<void>(engine_.set_bus_volume(
        AudioBus::sfx, static_cast<float>(product_.sfx_volume_percent) / 100.0f, ignored));
    for (std::size_t i = 0; i < sfx_slot_count; ++i) {
        static_cast<void>(engine_.set_slot_volume(
            AudioBus::sfx, i, static_cast<float>(product_.sfx_slots[i].volume_percent) / 100.0f,
            ignored));
    }
}

// ---------------------------------------------------------------------------
// 输入
// ---------------------------------------------------------------------------

bool Application::input_trace_enabled() {
    static const bool enabled = [] {
        wchar_t buffer[8]{};
        return ::GetEnvironmentVariableW(L"MUSICBOARD_TRACE_INPUT", buffer,
                                        static_cast<DWORD>(std::size(buffer))) > 0;
    }();
    return enabled;
}

void Application::trace_input(const char* what, POINT client) const {
    if (!input_trace_enabled()) {
        return;
    }
    const int target = hit_.topmost_at(to_ui_point(client));
    report(LogLevel::info, what,
           tr("客户区 ", "Client ") + std::to_string(client.x) + "," + std::to_string(client.y) + tr(" 命中 id=", " hit id=") +
               std::to_string(target) + " dragging=" + (dragging_ ? "1" : "0") +
               " captured=" + std::to_string(hit_.captured()) + " dpi=" + std::to_string(g_panel_window->dpi()) +
               " logical=" + std::to_string(to_ui_point(client).x) + "," + std::to_string(to_ui_point(client).y));
}

HCURSOR Application::cursor_for_client_point(POINT client) const {
    // 拖动中：四向箭头（"正在搬东西"）
    if (dragging_) {
        return ::LoadCursorW(nullptr, IDC_SIZEALL);
    }
    const int target = hit_.topmost_at(to_ui_point(client));
    {
        bool card_is_sfx = false;
        int card_index = -1;
        if (card_of_id(target, card_is_sfx, card_index)) {
            if (card_is_sfx && product_.sfx_dj_mode) return ::LoadCursorW(nullptr, IDC_HAND);
            return ::LoadCursorW(nullptr, IDC_SIZEALL);   // 曲目卡：可以拖，也可以点
        }
    }
    if (target == kSearchIdBgm || target == kSearchIdSfx) return ::LoadCursorW(nullptr, IDC_IBEAM);
    if (target >= 0) {
        return ::LoadCursorW(nullptr, IDC_HAND);      // 唱片 / 按钮 / 芯片 / 音量条
    }
    return ::LoadCursorW(nullptr, IDC_ARROW);
}

void Application::apply_slider_drag(POINT client) {
    // 客户区坐标 → 逻辑坐标。**必须换算**：布局是逻辑像素，直接拿客户区像素
    // 去比在 150% 缩放下会偏掉三分之一（拖动位置和滑块位置对不上）。
    const ui::Point point = to_ui_point(client);
    const auto horizontal_ratio = [&point](const ui::Rect& slider) {
        if (slider.width <= 1.0f) {
            return 0.0;
        }
        return std::clamp(static_cast<double>(point.x - slider.x) / slider.width, 0.0, 1.0);
    };
    const auto vertical_ratio = [&point](const ui::Rect& slider) {
        if (slider.height <= 1.0f) {
            return 0.0;
        }
        // 竖向：从下往上
        return std::clamp(
            1.0 - static_cast<double>(point.y - slider.y) / slider.height, 0.0, 1.0);
    };

    if (slider_drag_target_ == 103) {
        product_.bgm_volume_percent = static_cast<std::uint16_t>(
            horizontal_ratio(ui::PanelLayout::volume_track(layout_.bgm_volume_slider)) *
                maximum_volume_percent + 0.5);
    } else if (slider_drag_target_ == 107) {
        product_.sfx_volume_percent = static_cast<std::uint16_t>(
            horizontal_ratio(ui::PanelLayout::volume_track(layout_.sfx_volume_slider)) *
                maximum_volume_percent + 0.5);
    } else if (slider_drag_target_ >= 200 && slider_drag_target_ < 300 &&
               (slider_drag_target_ - 200) % 10 == 4) {
        const std::size_t deck = static_cast<std::size_t>((slider_drag_target_ - 200) / 10);
        if (deck < sfx_slot_count) {
            product_.sfx_slots[deck].volume_percent = static_cast<std::uint16_t>(
                vertical_ratio(layout_.sfx_volumes[deck]) * maximum_volume_percent + 0.5);
        }
    } else {
        return;
    }
    apply_volume_changes();
    dirty_ = true;
}

void Application::on_mouse_move(POINT client) {
    cursor_client_ = client;
    hit_.set_cursor(to_ui_point(client));

    if (dragging_) {
        hit_.set_drag_position(to_ui_point(client));
        const int dx = client.x - drag_press_.x;
        const int dy = client.y - drag_press_.y;
        if (!drag_moved_ && dx * dx + dy * dy >= kDragThreshold * kDragThreshold) {
            drag_moved_ = true;
        }
        dirty_ = true;
        return;
    }

    // 正在拖音量条
    if (slider_drag_target_ >= 0) {
        apply_slider_drag(client);
        return;
    }

    // 拖动音波图（seek）
    if (seeking_) {
        const ui::Rect& wave = seek_slot_ < 0 ? layout_.bgm_waveform
                                              : layout_.sfx_waveforms[static_cast<std::size_t>(seek_slot_)];
        if (wave.width > 1.0f) {
            // 同一个换算问题：wave 是逻辑坐标，client 是物理像素。
            const double ratio = std::clamp(
                static_cast<double>(to_ui_point(client).x - wave.x) / wave.width, 0.0, 1.0);
            const double duration =
                seek_slot_ < 0
                    ? engine_.slot_state(AudioBus::bgm, bgm_slot_).duration_seconds
                    : engine_.slot_state(AudioBus::sfx, static_cast<std::size_t>(seek_slot_))
                          .duration_seconds;
            pending_seek_seconds_ = ratio * duration;
            dirty_ = true;
            // 音效台拖动中立即提交；BGM 等到松手
            if (seek_slot_ >= 0) {
                commit_pending_seek();
            }
        }
        return;
    }

    // 悬停时把目标曲目的包络提前算好，这样预览里的波形是现成的
    const int hovered = hit_.hovered();
    bool hover_is_sfx = false;
    int hover_index = -1;
    if (card_of_id(hovered, hover_is_sfx, hover_index)) {
        const Column& column = hover_is_sfx ? sfx_ : bgm_;
        if (hover_index >= 0 && static_cast<std::size_t>(hover_index) < column.visible.size()) {
            const std::size_t index = column.visible[static_cast<std::size_t>(hover_index)];
            ensure_envelope(index);
        }
    }
    dirty_ = true;
}

void Application::on_left_down(POINT client) {
    hit_.set_cursor(to_ui_point(client));
    hit_.set_left_pressed(true);
    hit_.set_left_down(true);

    const int target = hit_.topmost_at(to_ui_point(client));
    if (target < 0) {
        bgm_.search_focused = false;
        sfx_.search_focused = false;
        dirty_ = true;
        return;
    }

    if (target == 11 || target == 12) { set_ui_language(target == 12 ? Language::english : Language::chinese); return; }
    if (target == 9 && !loading_) { set_sfx_collapsed(!metrics_.sfx_collapsed); return; }
    if (target == 10 && !loading_) { set_show_all_categories(!product_.show_all_categories); return; }
    if (target == 8) { loader_.cancel(); set_status(tr("正在取消加载", "Cancelling load")); return; }
    if (target == 5) { if (!loading_) begin_library_load(library_root_); return; }
    if (target == 6) { if (!loading_) choose_new_library(); return; }
    if (loading_ && target != 2 && target != 3 && target != 4) return;
    if (target == 7) { set_dj_mode(to_ui_point(client).x >= layout_.sfx_mode.x + 88.0f); return; }
    // 标题栏按钮
    if (target == 2) {
        ::PostMessageW(g_panel_window->handle(), WM_CLOSE, 0, 0);
        return;
    }
    if (target == 3) {
        Failure failure;
        if (!toggle_panel_topmost(failure)) set_status(tr("切换置顶失败: ", "Could not change pin state: ") + failure.to_string());
        else set_status(product_.panel.topmost ? tr("已置顶", "Always on top") : tr("取消置顶", "Always on top off"));
        return;
    }
    if (target == 4) { set_panel_visible(false); return; }

    // 清空：BGM 一路 / 音效四路（用户："在该台上有音乐的情况下可以清空当前播放"）
    if (target == 130 || target == 131) {
        clear_playback(target == 131, true);
        return;
    }

    // 上一首 / 下一首
    if (target == 110) {
        step_bgm(-1);
        return;
    }
    if (target == 111) {
        step_bgm(1);
        return;
    }

    // BGM 台的三个开关：循环 / 淡入淡出 / 混音
    if (target >= 104 && target <= 106) {
        if (target == 104) {
            product_.bgm_loop = !product_.bgm_loop;
            // 与音效台同理：**必须推到引擎**，否则"关掉循环还在放"。
            if (audio_ready_) {
                Failure failure;
                if (!engine_.set_loop(AudioBus::bgm, bgm_slot_, product_.bgm_loop, failure)) {
                    set_status(tr("切换循环失败: ", "Could not change loop: ") + failure.to_string());
                }
            }
            set_status(product_.bgm_loop ? tr("循环：开", "Loop on") : tr("循环：关（播完停下）", "Loop off (stop at end)"));
        } else if (target == 105) {
            product_.bgm_fade = !product_.bgm_fade;
            Failure failure;
            if (!engine_.set_end_fade(AudioBus::bgm, bgm_slot_, product_.bgm_fade ? 1.2 : 0.0, failure))
                report(LogLevel::error, "Set end fade", failure.to_string());
            set_status(product_.bgm_fade ? tr("淡入淡出：开", "Fade on") : tr("淡入淡出：关", "Fade off"));
        } else {
            product_.bgm_mix = !product_.bgm_mix;
            set_status(product_.bgm_mix ? tr("混音：开（交叉淡化切歌）", "Mix on (crossfade tracks)") : tr("混音：关", "Mix off"));
        }
        save_settings();
        dirty_ = true;
        return;
    }

    // 搜索框 → 获得焦点（真正的文本输入由 WM_CHAR 处理）。
    // 两列各有一个搜索框：点右列那个必须把焦点给右列，否则输入会跑到左列的筛选里。
    if (target == kSearchIdBgm || target == kSearchIdSfx) {
        const bool sfx_focus = target == kSearchIdSfx;
        bgm_.search_focused = !sfx_focus;
        sfx_.search_focused = sfx_focus;
        active_search_column().search_edit_time = std::chrono::duration<double>(std::chrono::steady_clock::now()-started_at_).count();
        dirty_ = true;
        return;
    }
    bgm_.search_focused = false;
    sfx_.search_focused = false;

    // 曲目卡 → 开始可能的拖动（单击本身只选中，不播放）
    {
        bool card_is_sfx = false;
        int card_index = -1;
        if (card_of_id(target, card_is_sfx, card_index)) {
            const Column& column = card_is_sfx ? sfx_ : bgm_;
            if (card_index >= 0 && static_cast<std::size_t>(card_index) < column.visible.size()) {
                if (card_is_sfx && product_.sfx_dj_mode) {
                    static_cast<void>(trigger_pad(column.visible[static_cast<std::size_t>(card_index)]));
                    return;
                }
                hit_.capture(target);
                hit_.set_capture_origin(to_ui_point(client));
                hit_.set_drag_position(to_ui_point(client));
                // 光标在卡片内的偏移（逻辑坐标）：幽灵按它对齐，手感才是"拎起这张卡"
                {
                    const ui::Rect card = layout_.card_rect(
                        card_is_sfx ? layout_.sfx_grid : layout_.bgm_grid,
                        static_cast<std::size_t>(card_index),
                        (card_is_sfx ? sfx_ : bgm_).scroll.offset());
                    const ui::Point point = to_ui_point(client);
                    drag_grab_ = ui::Point{std::clamp(point.x - card.x, 0.0f, card.width),
                                           std::clamp(point.y - card.y, 0.0f, card.height)};
                    // 幽灵的尺寸取**这一列**卡片的实际尺寸。
                    // 之前用的是"左列的卡片宽度"—— 右列（音效）比左列窄，
                    // 于是拖右列的卡时幽灵宽出一大截（用户看到的"宽度过长"）。
                    drag_size_ = ui::Point{card.width, card.height};
                }
                dragging_ = true;
                drag_moved_ = false;
                // 记的是**全局条目下标**：拖到唱片上时由唱片决定放哪条总线，
                // 所以音乐卡拖到音效台上是合法的（设计稿里明确要支持）。
                drag_index_ = column.visible[static_cast<std::size_t>(card_index)];
                drag_from_sfx_ = card_is_sfx;
                if (card_is_sfx) selected_sfx_index_ = drag_index_;
                drag_press_ = client;
                // 抓住鼠标：手一快划出窗口，WM_MOUSEMOVE 就不再来，拖动会莫名其妙断在半路
                // （原型里靠 pointer capture 解决同一件事）。
                ::SetCapture(g_panel_window->handle());
                set_status(tr("拖到唱片上播放  ", "Drag onto a record to play  ") + entries_[drag_index_].track.name);
            }
            return;
        }
    }

    // 分类芯片 → 筛选（两列各筛自己）
    {
        bool chip_is_sfx = false;
        int chip_index = -1;
        if (chip_of_id(target, chip_is_sfx, chip_index)) {
            Column& column = chip_is_sfx ? sfx_ : bgm_;
            column.active_chip = chip_index - (product_.show_all_categories ? 1 : 0);
            // 换分类后滚动位置必须回零：不然新列表比旧列表短时，
            // 用户会看到一片空白（偏移还在下面）。
            column.scroll.set_offset(0.0f);
            rebuild_visible(column, chip_is_sfx);
            return;
        }
    }

    // BGM 唱片 → 暂停/继续
    if (target == 100) {
        toggle_bgm_pause();
        return;
    }

    // 音波图 → seek
    if (target == 101) {
        ::SetCapture(g_panel_window->handle());
        seeking_ = true;
        seek_slot_ = -1;
        on_mouse_move(client);
        return;
    }

    // 音量条 → 开始拖动（按下就跳到点的位置，然后跟着鼠标走）
    if (target == 103 || target == 107 || (target >= 200 && target < 300 && (target - 200) % 10 == 4)) {
        ::SetCapture(g_panel_window->handle());
        slider_drag_target_ = target;
        apply_slider_drag(client);
        return;
    }


    // 音效台
    if (target >= 200 && target < 300) {
        const std::size_t deck = static_cast<std::size_t>((target - 200) / 10);
        const int part = (target - 200) % 10;
        if (deck >= sfx_slot_count) {
            return;
        }
        switch (part) {
            case 0: {
                // 点击音效唱片 = 暂停 / 继续。
                //
                // 之前这里是"停止"（把台上清掉），而音效台上没有别的重新开始入口，
                // 一停就只能重新拖一次。BGM 那边是暂停/继续，音效台跟着一致才不别扭。
                if (!audio_ready_) {
                    break;
                }
                const VoiceSlotState slot = engine_.slot_state(AudioBus::sfx, deck);
                if (!slot.in_use) {
                    break;   // 空台点上没有反应（与空盘一致）
                }
                Failure failure;
                if (slot.paused) {
                    if (engine_.resume(AudioBus::sfx, deck, failure, product_.sfx_slots[deck].fade ? 0.35 : 0.0)) {
                        set_status(tr("音效 ", "Effect ") + std::to_string(deck + 1) + tr(" 继续  ", " resumed  ") + slot.name);
                    }
                } else {
                    if (engine_.pause(AudioBus::sfx, deck, failure, product_.sfx_slots[deck].fade ? 0.35 : 0.0)) {
                        set_status(tr("音效 ", "Effect ") + std::to_string(deck + 1) + tr(" 已暂停  ", " paused  ") + slot.name);
                    }
                }
                dirty_ = true;
                break;
            }
            case 1:
                ::SetCapture(g_panel_window->handle());
                seeking_ = true;
                seek_slot_ = static_cast<int>(deck);
                on_mouse_move(client);
                break;
            case 2: {
                const bool loop = !product_.sfx_slots[deck].loop;
                product_.sfx_slots[deck].loop = loop;
                // **必须推到引擎上。** 只改设置的话，正在循环的那条声音不会停 ——
                // 用户看到的就是"循环按钮关掉了还在一直放"。
                if (audio_ready_) {
                    Failure failure;
                    if (!engine_.set_loop(AudioBus::sfx, deck, loop, failure)) {
                        set_status(tr("切换循环失败: ", "Could not change loop: ") + failure.to_string());
                    }
                }
                save_settings();
                dirty_ = true;
                break;
            }
            case 3: {
                product_.sfx_slots[deck].fade = !product_.sfx_slots[deck].fade;
                Failure failure;
                if (!engine_.set_end_fade(AudioBus::sfx, deck, product_.sfx_slots[deck].fade ? 0.35 : 0.0, failure))
                    set_status(tr("音效淡化设置失败: ", "Could not set effect fade: ") + failure.to_string());
                save_settings(); dirty_ = true; break;
            }
            case 4:
                // 竖向音量条：按下就交给 apply_slider_drag（在 on_left_down 更前面
                // 统一处理），这里不会走到 —— 保留分支是为了说明它归谁管。
                break;
            default:
                break;
        }
    }
}

void Application::cancel_pointer_gesture() {
    const bool save_volume = slider_drag_target_ >= 0;
    dragging_ = false;
    drag_moved_ = false;
    seeking_ = false;
    seek_slot_ = -1;
    pending_seek_seconds_ = -1.0;
    slider_drag_target_ = -1;
    hit_.set_left_down(false);
    hit_.release_capture();
    if (g_panel_window && ::GetCapture() == g_panel_window->handle()) ::ReleaseCapture();
    if (save_volume) save_settings();
    dirty_ = true;
}

void Application::on_left_up(POINT client) {
    hit_.set_left_released(true);
    hit_.set_left_down(false);

    if (slider_drag_target_ >= 0) {
        // 松手才落盘：拖动过程中每次鼠标移动都写设置文件是没必要的 IO
        apply_slider_drag(client);
        slider_drag_target_ = -1;
        save_settings();
        if (::GetCapture() == g_panel_window->handle()) ::ReleaseCapture();
        return;
    }

    if (seeking_) {
        seeking_ = false;
        commit_pending_seek();
        seek_slot_ = -1;
        if (::GetCapture() == g_panel_window->handle()) ::ReleaseCapture();
        return;
    }

    if (dragging_) {
        dragging_ = false;
        if (::GetCapture() == g_panel_window->handle()) {
            ::ReleaseCapture();
        }
        const ui::Point drop_point = to_ui_point(client);
        int dropped = layout_.bgm_deck.contains(drop_point) ? 120 : -1;
        for (std::size_t deck = 0; deck < sfx_slot_count; ++deck) {
            if (layout_.sfx_decks[deck].contains(drop_point)) dropped = 205 + static_cast<int>(deck) * 10;
        }
        hit_.release_capture();

        if (input_trace_enabled()) {
            report(LogLevel::info, "drop",
                   "dropped=" + std::to_string(dropped) + " moved=" +
                       (drag_moved_ ? "1" : "0") + " drag_index=" + std::to_string(drag_index_));
        }
        if (drag_moved_ && dropped >= 0) {
            // 落在台身的任何位置都算（唱片 = +0，台身 = +5）
            const std::size_t deck = static_cast<std::size_t>((dropped - 200) / 10);
            const bool on_sfx_deck =
                dropped >= 200 && dropped < 300 && deck < sfx_slot_count;
            if (on_sfx_deck) {
                static_cast<void>(trigger_sfx(deck, drag_index_));
            } else if (dropped == 100 || dropped == 120) {
                static_cast<void>(play_bgm(drag_index_));
            }
        } else {
            // 没移动就是单击：只选中，不播放（跑团现场最怕误触发）
            set_status(tr("已选中  ", "Selected  ") + entries_[drag_index_].track.name +
                       tr("（拖到唱片上播放，按快捷键触发音效）", " (drag onto a record or use a shortcut to play)"));
        }
        dirty_ = true;
    }
}

void Application::on_right_up(POINT client) {
    if (loading_) return;
    bool pad_sfx = false; int pad_index = -1;
    if (product_.sfx_dj_mode && card_of_id(hit_.topmost_at(to_ui_point(client)), pad_sfx, pad_index) &&
        pad_sfx && pad_index >= 0 && static_cast<std::size_t>(pad_index) < sfx_.visible.size()) {
        Failure failure;
        if (!engine_.stop(AudioBus::sfx, sfx_slot_count + sfx_.visible[static_cast<std::size_t>(pad_index)], 0.0, failure))
            set_status(tr("停止音效失败: ", "Could not stop effect: ") + failure.to_string());
        dirty_ = true; return;
    }
    // Right-click routes to the card's own bus. Paused/loading decks are occupied.
    const int target = hit_.topmost_at(to_ui_point(client));
    {
        bool card_is_sfx = false;
        int card_index = -1;
        if (card_of_id(target, card_is_sfx, card_index)) {
            const Column& column = card_is_sfx ? sfx_ : bgm_;
            if (card_index >= 0 && static_cast<std::size_t>(card_index) < column.visible.size()) {
                const auto index = column.visible[static_cast<std::size_t>(card_index)];
                if (card_is_sfx) {
                    std::size_t destination = 0;
                    for (std::size_t deck = 0; deck < sfx_slot_count; ++deck) {
                        const bool pending = std::any_of(pending_plays_.begin(), pending_plays_.end(),
                            [deck](const PendingPlay& play) { return play.sfx_deck == static_cast<int>(deck); });
                        if (!engine_.slot_state(AudioBus::sfx, deck).in_use && !pending) {
                            destination = deck; break;
                        }
                    }
                    if (!trigger_sfx(destination, index)) return;
                } else if (!play_bgm(index)) return;
            }
            return;
        }
    }
    if (target >= 200 && target < 300) {
        const std::size_t deck = static_cast<std::size_t>((target - 200) / 10);
        if (deck < sfx_slot_count && audio_ready_) {
            Failure failure;
            static_cast<void>(engine_.stop(AudioBus::sfx, deck, 0.0, failure));
            dirty_ = true;
        }
        return;
    }
    POINT screen = client;
    ::ClientToScreen(g_panel_window->handle(), &screen);
    show_tray_menu(screen);
}

void Application::on_wheel(POINT client, int delta) {
    // 滚轮语义由悬停位置决定：在音量条上就调音量，否则滚曲库
    const int target = hit_.topmost_at(to_ui_point(client));
    const int steps = delta / WHEEL_DELTA;

    if (target == 103) {
        const int next = std::clamp(product_.bgm_volume_percent + steps * 4, 0,
                                       static_cast<int>(maximum_volume_percent));
        product_.bgm_volume_percent = static_cast<std::uint16_t>(next);
        apply_volume_changes();
        save_settings();
        dirty_ = true;
        return;
    }
    if (target >= 200 && target < 300 && (target - 200) % 10 == 4) {
        const std::size_t deck = static_cast<std::size_t>((target - 200) / 10);
        if (deck < sfx_slot_count) {
            const int next = std::clamp(product_.sfx_slots[deck].volume_percent + steps * 4, 0,
                                       static_cast<int>(maximum_volume_percent));
            product_.sfx_slots[deck].volume_percent = static_cast<std::uint16_t>(next);
            apply_volume_changes();
            save_settings();
            dirty_ = true;
        }
        return;
    }

    // 曲库滚动：滚光标下的那一列。
    // 光标不在任何一列上时滚左列 —— 至少有一个确定的默认，比"没反应"好。
    //
    // 这里改的是**目标偏移**，真正的位移由 update_hover_animations 里的 glide
    // 每帧追过去。直接改 offset 的话滚动是一格一格跳的，而且一次滚很多格时
    // 中间的卡片根本来不及画（看起来就是"没滚到底"）。
    // 滚哪一列由**光标在哪一列**决定（不是命中 id）——
    // 按 id 判断会让右列的空白处落到左列，看起来就是"两列连在一起"。
    Column& column = column_at_point(to_ui_point(client));
    // 起点用**当前目标**（连滚时是叠加的：连滚三下就该滚三下的距离，
    // 而不是每次都从"眼下的位置"重新算 —— 后者会让连滚变得非常慢）。
    float& scroll_target =
        (&column == &sfx_) ? sfx_scroll_target_ : bgm_scroll_target_;
    const float next = std::clamp(scroll_target + static_cast<float>(-steps) * 72.0f, 0.0f,
                                  column.scroll.maximum_offset());
    if (next != scroll_target) {
        scroll_target = next;
        dirty_ = true;
    }
}

void Application::on_char(wchar_t character) {
    if (!bgm_.search_focused && !sfx_.search_focused) {
        return;
    }
    Column& column = active_search_column();
    if (character == L'\b') {
        if (!column.search_text.empty()) {
            // 退格要按字符删，不能按字节 —— UTF-8 的汉字是三个字节
            std::size_t cut = column.search_text.size() - 1;
            while (cut > 0 && (static_cast<unsigned char>(column.search_text[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            column.search_text.resize(cut);
            set_search(column.search_text);
        }
        return;
    }
    if (character < 0x20 || character == 0x7F) {
        return;   // 控制字符不进搜索框
    }

    const std::wstring_view single(&character, 1);
    column.search_text += wide_to_utf8(single);
    set_search(column.search_text);
}

void Application::on_key_down(UINT virtual_key) {
    if (virtual_key == 'U' && (::GetKeyState(VK_CONTROL) & 0x8000) && capturing_slot_ < 0) {
        show_link_import();
        return;
    }
    if (loading_) { if (virtual_key == VK_ESCAPE) loader_.cancel(); return; }
    if (virtual_key == VK_ESCAPE && (dragging_ || seeking_ || slider_drag_target_ >= 0)) {
        cancel_pointer_gesture();
        set_status(tr("已取消拖动", "Drag cancelled"));
        return;
    }
    // 捕获态优先：此时按下的键是"新快捷键"，不是给界面的快捷键
    if (capturing_slot_ >= 0) {
        if (virtual_key == VK_ESCAPE) {
            cancel_hotkey_capture();
            return;
        }
        // 只按修饰键本身不算 —— 用户还在按住组合键的过程中
        switch (virtual_key) {
            case VK_CONTROL:
            case VK_SHIFT:
            case VK_MENU:
            case VK_LWIN:
            case VK_RWIN:
            case VK_LCONTROL:
            case VK_RCONTROL:
            case VK_LSHIFT:
            case VK_RSHIFT:
            case VK_LMENU:
            case VK_RMENU:
                return;
            default:
                break;
        }
        static_cast<void>(commit_hotkey_capture(virtual_key));
        return;
    }

    // 两列都可能正在搜索。只看左列的话，用户在右列搜索时按空格会开始播放，
    // 而搜索框里还有一个"空格"的期望落空。
    const bool searching = bgm_.search_focused || sfx_.search_focused;
    switch (virtual_key) {
        case VK_ESCAPE:
            // 一次 Esc 就够：清空并退出搜索。
            // 不做"第一次清空、第二次退出"的两段式 —— 搜索框里没有"未保存的工作"，
            // 而两段式在只有一个焦点环的情况下，用户看不出自己处在哪一段。
            bgm_.search_focused = false;
            sfx_.search_focused = false;
            if (!bgm_.search_text.empty()) {
                bgm_.search_text.clear();
                rebuild_visible(bgm_, false);
                dirty_ = true;
            }
            if (!sfx_.search_text.empty()) {
                sfx_.search_text.clear();
                rebuild_visible(sfx_, true);
                dirty_ = true;
            }
            break;
        case VK_SPACE:
            // 空格 = 暂停/继续（搜索框有焦点时不抢）
            if (!searching) {
                toggle_bgm_pause();
            }
            break;
        case VK_LEFT:
            if (!searching) {
                step_bgm(-1);
            }
            break;
        case VK_RIGHT:
            if (!searching) {
                step_bgm(1);
            }
            break;
        case VK_DELETE:
            if (!searching) clear_playback(false, true);
            break;
        default:
            break;
    }
}

bool Application::import_local_file(const std::filesystem::path& source,
                                    const std::string& category, bool as_sfx,
                                    std::string& problem) {
    problem.clear();

    // 分类名要经 path_from_utf8 转成路径。**不能**直接 library_root_ / category ——
    // 那会走活动代码页（AGENTS.md 第 2 条边界），中文分类名会被打乱。
    std::filesystem::path category_dir;
    Failure dir_failure;
    if (!path_from_utf8(category, category_dir, dir_failure)) {
        problem = tr("分类名不是合法 UTF-8", "Category name is not valid UTF-8");
        return false;
    }
    // 落到哪一栏由调用方说：拖到音效列里导入的就是音效。
    // 写死 "BGM" 会让拖进右列的文件跑到左列去，而用户看到的只是"没进来"。
    const std::filesystem::path target =
        library_root_ / (as_sfx ? L"SFX" : L"BGM") / category_dir;

    // 分类目录不存在就建一个。
    //
    // 原因很具体：扫描器会把**散放在根目录下的文件**
    // 归纳成一个叫「默认」的分类，但那个目录从来没有真的建过 ——
    // 于是选中「默认」拖进一个文件会报"目标分类目录不存在"。
    // 创建它不是静默兜底：这个分类本来就在库的列表里，
    // 用户选中它就是明确说了"放这儿"。
    {
        std::error_code mkdir_ec;
        std::filesystem::create_directories(target, mkdir_ec);
        if (mkdir_ec && !std::filesystem::is_directory(target)) {
            problem = tr("无法建立分类目录: ", "Could not create category folder: ") + path_to_utf8(target) + " —— " +
                      mkdir_ec.message();
            return false;
        }
    }

    // 一次导入 = 一个独立的暂存目录（与下载路线同一个约定），
    // 于是"暂存区里有什么"永远只对应一次导入。
    std::filesystem::path job_dir;
    Failure stage_failure;
    if (!create_staging_job_dir(default_staging_root(), job_dir, stage_failure)) {
        problem = stage_failure.to_string();
        return false;
    }

    // 拷进暂存区。**不能直接搬用户的原文件** —— 那是他的东西。
    const std::filesystem::path staged = job_dir / source.filename();
    std::error_code copy_ec;
    std::filesystem::copy_file(source, staged, std::filesystem::copy_options::none, copy_ec);
    if (copy_ec) {
        Failure cleanup;
        static_cast<void>(remove_staging_job_dir(job_dir, cleanup));
        problem = tr("复制失败: ", "Copy failed: ") + path_to_utf8(source) + " —— " + copy_ec.message();
        return false;
    }

    ImportRequest request;
    request.staged_file = staged;
    request.staging_root = job_dir;
    request.target_category_dir = target;
    // 名字沿用文件自己的主干：用户在别处整理好的命名不该被我们改掉
    request.preferred_stem.clear();

    ImportResult result;
    Failure import_failure;
    if (!import_staged_file(request, result, import_failure)) {
        problem = path_to_utf8(source.filename()) + " —— " + import_failure.context;
        // 失败时留着暂存目录当证据（与下载路线一致），下次启动会清理旧的
        return false;
    }

    // 直接插进内存里的曲库，**不重扫**：重扫一次是三秒，那会让界面卡死三秒。
    // 时长与容器是导入事务里刚探测出来的，可信。
    LibraryEntry entry;
    entry.track.name = result.final_stem;
    entry.label = ui::parse_track_label(entry.track.name);
    entry.track.path = result.installed_path;
    entry.track.duration_seconds = result.duration_seconds;
    AudioInfo info;
    Failure probe_failure;
    if (probe_audio_file(result.installed_path, info, probe_failure)) {
        entry.track.container = info.container;
        entry.track.format = info.format;
        bool cache_hit = false;
        std::string cache_note;
        if (!prepare_audio_cached(entry.track.path, info, product_data_root() / L"preload-v1",
                entry.prepared, cache_hit, cache_note, entry.preload_failure, nullptr, {})) {
            problem = tr("文件已导入，但播放准备失败: ", "File imported, but playback preparation failed: ") + entry.preload_failure.to_string();
        }
        if (!cache_note.empty()) report(LogLevel::warning, tr("导入预载", "Import preload"), cache_note);
    } else {
        entry.preload_failure = std::move(probe_failure);
        problem = tr("文件已导入，但格式探测失败: ", "File imported, but format probing failed: ") + entry.preload_failure.to_string();
    }
    Failure slots_failure;
    if (audio_ready_ && !engine_.ensure_sfx_slots(sfx_slot_count + entries_.size() + 1, slots_failure)) {
        problem = tr("文件已导入，但音效按钮准备失败: ", "File imported, but effect pad preparation failed: ") + slots_failure.to_string();
    }
    if (entry.prepared) entry.envelope = entry.prepared->envelope;
    entry.envelope_attempted = !entry.envelope.empty() || !entry.preload_failure.empty();
    entry.category = category;
    entry.is_sfx = as_sfx;
    entries_.push_back(std::move(entry));

    Column& column = as_sfx ? sfx_ : bgm_;
    if (std::find(column.categories.begin(), column.categories.end(), category) ==
        column.categories.end()) {
        column.categories.push_back(category);
    }

    Failure cleanup;
    static_cast<void>(remove_staging_job_dir(job_dir, cleanup));
    rebuild_visible(column, as_sfx);
    return problem.empty();
}

void Application::on_drop_files(HDROP drop) {
    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count == 0) {
        ::DragFinish(drop);
        return;
    }

    // 落到哪一栏：按光标最后所在的位置。
    // 拖到音效列里 = 我想加音效，这是唯一能表达这件事的手势。
    const bool as_sfx = section_of_id(hit_.topmost_at(to_ui_point(cursor_client_)));
    Column& column = as_sfx ? sfx_ : bgm_;

    // 落到哪个分类：当前选中的分类 → 第一个分类 → "默认"。
    // 不新建分类：拖进来这个动作没有表达"我想要一个新分类"。
    std::string category = std::string(default_category_name);
    if (column.active_chip >= 0 &&
        static_cast<std::size_t>(column.active_chip) < column.categories.size()) {
        category = column.categories[static_cast<std::size_t>(column.active_chip)];
    } else if (!column.categories.empty()) {
        category = column.categories.front();
    }

    std::size_t imported = 0;
    std::size_t skipped_inside = 0;
    std::string first_problem;

    for (UINT i = 0; i < count; ++i) {
        const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
        if (length == 0) {
            continue;
        }
        std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
        if (::DragQueryFileW(drop, i, buffer.data(), length + 1) == 0) {
            continue;
        }
        buffer.resize(length);
        const std::filesystem::path source(buffer);

        // 已经在曲库里的文件不动它：复制一遍会得到一份同名副本，
        // 而用户把它拖进来多半只是想播放它。
        if (path_is_inside(library_root_, source)) {
            ++skipped_inside;
            continue;
        }

        std::string problem;
        if (import_local_file(source, category, as_sfx, problem)) {
            ++imported;
        } else if (first_problem.empty()) {
            first_problem = problem;
        }
    }

    ::DragFinish(drop);
    rebuild_visible(column, as_sfx);

    std::string message = tr("已导入 ", "Imported ") + std::to_string(imported) + tr(" 个到", " files; ") +
                          (as_sfx ? tr("音效", "Effects") : tr("音乐", "Music")) + "「" + category + "」";
    if (skipped_inside > 0) {
        message += "，" + std::to_string(skipped_inside) + tr(" 个已在曲库里（跳过）", " already in library (skipped)");
    }
    if (!first_problem.empty()) {
        // 有失败就把第一条原因带出来。只报"导入 N 个"而把失败咽下去，
        // 用户会以为剩下的文件根本不存在。
        message += tr("；有失败: ", "; failures: ") + first_problem;
    }
    set_status(message);
}

// ---------------------------------------------------------------------------
// 通知区域
// ---------------------------------------------------------------------------

bool Application::create_tray_icon(Failure& failure) {
    if (g_panel_window == nullptr) {
        failure.set(fail_code::invalid_argument, tr("面板窗口还没建好", "Panel window is not ready"));
        return false;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_panel_window->handle();
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    // 图标按系统小图标尺寸生成，缩放过的小图标会糊
    data.hIcon = make_orb_icon(::GetSystemMetrics(SM_CXSMICON));
    if (data.hIcon == nullptr) {
        failure.set(fail_code::platform_api_failed, tr("生成通知区域图标失败", "Could not generate tray icon"));
        return false;
    }
    const wchar_t* tip = tr(L"音乐板", L"MusicBoard");
    static_cast<void>(::wcsncpy_s(data.szTip, tip, _TRUNCATE));

    if (::Shell_NotifyIconW(NIM_ADD, &data) == FALSE) {
        ::DestroyIcon(data.hIcon);
        failure.set(fail_code::platform_api_failed,
                    tr("Shell_NotifyIcon(NIM_ADD) 失败（Win32 错误 ", "Shell_NotifyIcon(NIM_ADD) failed (Win32 error ") +
                        std::to_string(::GetLastError()) + "）");
        return false;
    }
    tray_icon_ = data.hIcon;
    tray_ready_ = true;
    return true;
}

void Application::destroy_tray_icon() {
    if (!tray_ready_) {
        return;
    }
    if (g_panel_window != nullptr) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = g_panel_window->handle();
        data.uID = kTrayIconId;
        static_cast<void>(::Shell_NotifyIconW(NIM_DELETE, &data));
    }
    if (tray_icon_ != nullptr) {
        ::DestroyIcon(tray_icon_);
        tray_icon_ = nullptr;
    }
    tray_ready_ = false;
}

bool Application::create_tray_menu(HMENU& menu, Failure& failure) {
    menu = ::CreatePopupMenu();
    if (!menu) { failure.set("ui.menu", "CreatePopupMenu Win32=" + std::to_string(::GetLastError())); return false; }
    const auto add = [&](HMENU parent, UINT flags, UINT_PTR id, const wchar_t* label) {
        if (::AppendMenuW(parent, flags, id, label)) return true;
        failure.set("ui.menu", "AppendMenu Win32=" + std::to_string(::GetLastError())); return false;
    };
    const auto bgm = engine_.slot_state(AudioBus::bgm,0);
    const UINT available = !loading_ && bgm.in_use ? MF_STRING : MF_STRING | MF_GRAYED;
    bool ok = add(menu, available, 10, bgm.paused ? tr(L"继续播放", L"Resume") : tr(L"暂停", L"Pause")) &&
        add(menu, available, 11, tr(L"中止播放", L"Stop playback")) &&
        add(menu, MF_STRING, 20, tr(L"导入音乐链接…  Ctrl+U", L"Import music link…  Ctrl+U")) &&
        add(menu, MF_STRING, 21, tr(L"检查更新…", L"Check for updates…"));
    const HMENU recent = ::CreatePopupMenu();
    const HMENU shortcuts = ::CreatePopupMenu();
    if (!recent || !shortcuts) {
        failure.set("ui.menu", "Create submenu Win32=" + std::to_string(::GetLastError()));
        if (recent) ::DestroyMenu(recent); if (shortcuts) ::DestroyMenu(shortcuts);
        ::DestroyMenu(menu); menu=nullptr; return false;
    }
    recent_menu_indices_ = ranked_recent_bgm();
    if (recent_menu_indices_.empty()) ok = add(recent,MF_STRING|MF_GRAYED,0,tr(L"本次尚未播放 BGM", L"No BGM played this session")) && ok;
    for (std::size_t i=0;i<recent_menu_indices_.size();++i) {
        std::wstring label;
        if (!utf8_to_wide(entries_[recent_menu_indices_[i]].label.title, label, failure)) { ok=false; break; }
        // Literal ampersands in track titles must not become menu mnemonics.
        for (std::size_t pos=0;(pos=label.find(L'&',pos))!=std::wstring::npos;pos+=2) label.insert(pos,1,L'&');
        ok=add(recent,MF_STRING|(loading_?MF_GRAYED:0),1100+i,label.c_str()) && ok;
    }
    const bool attached_recent=add(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(recent),tr(L"播放最近", L"Recent tracks"));
    if (!attached_recent) ::DestroyMenu(recent);
    ok=attached_recent && ok;
    ok=add(shortcuts,MF_STRING|(product_.hotkeys.enabled?MF_CHECKED:0),1002,tr(L"启用全局快捷键", L"Enable global shortcuts")) && ok;
    for(int slot=0;slot<static_cast<int>(sfx_slot_count);++slot) {
        std::wstring label;
        if(!utf8_to_wide(hotkey_menu_label(slot),label,failure)){ok=false;break;}
        ok=add(shortcuts,MF_STRING,1003+slot,label.c_str()) && ok;
    }
    ok=add(shortcuts,MF_STRING,1007,tr(L"恢复默认", L"Restore defaults")) && ok;
    const bool attached_shortcuts=add(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(shortcuts),tr(L"快捷键", L"Shortcuts"));
    if(!attached_shortcuts)::DestroyMenu(shortcuts);
    ok=attached_shortcuts && ok;
    ok=add(menu,MF_STRING,1,panel_visible_?tr(L"隐藏面板", L"Hide panel"):tr(L"显示面板", L"Show panel")) && ok;
    ok=add(menu,MF_STRING,3,tr(L"退出程序", L"Exit")) && ok;
    if(!ok){::DestroyMenu(menu);menu=nullptr;} return ok;
}

void Application::show_tray_menu(POINT screen_point) {
    if (!g_panel_window) return;
    HMENU menu=nullptr;
    Failure failure;
    if (!create_tray_menu(menu,failure)) { set_status(tr("菜单创建失败: ", "Could not create menu: ") + failure.to_string()); return; }
    ::SetForegroundWindow(g_panel_window->handle());
    const UINT command=::TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,screen_point.x,screen_point.y,0,g_panel_window->handle(),nullptr);
    ::DestroyMenu(menu);
    if(command) ::SendMessageW(g_panel_window->handle(),WM_COMMAND,command,0);
}

// ---------------------------------------------------------------------------
// 设置
// ---------------------------------------------------------------------------

std::string Application::hotkey_menu_label(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(sfx_slot_count)) {
        return {};
    }
    const HotkeyChord& chord = product_.hotkeys.decks[static_cast<std::size_t>(slot)];
    const std::string name = tr("音效台 ", "Effect deck ") + std::to_string(slot + 1) + "：";
    if (chord.virtual_key == 0) {
        return name + tr("（未绑定）", " (unbound)");
    }
    return name + describe_hotkey(chord.modifiers, chord.virtual_key);
}

void Application::begin_hotkey_capture(int slot) {
    if (slot < 0 || slot >= static_cast<int>(sfx_slot_count)) {
        return;
    }
    capturing_slot_ = slot;
    // 先把自己的全局快捷键全撤掉：否则用户按下的那一组可能正好已经注册着，
    // 结果一边被捕获、一边把音效打响了。
    hotkeys_.unregister_all();
    set_status(tr("请按下「音效台 ", "Press a new shortcut for effect deck ") + std::to_string(slot + 1) + tr("」的新快捷键（至少带一个修饰键，Esc 取消）", " (include a modifier key; Esc cancels)"));
}

void Application::cancel_hotkey_capture() {
    if (capturing_slot_ < 0) {
        return;
    }
    capturing_slot_ = -1;
    re_register_hotkeys();
    set_status(tr("已取消改键", "Shortcut change cancelled"));
}

bool Application::commit_hotkey_capture(UINT virtual_key) {
    if (capturing_slot_ < 0) {
        return false;
    }
    const std::size_t slot = static_cast<std::size_t>(capturing_slot_);

    // 修饰键状态要从**按下那一刻**取：WM_KEYDOWN 不带修饰键信息
    UINT modifiers = MOD_NOREPEAT;
    if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        modifiers |= MOD_CONTROL;
    }
    if ((::GetKeyState(VK_MENU) & 0x8000) != 0) {
        modifiers |= MOD_ALT;
    }
    if ((::GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        modifiers |= MOD_SHIFT;
    }
    if ((::GetKeyState(VK_LWIN) & 0x8000) != 0 ||
        (::GetKeyState(VK_RWIN) & 0x8000) != 0) {
        modifiers |= MOD_WIN;
    }

    // 至少要有一个修饰键。一个裸字母当全局快捷键会把用户在任何程序里
    // 打的那个字吃掉 —— 这是个真实的危险，不是洁癖。
    if ((modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) == 0) {
        set_status(tr("这个组合没有修饰键，会抢掉别的程序里的按键。请按住 Ctrl / Alt / Shift / Win 再按一个键。", "Include a modifier to avoid taking normal keys from other apps. Hold Ctrl / Alt / Shift / Win and press a key."));
        return false;
    }

    product_.hotkeys.decks[slot].modifiers = modifiers;
    product_.hotkeys.decks[slot].virtual_key = virtual_key;
    capturing_slot_ = -1;
    save_settings();
    re_register_hotkeys();

    // 注册结果由 re_register_hotkeys 里的提示负责说明（可能被别的程序占用）
    return true;
}

void Application::re_register_hotkeys() {
    hotkeys_.unregister_all();
    const std::vector<HotkeyBinding> bindings = current_hotkey_bindings();
    for (const HotkeyBinding& binding : bindings) {
        if (binding.id >= 0 && binding.id < static_cast<int>(sfx_slot_count)) {
            hotkey_labels_[static_cast<std::size_t>(binding.id)] =
                describe_hotkey(binding.modifiers, binding.virtual_key);
        }
    }
    // 没绑定的台位清掉角标，免得界面上还显示着旧键
    for (std::size_t i = 0; i < sfx_slot_count; ++i) {
        if (product_.hotkeys.decks[i].virtual_key == 0) {
            hotkey_labels_[i].clear();
        }
    }
    state_.sfx_hotkeys = hotkey_labels_;
    dirty_ = true;

    if (!product_.hotkeys.enabled) {
        return;
    }
    if (bindings.empty()) {
        return;
    }

    std::vector<HotkeyRegistration> results;
    Failure failure;
    static_cast<void>(
        hotkeys_.register_all(g_panel_window->handle(), bindings, results, failure));
    std::string conflicts;
    for (const HotkeyRegistration& result : results) {
        if (!result.registered) {
            report_line_fmt(tr("快捷键未注册: %s\n", "Shortcut not registered: %s\n"), result.reason.c_str());
            if (!conflicts.empty()) {
                conflicts += "；";
            }
            conflicts += result.reason;
        }
    }
    if (!conflicts.empty()) {
        // 冲突必须说出来：用户以为设置好了，实际按下去没反应是最难查的问题之一
        set_status(tr("有快捷键没注册上：", "Some shortcuts could not be registered: ") + conflicts);
    }
}

std::vector<HotkeyBinding> Application::current_hotkey_bindings() const {
    std::vector<HotkeyBinding> bindings;
    bindings.reserve(sfx_slot_count);
    for (std::size_t i = 0; i < sfx_slot_count; ++i) {
        HotkeyBinding binding;
        binding.id = static_cast<int>(i);
        binding.modifiers = product_.hotkeys.decks[i].modifiers;
        binding.virtual_key = product_.hotkeys.decks[i].virtual_key;
        binding.description = tr("音效台 ", "Effect deck ") + std::to_string(i + 1);
        if (binding.virtual_key != 0) {
            bindings.push_back(std::move(binding));
        }
    }
    return bindings;
}

void Application::save_settings() {
    if (settings_path_.empty()) {
        return;
    }
    ProductSettingsStore store;
    Failure failure;
    if (!store.save_atomic(settings_path_, product_, failure)) {
        // 保存失败必须留痕：用户改了音量却没存上，下次启动又变回去，
        // 而他不知道为什么。
        report_line_fmt(tr("保存设置失败: %s\n", "Could not save settings: %s\n"), failure.to_string().c_str());
    }
}

}  // namespace musicboard::app
