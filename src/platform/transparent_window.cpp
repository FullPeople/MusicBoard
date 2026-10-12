#include "platform/transparent_window.h"

#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <d2d1_1helper.h>
#include <d3d11_1.h>
#include <shellscalingapi.h>
#include <wincodec.h>

#include <string>
#include <vector>

#include "core/path_utf8.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shcore.lib")

namespace musicboard {
namespace {

constexpr wchar_t kWindowTitleFallback[] = L"音乐板";

[[nodiscard]] std::string hresult_hex(HRESULT hr) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

}  // namespace

TransparentWindow::~TransparentWindow() {
    // 释放顺序有讲究：composition 相关要先于 D2D 设备释放，
    // 否则 surface 还会引用 d2d bitmap。
    discard_composition_surface();
    composition_visual_.Reset();
    composition_target_.Reset();
    composition_device_.Reset();
    d2d_context_.Reset();
    d2d_device_.Reset();
    d2d_factory_.Reset();
    dxgi_device_.Reset();
    d3d_device_.Reset();

    if (window_ != nullptr) {
        ::DestroyWindow(window_);
        window_ = nullptr;
    }
    // 用 create() 时抄下来的类名。**不能在这里调 window_class_name()** ——
    // 基类析构期间虚函数已经不指向子类，纯虚函数更没有定义，
    // 结果是链接期的一句"无法解析的外部符号"，看不出跟析构有关。
    if (registered_class_name_ != nullptr) {
        ::UnregisterClassW(registered_class_name_, ::GetModuleHandleW(nullptr));
    }
}

DrawContext TransparentWindow::draw_context() const noexcept {
    return make_draw_context();
}

DrawContext TransparentWindow::make_draw_context() const noexcept {
    DrawContext context;
    context.logical_width = static_cast<float>(options_.logical_width);
    context.logical_height = static_cast<float>(options_.logical_height);
    context.dpi = dpi_;
    return context;
}

RECT TransparentWindow::window_rect() const noexcept {
    RECT rect{};
    if (window_ != nullptr) {
        ::GetWindowRect(window_, &rect);
    }
    return rect;
}

void TransparentWindow::set_opacity(float opacity) {
    if (composition_visual_ == nullptr) {
        return;
    }
    // SetOpacity 在 IDCompositionVisual3 上（Visual 与 Visual2 都没有），
    // 所以先取一次那个接口并留着 —— 每帧 QI 一次是没必要的开销。
    // 取不到就安静地跳过：不透明度只是动效，不该因为它让面板出不来。
    if (composition_visual3_ == nullptr) {
        if (FAILED(composition_visual_.As(&composition_visual3_)) || composition_visual3_ == nullptr) {
            return;
        }
    }
    const float clamped = opacity < 0.0f ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
    if (FAILED(composition_visual3_->SetOpacity(clamped))) {
        return;
    }
    if (composition_device_ != nullptr) {
        static_cast<void>(composition_device_->Commit());
    }
}

bool TransparentWindow::create(const Options& options, RenderCallback callback, Failure& failure) {
    options_ = options;
    render_callback_ = std::move(callback);

    if (options_.logical_width <= 0 || options_.logical_height <= 0) {
        failure.set(fail_code::invalid_argument, "窗口的逻辑尺寸必须为正");
        return false;
    }

    if (!enable_per_monitor_dpi(failure)) {
        return false;
    }

    if (!create_device_resources(failure)) {
        return false;
    }

    const wchar_t* class_name = window_class_name();
    if (class_name == nullptr) {
        failure.set(fail_code::invalid_argument, "窗口类名为空");
        return false;
    }
    registered_class_name_ = class_name;

    // 注册窗口类。CS_HREDRAW/CS_VREDRAW 让尺寸变化时整窗重绘。
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = &TransparentWindow::window_proc_thunk;
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = ::LoadIconW(window_class.hInstance, MAKEINTRESOURCEW(1));
    window_class.hIconSm = ::LoadIconW(window_class.hInstance, MAKEINTRESOURCEW(1));
    window_class.lpszClassName = class_name;
    // 刻意不设 hbrBackground：窗口内容全部由 DirectComposition 提供，
    // 给背景刷反而会在启动瞬间闪一下白。

    if (::RegisterClassExW(&window_class) == 0) {
        const DWORD error = ::GetLastError();
        // 类已注册（重复初始化）不算失败
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            failure.set(fail_code::platform_api_failed,
                        "RegisterClassExW 失败，Win32 错误 " + std::to_string(error));
            return false;
        }
    }

