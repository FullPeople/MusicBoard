#include "platform/dpi.h"

#include <cmath>
#include <string>

namespace musicboard {
namespace {

// 浮点比较的容差。DPI 参与乘除后会有小的尾差，
// 用精确相等去判 96 会误报。
constexpr float kDpiEpsilon = 0.01f;

[[nodiscard]] std::string format_float(float value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    return buffer;
}

}  // namespace

UINT dpi_for_window(HWND window) noexcept {
    if (window == nullptr) {
        return kBaseDpi;
    }
    // GetDpiForWindow 在 Win10 1607+ 可用。对不可用的老系统，
    // 退回系统 DPI —— 多显示器混合缩放会不准，但不会崩。
    const UINT dpi = ::GetDpiForWindow(window);
    return dpi == 0 ? kBaseDpi : dpi;
}

int scale_for_dpi(int logical, UINT dpi) noexcept {
    // MulDiv 自带四舍五入，比手写 (logical * dpi) / 96 的边缘取整更符合预期
    return ::MulDiv(logical, static_cast<int>(dpi), static_cast<int>(kBaseDpi));
}

float scale_for_dpi(float logical, UINT dpi) noexcept {
    return logical * static_cast<float>(dpi) / static_cast<float>(kBaseDpi);
}

int unscale_for_dpi(int physical, UINT dpi) noexcept {
    if (dpi == 0) {
        return physical;
    }
    return ::MulDiv(physical, static_cast<int>(kBaseDpi), static_cast<int>(dpi));
}

bool enable_per_monitor_dpi(Failure& failure) {
    // PER_MONITOR_AWARE_V2 比 V1 多处理非客户区与子窗口的缩放通知。
    // 必须在创建窗口之前设，之后再设对已创建的窗口无效。
    if (!::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        const DWORD error = ::GetLastError();
        // Already configured is only valid if it is actually PerMonitorV2.
        if (error == ERROR_ACCESS_DENIED && ::AreDpiAwarenessContextsEqual(
                ::GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return true;
        }
        failure.set(fail_code::platform_api_failed,
                    "SetProcessDpiAwarenessContext 未能启用 PerMonitorV2，Win32 错误 " + std::to_string(error) +
                    "，当前 awareness=" + std::to_string(static_cast<int>(::GetAwarenessFromDpiAwarenessContext(
                        ::GetThreadDpiAwarenessContext()))));
        return false;
    }
    return true;
}

bool verify_render_dpi_contract(float render_dpi_x, float render_dpi_y, UINT window_dpi,
                                Failure& failure) {
    const float expected = static_cast<float>(kBaseDpi);
    const bool x_ok = std::fabs(render_dpi_x - expected) < kDpiEpsilon;
    const bool y_ok = std::fabs(render_dpi_y - expected) < kDpiEpsilon;
    if (x_ok && y_ok) {
        return true;
    }
    // 这条信息要能直接指出问题：渲染 DPI 不是 96 说明 D2D 参与了缩放，
    // 于是我们自己的乘法缩放会与它叠加。
    failure.set(fail_code::platform_api_failed,
                "渲染 DPI 契约不符：期望 96x96，实际 " + format_float(render_dpi_x) + "x" +
                    format_float(render_dpi_y) + "，窗口 DPI " + std::to_string(window_dpi) +
                    "。说明某处让 Direct2D 自己参与了缩放，布局会整体偏掉。");
    return false;
}

}  // namespace musicboard
