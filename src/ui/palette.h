#pragma once

#include <d2d1.h>

namespace musicboard::ui {

// ============================================================================
// 设计稿的颜色令牌
//
// 与 design/mockup.html 的 CSS 变量一一对应。抽成共享头是因为现在有三处要画：
// 主面板、悬浮球、以及面板的离屏预览 —— 各留一份迟早会漂，
// 而"两处的琥珀色不一样"这种差异肉眼几乎看不出来。
// ============================================================================

inline const D2D1_COLOR_F kBgVoid = D2D1::ColorF(0x0B0C0F);
inline const D2D1_COLOR_F kWindowBgTop = D2D1::ColorF(0x181B21);
inline const D2D1_COLOR_F kWindowBgBottom = D2D1::ColorF(0x131519);
inline const D2D1_COLOR_F kSurface1 = D2D1::ColorF(0x1A1D24);
inline const D2D1_COLOR_F kSurface2 = D2D1::ColorF(0x20242C);
inline const D2D1_COLOR_F kSurface3 = D2D1::ColorF(0x272C36);
inline const D2D1_COLOR_F kHairline = D2D1::ColorF(0x2C313B);
inline const D2D1_COLOR_F kHairlineSoft = D2D1::ColorF(0x23272F);
inline const D2D1_COLOR_F kText1 = D2D1::ColorF(0xE9EBEF);
inline const D2D1_COLOR_F kText2 = D2D1::ColorF(0x9AA0AC);
inline const D2D1_COLOR_F kText3 = D2D1::ColorF(0x616874);
inline const D2D1_COLOR_F kAmber = D2D1::ColorF(0xF2A03D);
inline const D2D1_COLOR_F kTeal = D2D1::ColorF(0x3FD0C9);
inline const D2D1_COLOR_F kDanger = D2D1::ColorF(0xE5544B);
inline const D2D1_COLOR_F kWaveDim = D2D1::ColorF(0x2F3540);

// 窗口与卡片的圆角
inline constexpr float kWindowRadius = 16.0f;
inline constexpr float kCardRadius = 8.0f;
inline constexpr float kPanelRadius = 12.0f;
// 分类芯片的左右内边距（与原型里 .chip 的 padding 0 11px 一致）
inline constexpr float kChipPaddingX = 11.0f;
// 悬停时边框提亮到的颜色（原型 .track:hover 的 border-color:#3A414D）
inline const D2D1_COLOR_F kHoverBorder = D2D1::ColorF(0x3A414D);
// 唱片每转一圈的秒数（设计稿：6.5 秒/圈）
inline constexpr float kDiscSecondsPerTurn = 13.0f;

}  // namespace musicboard::ui