    // 先按 96 DPI 建窗口，拿到 HWND 之后再问它的实际 DPI 并调整尺寸。
    // 反过来做的话，窗口在哪个显示器还没确定。
    UINT initial_dpi = kBaseDpi;
    {
        // 有初始位置就按那个位置找显示器，没有就按主显示器 ——
        // 悬浮球要落回上次那块屏上，这正是"位置持久化"能不能生效的前提。
        POINT probe{kBaseDpi, kBaseDpi};
        if (options_.initial_x != CW_USEDEFAULT && options_.initial_y != CW_USEDEFAULT) {
            probe.x = options_.initial_x;
            probe.y = options_.initial_y;
        }
        const HMONITOR monitor = ::MonitorFromPoint(probe, MONITOR_DEFAULTTOPRIMARY);
        UINT dpi_x = kBaseDpi, dpi_y = kBaseDpi;
        if (SUCCEEDED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) {
            initial_dpi = dpi_x;
        }
    }

    const int physical_width = scale_for_dpi(options_.logical_width, initial_dpi);
    const int physical_height = scale_for_dpi(options_.logical_height, initial_dpi);

    DWORD ex_style = window_ex_style() | extra_ex_style_ | WS_EX_NOREDIRECTIONBITMAP;
    if (options_.topmost) {
        ex_style |= WS_EX_TOPMOST;
    }
    if (options_.tool_window) {
        ex_style |= WS_EX_TOOLWINDOW;
    }
    if (options_.no_activate) {
        ex_style |= WS_EX_NOACTIVATE;
    }

    const std::wstring& title =
        options_.title.empty() ? std::wstring(kWindowTitleFallback) : options_.title;

    window_ = ::CreateWindowExW(
        ex_style, class_name, title.c_str(), window_style() | extra_style_,
        options_.initial_x, options_.initial_y, physical_width, physical_height,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), this);

    if (window_ == nullptr) {
        const DWORD error = ::GetLastError();
        failure.set(fail_code::platform_api_failed,
                    "CreateWindowExW 失败，Win32 错误 " + std::to_string(error));
        return false;
    }

    dpi_ = dpi_for_window(window_);
    physical_width_ = static_cast<UINT>(scale_for_dpi(options_.logical_width, dpi_));
    physical_height_ = static_cast<UINT>(scale_for_dpi(options_.logical_height, dpi_));

    if (!create_composition_target(failure)) {
        return false;
    }
    if (!resize_composition_surface(physical_width_, physical_height_, failure)) {
        return false;
    }
    on_size_changed(physical_width_, physical_height_);

    if (options_.initially_visible) ::ShowWindow(window_, options_.no_activate ? SW_SHOWNOACTIVATE : SW_SHOW);
    return true;
}

bool TransparentWindow::create_device_resources(Failure& failure) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL achieved{};
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                     static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                     d3d_device_.GetAddressOf(), &achieved, nullptr);
    if (FAILED(hr)) {
        // 没有硬件设备时退回 WARP —— 至少在虚拟机/远程桌面里能跑起来并明确知道自己在软件渲染
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                                 static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                 d3d_device_.GetAddressOf(), &achieved, nullptr);
        if (FAILED(hr)) {
            failure.set(fail_code::platform_api_failed,
                        "D3D11CreateDevice 失败（硬件与 WARP 都不行），hr=" + hresult_hex(hr));
            return false;
        }
    }

    hr = d3d_device_.As(&dxgi_device_);
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "取 IDXGIDevice 失败，hr=" + hresult_hex(hr));
        return false;
    }

    D2D1_FACTORY_OPTIONS factory_options{};
#if defined(_DEBUG)
    factory_options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    // 用 4 参数原始形式：这个 SDK 配置下 D2D1CreateFactory 只暴露收 void** 的重载，
    // 模板重载不在候选里。显式 void** 转换即可。
    hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                             &factory_options,
                             reinterpret_cast<void**>(d2d_factory_.GetAddressOf()));
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "D2D1CreateFactory 失败，hr=" + hresult_hex(hr));
        return false;
    }

    hr = d2d_factory_->CreateDevice(dxgi_device_.Get(), d2d_device_.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "创建 D2D 设备失败，hr=" + hresult_hex(hr));
        return false;
    }

    hr = d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                          d2d_context_.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "创建 D2D 设备上下文失败，hr=" + hresult_hex(hr));
        return false;
    }

    // 文本渲染质量：灰度抗锯齿在透明背景上比亚像素清晰
    // （亚像素抗锯齿假设背景不透明，叠在桌面上会出现彩边）
    d2d_context_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    d2d_context_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    return true;
}

