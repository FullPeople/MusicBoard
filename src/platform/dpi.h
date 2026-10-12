#pragma once

#include <windows.h>

#include <cstdint>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// per-monitor DPI
//
// 设计约定（来自 design/DESIGN.md 的窗口一节）：
//
//   布局跟着缩放，绘制不被二次缩放。
//
// 意思是：所有逻辑尺寸都用 96 DPI 下的像素表达，画的时候乘以当前显示器的
// 缩放比；而 Direct2D 的 render target 必须把 DPI 固定成 96，
// 否则它会自己再缩一次，两套缩放叠加的结果是文字发虚、圆角错位。
//
// 这个"渲染 DPI 必须是 96"不是假设，是运行时要验证的不变量 —— 见
// verify_render_dpi_contract()。宁可发现后明确失败，也不要让它默默糊掉。
// ============================================================================

// 96 DPI 是 Windows 的基准，所有逻辑尺寸都相对它
inline constexpr UINT kBaseDpi = 96;

// 设计稿的画布尺寸（design/mockup.html 是按 1× 设计的）
inline constexpr int kPanelLogicalWidth = 1256;
inline constexpr int kPanelLogicalHeight = 788;

// 取窗口当前所在显示器的 DPI。
// 用 GetDpiForWindow 而不是 GetDpiForSystem：后者在多显示器混合缩放下是错的。
[[nodiscard]] UINT dpi_for_window(HWND window) noexcept;

[[nodiscard]] int scale_for_dpi(int logical, UINT dpi) noexcept;
[[nodiscard]] float scale_for_dpi(float logical, UINT dpi) noexcept;
[[nodiscard]] int unscale_for_dpi(int physical, UINT dpi) noexcept;

// 把进程设成 per-monitor v2 感知。必须在创建任何窗口之前调用。
[[nodiscard]] bool enable_per_monitor_dpi(Failure& failure);

// 运行时不变量检查：Direct2D render target 的 DPI 必须是 96。
// 不是 96 说明某处让 D2D 参与了缩放，布局会整体偏掉 —— 明确失败而不是将就。
[[nodiscard]] bool verify_render_dpi_contract(float render_dpi_x, float render_dpi_y, UINT window_dpi,
                                              Failure& failure);

}  // namespace musicboard