bool TransparentWindow::create_composition_target(Failure& failure) {
    HRESULT hr =
        ::DCompositionCreateDevice(dxgi_device_.Get(), IID_PPV_ARGS(composition_device_.GetAddressOf()));
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed,
                    "DCompositionCreateDevice 失败，hr=" + hresult_hex(hr));
        return false;
    }

    hr = composition_device_->CreateTargetForHwnd(window_, TRUE, composition_target_.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed,
                    "CreateTargetForHwnd 失败，hr=" + hresult_hex(hr));
        return false;
    }

    hr = composition_device_->CreateVisual(composition_visual_.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "CreateVisual 失败，hr=" + hresult_hex(hr));
        return false;
    }

    hr = composition_target_->SetRoot(composition_visual_.Get());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "SetRoot 失败，hr=" + hresult_hex(hr));
        return false;
    }
    return true;
}

void TransparentWindow::discard_composition_surface() noexcept {
    if (d2d_context_) d2d_context_->SetTarget(nullptr);
    surface_bitmap_.Reset();
    composition_surface_.Reset();
}

bool TransparentWindow::resize_composition_surface(UINT physical_width, UINT physical_height,
                                                   Failure& failure) {
    if (physical_width == 0 || physical_height == 0) {
        failure.set(fail_code::invalid_argument, "合成表面尺寸不能为 0");
        return false;
    }

    discard_composition_surface();

    // 预乘 alpha：窗口要叠在桌面上，非预乘会让半透明边缘发白
    HRESULT hr = composition_device_->CreateSurface(
        physical_width, physical_height, DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED, composition_surface_.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed,
                    "CreateSurface 失败 " + std::to_string(physical_width) + "x" +
                        std::to_string(physical_height) + "，hr=" + hresult_hex(hr));
        return false;
    }

    return paint_composition_surface(failure);
}

bool TransparentWindow::paint_composition_surface(Failure& failure) {
    // BeginDraw can return a different backing atlas and offset on every frame.
    // Keep the composition surface attached; only reacquire its drawing target.
    d2d_context_->SetTarget(nullptr);
    surface_bitmap_.Reset();
    POINT offset{};
    Microsoft::WRL::ComPtr<IDXGISurface> dxgi_surface;
    // BeginDraw 收 void**，模板的 GetAddressOf() 不会隐式转换，显式转一下
    HRESULT hr = composition_surface_->BeginDraw(nullptr, __uuidof(IDXGISurface),
                                        reinterpret_cast<void**>(dxgi_surface.GetAddressOf()),
                                        &offset);
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "BeginDraw 失败，hr=" + hresult_hex(hr));
        return false;
    }

    const D2D1_BITMAP_PROPERTIES1 bitmap_properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    hr = d2d_context_->CreateBitmapFromDxgiSurface(dxgi_surface.Get(), &bitmap_properties,
                                                  surface_bitmap_.GetAddressOf());
    if (FAILED(hr)) {
        composition_surface_->EndDraw();
        failure.set(fail_code::platform_api_failed,
                    "CreateBitmapFromDxgiSurface 失败，hr=" + hresult_hex(hr));
        return false;
    }

    d2d_context_->SetTarget(surface_bitmap_.Get());
    // 关键：render target 的 DPI 固定 96，缩放由我们自己按 dpi_ 做。
    // 让 D2D 按显示器 DPI 自己缩的话，我们的乘法与它叠加，布局会整体偏掉。
    d2d_context_->SetDpi(static_cast<float>(kBaseDpi), static_cast<float>(kBaseDpi));
    // 逻辑 → 物理的缩放变换
    const float scale = static_cast<float>(dpi_) / static_cast<float>(kBaseDpi);
    d2d_context_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
        D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));

    d2d_context_->BeginDraw();
    d2d_context_->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f,
        static_cast<float>(physical_width_) / scale, static_cast<float>(physical_height_) / scale),
        D2D1_ANTIALIAS_MODE_ALIASED);
    d2d_context_->Clear(D2D1::ColorF(0, 0.0f));   // 全透明：这一句就是"透明窗口"本身
    if (render_callback_) {
        render_callback_(*d2d_context_.Get(), make_draw_context());
    }
    d2d_context_->PopAxisAlignedClip();
    hr = d2d_context_->EndDraw();
    if (FAILED(hr)) {
        composition_surface_->EndDraw();
        failure.set(fail_code::platform_api_failed, "D2D EndDraw 失败，hr=" + hresult_hex(hr));
        return false;
    }

    hr = composition_surface_->EndDraw();
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "合成 EndDraw 失败，hr=" + hresult_hex(hr));
        return false;
    }

    hr = composition_visual_->SetContent(composition_surface_.Get());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "SetContent 失败，hr=" + hresult_hex(hr));
        return false;
    }
    hr = composition_device_->Commit();
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "Commit 失败，hr=" + hresult_hex(hr));
        return false;
    }
    return true;
}

bool TransparentWindow::redraw_frame(Failure& failure) {
    if (window_ == nullptr || !composition_device_) {
        failure.set(fail_code::invalid_argument, "窗口尚未创建，无法重画");
        return false;
    }
    return paint_composition_surface(failure);
}

bool TransparentWindow::draw_into(ID2D1RenderTarget& render_target, Failure& failure) const {
    // 契约检查：render target 的 DPI 必须是 96
    float dpi_x = 0.0f, dpi_y = 0.0f;
    render_target.GetDpi(&dpi_x, &dpi_y);
    if (!verify_render_dpi_contract(dpi_x, dpi_y, dpi_, failure)) {
        return false;
    }

    if (!render_callback_) {
        failure.set(fail_code::invalid_argument, "没有设置绘制回调");
        return false;
    }

    render_target.BeginDraw();
    render_target.Clear(D2D1::ColorF(0, 0.0f));
    render_callback_(render_target, make_draw_context());
    const HRESULT hr = render_target.EndDraw();
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "EndDraw 失败，hr=" + hresult_hex(hr));
        return false;
    }
    return true;
}

bool TransparentWindow::save_frame_png(const std::filesystem::path& path, Failure& failure) const {
    if (!d2d_factory_ || !d2d_device_) {
        failure.set(fail_code::invalid_argument, "窗口尚未创建，无法渲染");
        return false;
    }

    // 用 WIC 位图 render target 跑同一份绘制回调。
    // 好处是"截图"与"屏幕"共用一份绘制代码，截图能代表真实观感。
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(wic.GetAddressOf()));
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "创建 WIC 工厂失败，hr=" + hresult_hex(hr));
        return false;
    }

    const UINT width = static_cast<UINT>(options_.logical_width);
    const UINT height = static_cast<UINT>(options_.logical_height);

    Microsoft::WRL::ComPtr<IWICBitmap> wic_bitmap;
    hr = wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                           wic_bitmap.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "创建 WIC 位图失败，hr=" + hresult_hex(hr));
        return false;
    }

    // 关键：D2D render target 的 DPI 同样固定 96，与窗口一致。
    // 这样截图与屏幕是同一套坐标，不会出现"截图和实际不一样"。
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(kBaseDpi), static_cast<float>(kBaseDpi));

    Microsoft::WRL::ComPtr<ID2D1RenderTarget> bitmap_target;
    hr = d2d_factory_->CreateWicBitmapRenderTarget(wic_bitmap.Get(), properties,
                                                  bitmap_target.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed,
                    "CreateWicBitmapRenderTarget 失败，hr=" + hresult_hex(hr));
        return false;
    }
    bitmap_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    bitmap_target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (!draw_into(*bitmap_target.Get(), failure)) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICStream> stream;
    hr = wic->CreateStream(stream.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "创建 WIC 流失败，hr=" + hresult_hex(hr));
        return false;
    }
    // WIC 的文件流也走宽字符路径 —— 这里不能出错，中文目录名很常见
    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        failure.set(fail_code::file_open_failed,
                    "无法写入 " + path_to_utf8(path) + "，hr=" + hresult_hex(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "创建 PNG 编码器失败，hr=" + hresult_hex(hr));
        return false;
    }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "初始化编码器失败，hr=" + hresult_hex(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> frame_properties;
    hr = encoder->CreateNewFrame(frame.GetAddressOf(), frame_properties.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "创建帧失败，hr=" + hresult_hex(hr));
        return false;
    }
    hr = frame->Initialize(frame_properties.Get());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "初始化帧失败，hr=" + hresult_hex(hr));
        return false;
    }
    hr = frame->SetSize(width, height);
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "设置帧尺寸失败，hr=" + hresult_hex(hr));
        return false;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "设置像素格式失败，hr=" + hresult_hex(hr));
        return false;
    }
    hr = frame->WriteSource(wic_bitmap.Get(), nullptr);
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "写入像素失败，hr=" + hresult_hex(hr));
        return false;
    }
    hr = frame->Commit();
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "提交帧失败，hr=" + hresult_hex(hr));
        return false;
    }
    hr = encoder->Commit();
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "提交编码失败，hr=" + hresult_hex(hr));
        return false;
    }
    return true;
}

LRESULT CALLBACK TransparentWindow::window_proc_thunk(HWND window, UINT message, WPARAM wparam,
                                                     LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create_struct = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        auto* self = static_cast<TransparentWindow*>(create_struct->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    }
    auto* self = reinterpret_cast<TransparentWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self == nullptr) {
        return ::DefWindowProcW(window, message, wparam, lparam);
    }
    return self->window_proc(message, wparam, lparam);
}

LRESULT TransparentWindow::window_proc(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCDESTROY) {
        const HWND previous = window_;
        ::SetWindowLongPtrW(previous, GWLP_USERDATA, 0);
        window_ = nullptr;
        return ::DefWindowProcW(previous, message, wparam, lparam);
    }
    // 子类先看：它们比基类更懂自己的窗口
    LRESULT subclass_result = 0;
    if (on_message(message, wparam, lparam, subclass_result)) {
        return subclass_result;
    }

    switch (message) {
        case WM_NCHITTEST: {
            const LRESULT base = ::DefWindowProcW(window_, message, wparam, lparam);
            if (base != HTCLIENT) {
                return base;   // 边框/系统按钮等区域交给系统
            }
            // lparam 是屏幕坐标（物理像素）
            POINT cursor{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ::ScreenToClient(window_, &cursor);
            return hit_test_client(cursor);
        }

        case WM_DPICHANGED: {
            // 跨显示器拖动时系统会给一个新 DPI 和建议矩形。
            // 按建议矩形调整窗口，然后按新 DPI 重建合成表面。
            dpi_ = HIWORD(wparam);
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            ::SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left, suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);

            RECT client{};
            ::GetClientRect(window_, &client);
            const UINT width = static_cast<UINT>(client.right - client.left);
            const UINT height = static_cast<UINT>(client.bottom - client.top);
            if (width > 0 && height > 0) {
                physical_width_ = width;
                physical_height_ = height;
                Failure failure;
                if (!resize_composition_surface(width, height, failure)) {
                    // DPI 切换失败不该让进程死掉，但必须留痕
                    ::OutputDebugStringA(
                        ("DPI 切换后重建合成表面失败: " + failure.to_string() + "\n").c_str());
                }
                on_size_changed(width, height);
            }
            return 0;
        }

        case WM_SIZE: {
            physical_width_ = LOWORD(lparam);
            physical_height_ = HIWORD(lparam);
            if (physical_width_ > 0 && physical_height_ > 0 && composition_surface_) {
                Failure failure;
                // 尺寸变化时重建失败不该让进程死掉（下一次 WM_SIZE 还会再试），
                // 但必须留痕，不能什么都不说。
                if (!resize_composition_surface(physical_width_, physical_height_, failure)) {
                    ::OutputDebugStringA(
                        ("WM_SIZE 重建合成表面失败: " + failure.to_string() + "\n").c_str());
                }
                on_size_changed(physical_width_, physical_height_);
            }
            return 0;
        }

        case WM_EXITSIZEMOVE:
            on_moved();
            return 0;

        case WM_ERASEBKGND:
            return 1;   // 内容由合成层提供，不擦背景，避免闪白

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(window_, message, wparam, lparam);
}

int TransparentWindow::run_message_loop() {
    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        const HWND root = ::GetAncestor(message.hwnd, GA_ROOT);
        if (root && ::GetPropW(root, L"MusicBoard.ModelessDialog") && ::IsDialogMessageW(root, &message)) continue;
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace musicboard
