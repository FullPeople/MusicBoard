// 阶段 2 spike：透明覆盖层 + 无边框 + 自绘标题栏 + DPI + 中文文本
//
// 这个程序不是产品，是**技术验证**。它按 design/mockup.html 的配色与排版画一版
// 完整的界面，用来确认四件事：
//
//   1. 无边框透明窗口能不能真的叠在桌面上（Godot 版栽在这里）
//   2. DirectWrite 排中文对不对（字体族、字重、灰度抗锯齿）
//   3. per-monitor DPI：布局跟着缩放、绘制不被二次缩放
//   4. 自绘标题栏的拖动是否走系统（HTCAPTION）
//
// 用法:
//   spike.exe                          显示窗口，跑消息循环
//   spike.exe --png <路径> [--exit]     把当前帧渲染成 PNG；带 --exit 则存完就退
//
// --png 走的是与屏幕完全相同的绘制回调，所以那张图就是屏幕上那一帧。
//
// 两个 Windows 上的坑，写在这里免得后人再踩：
//   · 成员变量不能叫 small —— rpcndr.h 把它定义成 char（16 位时代遗留），会被宏展开
//   · CreateSolidColorBrush 的 C++ 重载收引用，传 nullptr 编不过

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d2d1_1.h>
#include <dwrite.h>

#include <cmath>
#include <cstdint>
#include <iterator>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/path_utf8.h"
#include "app/panel_preview.h"
#include "platform/composition_window.h"
#include "platform/orb_window.h"
#include "ui/panel_layout.h"
#include "ui/panel_view.h"
#include "platform/dpi.h"

#pragma comment(lib, "dwrite.lib")

using namespace musicboard;
using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------
// 设计令牌。与 design/mockup.html 开头的 :root 一一对应 ——
// 原型与实现必须共用同一套数值，否则"看着像"就无从谈起。
// ---------------------------------------------------------------------------
inline const D2D1_COLOR_F kBgVoid    = D2D1::ColorF(0x0B0C0F);   // 设计稿的 --bg-void
inline const D2D1_COLOR_F kWindowBg  = D2D1::ColorF(0x14161B);
inline const D2D1_COLOR_F kSurface1  = D2D1::ColorF(0x1A1D24);
inline const D2D1_COLOR_F kSurface2  = D2D1::ColorF(0x20242C);
inline const D2D1_COLOR_F kSurface3  = D2D1::ColorF(0x272C36);
inline const D2D1_COLOR_F kHairline  = D2D1::ColorF(0x2C313B);
inline const D2D1_COLOR_F kText1     = D2D1::ColorF(0xE9EBEF);
inline const D2D1_COLOR_F kText2     = D2D1::ColorF(0x9AA0AC);
inline const D2D1_COLOR_F kText3     = D2D1::ColorF(0x616874);
inline const D2D1_COLOR_F kAmber     = D2D1::ColorF(0xF2A03D);
inline const D2D1_COLOR_F kTeal      = D2D1::ColorF(0x3FD0C9);
inline const D2D1_COLOR_F kAmberSoft = D2D1::ColorF(0xF2A03D, 0.14f);
inline const D2D1_COLOR_F kTealSoft  = D2D1::ColorF(0x3FD0C9, 0.14f);
inline const D2D1_COLOR_F kWaveDim   = D2D1::ColorF(0x2F3540);

inline constexpr float kWindowRadius = 16.0f;
inline constexpr float kPanelRadius  = 12.0f;
inline constexpr float kCardRadius   = 8.0f;
inline constexpr float kTitlebarHeight = 42.0f;
inline constexpr float kBusbarHeight   = 34.0f;

// 绘制时需要的持久资源。只建一次 —— 每帧重建 DirectWrite 对象是浪费。
struct Resources final {
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IDWriteTextFormat> title;      // 13.5px 600
    ComPtr<IDWriteTextFormat> section;    // 12.5px 600
    ComPtr<IDWriteTextFormat> track_name; // 18px 600
    ComPtr<IDWriteTextFormat> body;       // 12.5px 400
    // 注意：成员名不能用 small —— Windows 的 rpcndr.h 把 small 定义成 char（16 位时代遗留），
    // 写出来会被宏展开成 char，报错信息还完全看不出原因。实测被咬过一次。
    ComPtr<IDWriteTextFormat> caption;    // 10.5px 400
    ComPtr<IDWriteTextFormat> centered;   // 12.5px 400，水平居中（用于按钮文字）
    // 省略号裁剪符号：卡片宽度固定、长曲名要显示成 "…" 而不是被硬裁。
    // DirectWrite 的做法是给 text format 设 trimming + 一个 inline object。
    ComPtr<IDWriteInlineObject> ellipsis;
    bool ready = false;

    [[nodiscard]] bool initialize(Failure& failure) {
        if (ready) {
            return true;
        }
        HRESULT hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                           reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
        if (FAILED(hr)) {
            failure.set(fail_code::platform_api_failed, "DWriteCreateFactory 失败");
            return false;
        }

        // 中文必须显式给 CJK 字体族 + zh-CN locale。
        // 只用 "Segoe UI" 的话中文会走字体回退，字重与行高都不可控。
        const wchar_t* family = L"Microsoft YaHei UI";
        const wchar_t* locale = L"zh-CN";

        if (!create(title, family, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, locale, failure) ||
            !create(section, family, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, locale, failure) ||
            !create(track_name, family, 18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, locale, failure) ||
            !create(body, family, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, locale, failure) ||
            !create(caption, family, 10.5f, DWRITE_FONT_WEIGHT_NORMAL, locale, failure)) {
            return false;
        }

        if (!create(centered, family, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, locale, failure)) {
            return false;
        }
        centered->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

        hr = dwrite->CreateEllipsisTrimmingSign(centered.Get(), ellipsis.GetAddressOf());
        if (FAILED(hr) || !ellipsis) {
            failure.set(fail_code::platform_api_failed, "CreateEllipsisTrimmingSign 失败");
            return false;
        }
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        centered->SetTrimming(&trimming, ellipsis.Get());
        // 卡片上的曲名用同一个省略号规则：这是设计里"长曲名显示成 …"的实现手段
        body->SetTrimming(&trimming, ellipsis.Get());

        ready = true;
        return true;
    }

private:
    [[nodiscard]] bool create(ComPtr<IDWriteTextFormat>& out, const wchar_t* family, float size,
                              DWRITE_FONT_WEIGHT weight, const wchar_t* locale,
                              Failure& failure) {
        HRESULT hr = dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size, locale,
                                              out.GetAddressOf());
        if (FAILED(hr)) {
            failure.set(fail_code::platform_api_failed, "CreateTextFormat 失败");
            return false;
        }
        out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    }
};

// ---------------------------------------------------------------------------
// 绘制辅助
// ---------------------------------------------------------------------------

void fill_rounded(ID2D1RenderTarget& target, ID2D1SolidColorBrush& brush, const D2D1_RECT_F& rect,
                  float radius, const D2D1_COLOR_F& color) {
    brush.SetColor(color);
    target.FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), &brush);
}

void stroke_rounded(ID2D1RenderTarget& target, ID2D1SolidColorBrush& brush, const D2D1_RECT_F& rect,
                    float radius, const D2D1_COLOR_F& color, float width) {
    brush.SetColor(color);
    target.DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), &brush, width);
}

void draw_text(ID2D1RenderTarget& target, ID2D1SolidColorBrush& brush, IDWriteTextFormat& format,
               const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color) {
    brush.SetColor(color);
    target.DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), &format, rect, &brush,
                     D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
}

// 把 UTF-8 字面量转成宽字符。源码用 /utf-8 编译，所以这里的字面量就是 UTF-8。
std::wstring to_wide(const char* utf8_text) {
    std::wstring out;
    Failure failure;
    if (!utf8_to_wide(utf8_text, out, failure)) {
        return L"<编码错误>";
    }
    return out;
}

// ---------------------------------------------------------------------------
// 音波图：连续包络，不是一排矩形
//
// 与原型一致：上下对称闭合成一个填充形状，已播段用强调色、未播段压暗。
// 已播/未播的分界用 PushAxisAlignedClip 实现 —— 相当于 SVG 里的 clip-path。
// 这里刻意用真实几何而不是矩形堆叠：真界面里的音波图就是这个画法，
// spike 要验证的正是 D2D 路径填充能不能达到原型的观感。
// ---------------------------------------------------------------------------
[[nodiscard]] ComPtr<ID2D1PathGeometry> make_envelope(ID2D1RenderTarget& target,
                                                      const D2D1_RECT_F& area, int samples,
                                                      float frequency) {
    ComPtr<ID2D1Factory> factory;
    target.GetFactory(factory.GetAddressOf());
    if (!factory) {
        return nullptr;
    }

    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(factory->CreatePathGeometry(geometry.GetAddressOf())) || !geometry) {
        return nullptr;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.GetAddressOf())) || !sink) {
        return nullptr;
    }

    const float center_y = (area.top + area.bottom) * 0.5f;
    const float half_height = (area.bottom - area.top) * 0.5f;
    const float span = area.right - area.left;

    // 确定性伪随机：同一首每次画出来一样，切歌时波形不会跳。
    const auto hash01 = [](std::uint32_t x) {
        x ^= x >> 16; x *= 0x7feb352dU;
        x ^= x >> 15; x *= 0x846ca68bU;
        x ^= x >> 16;
        return static_cast<float>(x & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
    };
    const auto envelope_at = [&](int index) {
        const float t = static_cast<float>(index) / static_cast<float>(samples - 1);
        // 两端渐弱，像一段完整的曲子
        const float taper = 0.28f + 0.72f * std::sin(3.14159265f * t);
        // 相邻采样做加权平均，得到相关的起伏 —— 有连贯的峰谷，
        // 而不是一根根孤立的尖刺（那样看起来像噪声不像音频）
        float sum = 0.0f, weight_sum = 0.0f;
        for (int k = -2; k <= 2; ++k) {
            const int j = index + k;
            if (j < 0 || j >= samples) continue;
            const float w = 1.0f - static_cast<float>(std::abs(k)) / 3.0f;
            sum += hash01(static_cast<std::uint32_t>(j) * 2654435761u +
                          static_cast<std::uint32_t>(frequency * 1000.0f)) * w;
            weight_sum += w;
        }
        const float noisy = weight_sum > 0.0f ? sum / weight_sum : 0.5f;
        // 对比曲线：不加的话起伏都挤在中间，画出来是一条等高的带子
        const float contrasted = 0.06f + 0.94f * std::pow(noisy, 2.6f);
        return taper * contrasted;
    };

    sink->BeginFigure(D2D1::Point2F(area.left, center_y - envelope_at(0) * half_height),
                      D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 1; i < samples; ++i) {
        const float x = area.left + span * static_cast<float>(i) / static_cast<float>(samples - 1);
        sink->AddLine(D2D1::Point2F(x, center_y - envelope_at(i) * half_height));
    }
    for (int i = samples - 1; i >= 0; --i) {   // 下包络：镜像走回来
        const float x = area.left + span * static_cast<float>(i) / static_cast<float>(samples - 1);
        sink->AddLine(D2D1::Point2F(x, center_y + envelope_at(i) * half_height));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return geometry;
}

// played_ratio < 0 表示不画进度（例如悬停预览里的小波形）
void draw_waveform(ID2D1RenderTarget& target, ID2D1SolidColorBrush& brush, const D2D1_RECT_F& area,
                   int samples, float frequency, float played_ratio, const D2D1_COLOR_F& accent,
                   const D2D1_COLOR_F& dim) {
    ComPtr<ID2D1PathGeometry> geometry = make_envelope(target, area, samples, frequency);
    if (!geometry) {
        return;
    }

    if (played_ratio < 0.0f) {
        brush.SetColor(dim);
        target.FillGeometry(geometry.Get(), &brush);
        return;
    }

    const float split = area.left + (area.right - area.left) * played_ratio;

    // 先铺未播段的底色（整条）
    brush.SetColor(dim);
    target.FillGeometry(geometry.Get(), &brush);

    // 再在已播区间内用强调色重铺一次。
    // PushAxisAlignedClip 会把后续绘制裁进这个矩形，等价于原型的 clip-path: inset()。
    target.PushAxisAlignedClip(D2D1::RectF(area.left, area.top - 1.0f, split, area.bottom + 1.0f),
                               D2D1_ANTIALIAS_MODE_ALIASED);
    brush.SetColor(accent);
    target.FillGeometry(geometry.Get(), &brush);
    target.PopAxisAlignedClip();
}

// 自绘标题栏按钮的位置 —— 与 WM_NCHITTEST 的判定区域无关，纯视觉
void draw_titlebar(ID2D1RenderTarget& target, ID2D1SolidColorBrush& brush, Resources& resources,
                   float width) {
    // 应用标记：一枚琥珀色小圆
    brush.SetColor(kAmber);
    target.FillEllipse(D2D1::Ellipse(D2D1::Point2F(28.0f, kTitlebarHeight / 2.0f), 8.0f, 8.0f),
                       &brush);

    draw_text(target, brush, *resources.title.Get(), to_wide("音乐板"),
              D2D1::RectF(44.0f, 0.0f, 160.0f, kTitlebarHeight), kText1);

    // 状态芯片：正在播放 + 曲名（对应原型里的 .tb-chip）
    const D2D1_RECT_F chip = D2D1::RectF(170.0f, 10.0f, 660.0f, 32.0f);
    fill_rounded(target, brush, chip, 11.0f, kAmberSoft);
    stroke_rounded(target, brush, chip, 11.0f, D2D1::ColorF(0xF2A03D, 0.45f), 1.0f);
    brush.SetColor(kAmber);
    target.FillEllipse(D2D1::Ellipse(D2D1::Point2F(184.0f, 21.0f), 3.0f, 3.0f), &brush);
    draw_text(target, brush, *resources.body.Get(),
              to_wide("正在播放 · 01 - Curse of Strahd - Castle Ravenloft - Main Theme"),
              D2D1::RectF(194.0f, 10.0f, 652.0f, 32.0f), D2D1::ColorF(0xF2C089));

    // 右侧三个窗口按钮
    const wchar_t* glyphs[] = {L"—", L"□", L"✕"};
    for (int i = 0; i < 3; ++i) {
        const float x = width - 120.0f + static_cast<float>(i) * 36.0f;
        draw_text(target, brush, *resources.body.Get(), glyphs[i],
                  D2D1::RectF(x, 0.0f, x + 30.0f, kTitlebarHeight),
                  i == 2 ? D2D1::ColorF(0xE5544B) : kText2);
    }
}

// 曲库区：搜索框（含内嵌的悬停预览区）+ 分类芯片 + 卡片网格
void draw_library(ID2D1RenderTarget& target, ID2D1SolidColorBrush& brush, Resources& resources,
                  const D2D1_RECT_F& area, bool is_bgm, bool preview_active) {
    const D2D1_COLOR_F accent = is_bgm ? kAmber : kTeal;
    const D2D1_COLOR_F accent_soft = is_bgm ? kAmberSoft : kTealSoft;

    // ---- 搜索框：左边输入框，右边是悬停预览区，中间一条竖线 ----
    // 这与原型一致：两者是同一个 box，所以不悬停时输入框照常可用。
    const D2D1_RECT_F box = D2D1::RectF(area.left, area.top, area.right, area.top + 46.0f);
    fill_rounded(target, brush, box, kCardRadius, preview_active ? accent_soft : kSurface1);
    stroke_rounded(target, brush, box, kCardRadius, preview_active ? accent : kHairline, 1.0f);

    brush.SetColor(kText3);
    target.FillEllipse(D2D1::Ellipse(D2D1::Point2F(box.left + 18.0f, box.top + 23.0f), 6.0f, 6.0f),
                       &brush);
    draw_text(target, brush, *resources.body.Get(),
              to_wide(is_bgm ? "搜索音乐…" : "搜索音效…"),
              D2D1::RectF(box.left + 32.0f, box.top, box.left + 200.0f, box.bottom), kText3);

    const float divider = box.left + 214.0f;
    brush.SetColor(kHairline);
    target.DrawLine(D2D1::Point2F(divider, box.top + 10.0f),
                    D2D1::Point2F(divider, box.bottom - 10.0f), &brush, 1.0f);

    const D2D1_RECT_F preview_wave = D2D1::RectF(divider + 12.0f, box.top + 13.0f,
                                                 divider + 76.0f, box.bottom - 13.0f);
    // 预览区里的波形整条一个色，不表达进度 —— 它是"这首长什么样"，不是"播到哪"
    draw_waveform(target, brush, preview_wave, 48, 19.0f, -1.0f, accent,
                  preview_active ? accent : D2D1::ColorF(0x616874, 0.5f));

    draw_text(target, brush, *resources.body.Get(),
              to_wide(preview_active
                          ? (is_bgm ? "01 - Curse of Strahd - Castle Ravenloft - Main Theme"
                                    : "火球爆发")
                          : (is_bgm ? "悬停曲目卡查看完整曲名" : "悬停音效卡查看完整名称")),
              D2D1::RectF(divider + 88.0f, box.top, area.right - 12.0f, box.bottom),
              preview_active ? kText1 : kText3);

    // ---- 分类芯片 ----
    const wchar_t* chips[] = {L"全部", L"城镇", L"战斗", L"探索", L"情绪", L"环境"};
    float chip_x = area.left;
    for (int i = 0; i < 6; ++i) {
        const float chip_width = 34.0f + static_cast<float>(wcslen(chips[i])) * 11.0f;
        const D2D1_RECT_F chip = D2D1::RectF(chip_x, box.bottom + 9.0f, chip_x + chip_width,
                                             box.bottom + 35.0f);
        if (i == 0) {
            fill_rounded(target, brush, chip, 13.0f, accent_soft);
            stroke_rounded(target, brush, chip, 13.0f, accent, 1.0f);
        } else {
            fill_rounded(target, brush, chip, 13.0f, kSurface1);
            stroke_rounded(target, brush, chip, 13.0f, kHairline, 1.0f);
        }
        draw_text(target, brush, *resources.caption.Get(), chips[i],
                  D2D1::RectF(chip.left + 10.0f, chip.top, chip.right, chip.bottom),
                  i == 0 ? accent : kText2);
        chip_x = chip.right + 6.0f;
    }

    // ---- 卡片网格 ----
    const float grid_top = box.bottom + 44.0f;
    const float columns = is_bgm ? 4.0f : 2.0f;
    const float gap = 7.0f;
    const float card_width = (area.right - area.left - gap * (columns - 1.0f)) / columns;
    const float card_height = is_bgm ? 46.0f : 50.0f;

    const wchar_t* bgm_names[] = {L"酒馆的夜晚", L"节日集市", L"低阶战斗",
                                  L"01 - Curse of Strahd - Castle Raven…", L"史诗之战",
                                  L"幽暗地下世界", L"失落神殿废墟", L"迷雾森林",
                                  L"悲剧背叛", L"希望 城镇", L"告别", L"雨声"};
    const wchar_t* sfx_names[] = {L"剑击", L"重击落地", L"弓箭破空", L"火球爆发",
                                  L"治疗术", L"传送", L"开门", L"脚步"};

    const int total = is_bgm ? 12 : 8;
    const float available = area.bottom - grid_top;
    const int max_rows = static_cast<int>(available / (card_height + gap));
    for (int i = 0; i < total && i / static_cast<int>(columns) < max_rows; ++i) {
        const int column = i % static_cast<int>(columns);
        const int row = i / static_cast<int>(columns);
        const float x = area.left + static_cast<float>(column) * (card_width + gap);
        const float y = grid_top + static_cast<float>(row) * (card_height + gap);
        const D2D1_RECT_F card = D2D1::RectF(x, y, x + card_width, y + card_height);

        // 第一张卡片画成"正在播放"的样子，验证选中态的观感
        const bool playing = (i == (is_bgm ? 2 : 3));
        fill_rounded(target, brush, card, kCardRadius, playing ? accent_soft : kSurface1);
        stroke_rounded(target, brush, card, kCardRadius, playing ? accent : kHairline, 1.0f);

        // 格式徽标
        const D2D1_RECT_F badge = D2D1::RectF(card.left + 9.0f, card.top + 10.0f,
                                              card.left + 41.0f, card.top + 36.0f);
        fill_rounded(target, brush, badge, 5.0f, playing ? accent : kSurface3);
        draw_text(target, brush, *resources.caption.Get(), L"OGG",
                  D2D1::RectF(badge.left, badge.top, badge.right, badge.bottom),
                  playing ? D2D1::ColorF(0x141414) : kText2);

        // 曲名。body 格式在创建时就设了 NO_WRAP，超出卡片宽度的部分会被裁掉 ——
        // 完整曲名由上面的预览区承担，卡片这里不追求显示全。
        const std::wstring name = is_bgm ? bgm_names[i] : sfx_names[i];
        draw_text(target, brush, *resources.body.Get(), name,
                  D2D1::RectF(card.left + 48.0f, card.top + 8.0f, card.right - 8.0f, card.top + 28.0f),
                  kText1);
        draw_text(target, brush, *resources.caption.Get(),
                  is_bgm ? L"战斗 · 04:06" : L"魔法 · 3.0 秒",
                  D2D1::RectF(card.left + 48.0f, card.top + 26.0f, card.right - 8.0f,
                              card.top + 42.0f),
                  kText3);
    }
}

// ---------------------------------------------------------------------------
// 面板绘制：照着 design/mockup.html 的布局画一版
// 全部在逻辑坐标下画，缩放由 CompositionWindow 负责。
// ---------------------------------------------------------------------------
void draw_panel(ID2D1RenderTarget& target, const DrawContext& context, Resources& resources) {
    const float width = context.logical_width;
    const float height = context.logical_height;

    ComPtr<ID2D1SolidColorBrush> brush;
    // C++ 重载收的是 D2D1_BRUSH_PROPERTIES 的引用，不是指针，传 nullptr 不行
    const D2D1_BRUSH_PROPERTIES brush_properties = D2D1::BrushProperties();
    if (FAILED(target.CreateSolidColorBrush(kText1, brush_properties, brush.GetAddressOf())) ||
        !brush) {
        return;
    }

    // ---- 窗口底：圆角 + 1px 描边。圆角之外必须是透明的，这就是透明窗口的意义 ----
    const D2D1_RECT_F window_rect = D2D1::RectF(0.5f, 0.5f, width - 0.5f, height - 0.5f);
    fill_rounded(target, *brush.Get(), window_rect, kWindowRadius, kWindowBg);
    stroke_rounded(target, *brush.Get(), window_rect, kWindowRadius, D2D1::ColorF(0x30363F), 1.0f);

    draw_titlebar(target, *brush.Get(), resources, width);

    brush->SetColor(kHairline);
    target.DrawLine(D2D1::Point2F(1.0f, kTitlebarHeight), D2D1::Point2F(width - 1.0f, kTitlebarHeight),
                    brush.Get(), 1.0f);

    // ---- 主体两列 ----
    const float body_top = kTitlebarHeight + 14.0f;
    const float left_x = 16.0f;
    const float gutter = 16.0f;
    const float left_width = (width - left_x * 2.0f - gutter) * 0.59f;
    const float right_x = left_x + left_width + gutter;
    const float right_width = width - right_x - left_x;
    const float busbar_top = height - kBusbarHeight;

    // ---- 左列：BGM ----
    draw_text(target, *brush.Get(), *resources.section.Get(), to_wide("音乐"),
              D2D1::RectF(left_x, body_top, left_x + 60.0f, body_top + 20.0f), kAmber);
    draw_text(target, *brush.Get(), *resources.caption.Get(), L"BGM",
              D2D1::RectF(left_x + 44.0f, body_top, left_x + 90.0f, body_top + 20.0f), kText3);

    const D2D1_RECT_F deck = D2D1::RectF(left_x, body_top + 30.0f, left_x + left_width,
                                         body_top + 222.0f);
    fill_rounded(target, *brush.Get(), deck, kPanelRadius, kSurface1);
    stroke_rounded(target, *brush.Get(), deck, kPanelRadius, kHairline, 1.0f);

    // 唱片：唱盘 + 黑胶 + 沟槽 + 中心 + 唱臂
    {
        const D2D1_POINT_2F center = D2D1::Point2F(deck.left + 96.0f, deck.top + 96.0f);
        brush->SetColor(D2D1::ColorF(0x0F1115));
        target.FillEllipse(D2D1::Ellipse(center, 78.0f, 78.0f), brush.Get());   // 唱盘
        brush->SetColor(D2D1::ColorF(0x101216));
        target.FillEllipse(D2D1::Ellipse(center, 70.0f, 70.0f), brush.Get());   // 唱片

        for (int i = 1; i <= 14; ++i) {   // 沟槽
            const float radius = 20.0f + static_cast<float>(i) * 3.4f;
            brush->SetColor(D2D1::ColorF(0xFFFFFF, 0.05f));
            target.DrawEllipse(D2D1::Ellipse(center, radius, radius), brush.Get(), 1.0f);
        }

        // 中心：设计约定是"不镂空黑洞"，有封面时整盘是封面。
        // spike 里画成琥珀色中心，验证这个约定的观感。
        brush->SetColor(kAmber);
        target.FillEllipse(D2D1::Ellipse(center, 21.0f, 21.0f), brush.Get());

        // 唱臂：支点在唱片右上外侧，落针在内圈
        const D2D1_POINT_2F pivot = D2D1::Point2F(center.x + 66.0f, center.y - 82.0f);
        const D2D1_POINT_2F needle = D2D1::Point2F(center.x + 30.0f, center.y + 26.0f);
        brush->SetColor(D2D1::ColorF(0xB9BEC7));
        target.DrawLine(pivot, needle, brush.Get(), 2.5f);
        brush->SetColor(D2D1::ColorF(0x9BA1AA));
        target.FillEllipse(D2D1::Ellipse(pivot, 7.0f, 7.0f), brush.Get());
    }

    const float info_x = deck.left + 200.0f;
    const float info_right = deck.right - 16.0f;
    draw_text(target, *brush.Get(), *resources.track_name.Get(), to_wide("低阶战斗"),
              D2D1::RectF(info_x, deck.top + 10.0f, info_right, deck.top + 42.0f), kText1);
    draw_text(target, *brush.Get(), *resources.body.Get(), to_wide("战斗 · 来自音乐库"),
              D2D1::RectF(info_x, deck.top + 42.0f, info_right, deck.top + 62.0f), kText3);

    {   // 分类与格式标签
        const D2D1_RECT_F tag = D2D1::RectF(info_x, deck.top + 72.0f, info_x + 44.0f,
                                            deck.top + 93.0f);
        fill_rounded(target, *brush.Get(), tag, 5.0f, kAmberSoft);
        stroke_rounded(target, *brush.Get(), tag, 5.0f, D2D1::ColorF(0xF2A03D, 0.4f), 1.0f);
        draw_text(target, *brush.Get(), *resources.caption.Get(), to_wide("战斗"),
                  D2D1::RectF(tag.left + 8.0f, tag.top, tag.right, tag.bottom), kAmber);

        const D2D1_RECT_F tag2 = D2D1::RectF(info_x + 52.0f, deck.top + 72.0f, info_x + 96.0f,
                                             deck.top + 93.0f);
        fill_rounded(target, *brush.Get(), tag2, 5.0f, kSurface2);
        stroke_rounded(target, *brush.Get(), tag2, 5.0f, kHairline, 1.0f);
        draw_text(target, *brush.Get(), *resources.caption.Get(), L"OGG",
                  D2D1::RectF(tag2.left + 8.0f, tag2.top, tag2.right, tag2.bottom), kText2);
    }

    // 时间与音波图：时间在两端，波形在中间，三者同一行 —— 与原型一致
    {
        const float row_center = deck.top + 145.0f;
        draw_text(target, *brush.Get(), *resources.caption.Get(), L"00:22",
                  D2D1::RectF(info_x, row_center - 10.0f, info_x + 40.0f, row_center + 10.0f), kText3);
        draw_text(target, *brush.Get(), *resources.caption.Get(), L"07:08",
                  D2D1::RectF(info_right - 40.0f, row_center - 10.0f, info_right, row_center + 10.0f),
                  kText3);

        const D2D1_RECT_F wave = D2D1::RectF(info_x + 48.0f, row_center - 13.0f, info_right - 48.0f,
                                             row_center + 13.0f);
        draw_waveform(target, *brush.Get(), wave, 160, 26.0f, 0.55f, kAmber, kWaveDim);
    }

    // 上一首 / 下一首：铺满整行
    {
        const float row_top = deck.top + 166.0f;
        const float half = (info_right - info_x - 8.0f) * 0.5f;
        const wchar_t* labels[] = {L"◀  上一首", L"下一首  ▶"};
        for (int i = 0; i < 2; ++i) {
            const float x = info_x + static_cast<float>(i) * (half + 8.0f);
            const D2D1_RECT_F button = D2D1::RectF(x, row_top, x + half, row_top + 38.0f);
            fill_rounded(target, *brush.Get(), button, 9.0f, kSurface2);
            stroke_rounded(target, *brush.Get(), button, 9.0f, kHairline, 1.0f);
            draw_text(target, *brush.Get(), *resources.centered.Get(), labels[i],
                      D2D1::RectF(button.left, button.top, button.right, button.bottom), kText2);
        }
    }

    // ---- 右列：SFX 四台 ----
    draw_text(target, *brush.Get(), *resources.section.Get(), to_wide("音效"),
              D2D1::RectF(right_x, body_top, right_x + 60.0f, body_top + 20.0f), kTeal);
    draw_text(target, *brush.Get(), *resources.caption.Get(), L"SFX",
              D2D1::RectF(right_x + 44.0f, body_top, right_x + 90.0f, body_top + 20.0f), kText3);

    const float deck_w = (right_width - 12.0f) * 0.5f;
    const float deck_h = 132.0f;
    const wchar_t* sfx_names[] = {L"火球爆发", L"空闲", L"狼嚎", L"空闲"};
    for (int i = 0; i < 4; ++i) {
        const int column = i % 2;
        const int row = i / 2;
        const float x = right_x + static_cast<float>(column) * (deck_w + 12.0f);
        const float y = body_top + 30.0f + static_cast<float>(row) * (deck_h + 9.0f);
        const D2D1_RECT_F card = D2D1::RectF(x, y, x + deck_w, y + deck_h);

        const bool active = (i == 0 || i == 2);
        fill_rounded(target, *brush.Get(), card, kPanelRadius, kSurface1);
        stroke_rounded(target, *brush.Get(), card, kPanelRadius, active ? kTeal : kHairline, 1.0f);

        // 左侧竖排功能键（默认循环开、淡入关）
        const wchar_t* funcs[] = {L"循环", L"淡入"};
        for (int f = 0; f < 2; ++f) {
            const float fy = y + 12.0f + static_cast<float>(f) * 29.0f;
            const D2D1_RECT_F button = D2D1::RectF(x + 10.0f, fy, x + 48.0f, fy + 24.0f);
            const bool lit = (f == 0) && active;
            fill_rounded(target, *brush.Get(), button, 6.0f, lit ? kTealSoft : kSurface1);
            stroke_rounded(target, *brush.Get(), button, 6.0f, lit ? kTeal : kHairline, 1.0f);
            draw_text(target, *brush.Get(), *resources.caption.Get(), funcs[f],
                      D2D1::RectF(button.left, button.top, button.right, button.bottom),
                      lit ? kTeal : kText3);
        }

        // 唱片
        const D2D1_POINT_2F center = D2D1::Point2F(x + 96.0f, y + 50.0f);
        brush->SetColor(D2D1::ColorF(0x0F1115));
        target.FillEllipse(D2D1::Ellipse(center, 35.0f, 35.0f), brush.Get());
        brush->SetColor(active ? kTeal : D2D1::ColorF(0x22262D));
        target.FillEllipse(D2D1::Ellipse(center, 11.0f, 11.0f), brush.Get());

        draw_text(target, *brush.Get(), *resources.body.Get(), sfx_names[i],
                  D2D1::RectF(x + 140.0f, y + 6.0f, x + deck_w - 26.0f, y + 40.0f),
                  active ? kTeal : kText2);

        // 音波图：同样用连续包络
        const D2D1_RECT_F wave = D2D1::RectF(x + 10.0f, y + deck_h - 34.0f, x + deck_w - 24.0f,
                                             y + deck_h - 12.0f);
        draw_waveform(target, *brush.Get(), wave, 80, 17.0f, active ? 0.4f : -1.0f, kTeal, kWaveDim);

        // 最右侧竖向音量条
        const float vol_x = x + deck_w - 13.0f;
        brush->SetColor(kSurface3);
        target.FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(vol_x, y + 14.0f, vol_x + 5.0f, y + deck_h - 14.0f), 2.5f,
                              2.5f),
            brush.Get());
        brush->SetColor(active ? kTeal : D2D1::ColorF(0x3A414D));
        target.FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(vol_x, y + deck_h * 0.45f, vol_x + 5.0f, y + deck_h - 14.0f), 2.5f, 2.5f),
            brush.Get());
    }

    // ---- 曲库 ----
    draw_library(target, *brush.Get(), resources,
                 D2D1::RectF(left_x, deck.bottom + 14.0f, deck.right, busbar_top - 6.0f), true, true);
    draw_library(target, *brush.Get(), resources,
                 D2D1::RectF(right_x, body_top + 30.0f + (deck_h + 9.0f) * 2.0f + 14.0f,
                             right_x + right_width, busbar_top - 6.0f),
                 false, false);

    // ---- 底部总线状态条 ----
    {
        brush->SetColor(D2D1::ColorF(0x000000, 0.22f));
        target.FillRectangle(D2D1::RectF(1.0f, busbar_top, width - 1.0f, height - 1.0f), brush.Get());
        brush->SetColor(kHairline);
        target.DrawLine(D2D1::Point2F(1.0f, busbar_top), D2D1::Point2F(width - 1.0f, busbar_top),
                        brush.Get(), 1.0f);

        const float center_y = busbar_top + kBusbarHeight * 0.5f;
        brush->SetColor(kAmber);
        target.FillEllipse(D2D1::Ellipse(D2D1::Point2F(20.0f, center_y), 3.0f, 3.0f), brush.Get());
        draw_text(target, *brush.Get(), *resources.caption.Get(), to_wide("BGM 总线  低阶战斗"),
                  D2D1::RectF(30.0f, busbar_top, 300.0f, height), kText2);

        brush->SetColor(kTeal);
        target.FillEllipse(D2D1::Ellipse(D2D1::Point2F(250.0f, center_y), 3.0f, 3.0f), brush.Get());
        draw_text(target, *brush.Get(), *resources.caption.Get(), to_wide("SFX 总线  2 个音效在响"),
                  D2D1::RectF(260.0f, busbar_top, 540.0f, height), kText2);

        const char path_chars[] = {'D', ':', static_cast<char>(92), 'M', 'u', 's', 'i', 'c',
                                   static_cast<char>(92), 'D', 'n', 'D', '\0'};
        draw_text(target, *brush.Get(), *resources.caption.Get(),
                  to_wide((std::string("曲库 ") + path_chars).c_str()),
                  D2D1::RectF(width - 300.0f, busbar_top, width - 18.0f, height), kText3);
    }
}

}  // namespace

// ===========================================================================
// 悬浮球（阶段 3.5）
//
// 这一段的目的不是把球画完，而是把**球这个窗口**验证清楚：
// 圆形命中测试、不进任务栏、点击不抢焦点、拖动与点击分得开、位置记得住。
// 观感（封面、辉光跟着音频起伏）留给阶段 6，这里只画到"能被眼睛认出来是颗球"。
// ===========================================================================
namespace {

// 球面的可视状态。接口刻意做成"喂数据进来"，阶段 6 直接接解码出来的 PCM 包络。
struct OrbVisual final {
    float progress = 0.42f;      // 0..1 播放进度
    float rotation_radians = 0.0f;
    float amplitude = 0.0f;      // 当前音频能量 0..1，驱动辉光
    bool playing = true;
    D2D1_COLOR_F bus_color = kAmber;
};

// 悬浮球 spike 的入口（实现在本文件后半部分）
int run_orb_spike(const std::filesystem::path& png_path, bool self_test, bool exit_after,
                  bool paused);

// 面板预览的入口（实现在本文件后半部分）
int run_panel_spike(const std::filesystem::path& library_root,
                    const std::filesystem::path& png_path, bool exit_after);

void draw_orb(ID2D1RenderTarget& target, const DrawContext& context, const OrbVisual& visual) {
    const float center_x = context.logical_width * 0.5f;
    const float center_y = context.logical_height * 0.5f;
    const float ring_radius = static_cast<float>(kOrbRingOuterDiameter) * 0.5f - kOrbRingWidth * 0.5f;
    const float disc_radius = static_cast<float>(kOrbLogicalDiameter) * 0.5f;

    // 暂停时整体压暗。设计稿要求"已暂停"与"空闲"必须一眼分得开，
    // 所以这里不是纯粹装饰 —— 它是两种状态之间最主要的区别。
    //
    // 一条在透明窗口上必须想清楚的事：**"压暗"不等于"降低 alpha"**。
    // 降低 alpha 会让盘面变得半透明 —— 那看起来像破了个洞，而不是变暗，
    // 在浅色桌面上尤其明显。所以这里是**把颜色往背景的暗色上混**，alpha 保持 1。
    const float dim = visual.playing ? 0.0f : 1.0f;
    const auto toward_dark = [dim](const D2D1_COLOR_F& color,
                                   float amount) -> D2D1_COLOR_F {
        const float t = amount * dim;
        return D2D1::ColorF(color.r * (1.0f - t) + kBgVoid.r * t,
                            color.g * (1.0f - t) + kBgVoid.g * t,
                            color.b * (1.0f - t) + kBgVoid.b * t, color.a);
    };

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target.CreateSolidColorBrush(D2D1::ColorF(0x000000, 1.0f), D2D1::BrushProperties(),
                                            brush.GetAddressOf()))) {
        return;
    }

    // ---- 辉光：跟着音频能量起伏 ----
    // 用的是真实幅度值（阶段 6 接 PCM 包络），不是凭空造的 BPM 动画。
    if (visual.amplitude > 0.001f) {
        constexpr int kLayers = 14;
        for (int layer = kLayers; layer >= 1; --layer) {
            const float t = static_cast<float>(layer) / static_cast<float>(kLayers);
            const float radius = ring_radius + t * (static_cast<float>(kOrbGlowMargin) - 2.0f);
            // 暂停时辉光收到 0.68（设计稿说的"整体压暗"在这个元素上的体现）
            const float alpha =
                visual.amplitude * (1.0f - t) * (1.0f - t) * 0.30f * (1.0f - 0.32f * dim);
            brush->SetColor(D2D1::ColorF(visual.bus_color.r, visual.bus_color.g,
                                         visual.bus_color.b, alpha));
            target.FillEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), radius, radius),
                               brush.Get());
        }
    }


    // ---- 进度环底 ----
    brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f - 0.04f * dim));
    target.DrawEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), ring_radius, ring_radius),
                       brush.Get(), kOrbRingWidth);


    // ---- 盘面 ----
    brush->SetColor(toward_dark(kSurface1, 0.55f));
    target.FillEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), disc_radius, disc_radius),
                       brush.Get());


    // ---- 沟槽 + 一条跟着转的高光，让"在转"这件事看得见 ----
    // 纯同心沟槽是旋转对称的，转起来看不出动静；高光才是转速的可视证据。
    {
        constexpr int kGrooves = 9;
        for (int i = 1; i <= kGrooves; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kGrooves + 1);
            const float radius = disc_radius * (0.36f + t * 0.60f);
            brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.045f - 0.015f * dim));
            target.DrawEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), radius, radius),
                               brush.Get(), 1.0f);
        }

        // 高光要“看得见在转”，但不能变成一块灰斑。
        // 半径 0.60 处、细长、alpha 0.03 ——
        // 试过 0.30/0.14/0.055，那看起来像一块污渍而不是反光。
        const D2D1_POINT_2F highlight_center =
            D2D1::Point2F(center_x + std::cos(visual.rotation_radians) * disc_radius * 0.60f,
                          center_y + std::sin(visual.rotation_radians) * disc_radius * 0.60f);
        brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.030f - 0.012f * dim));
        target.FillEllipse(
            D2D1::Ellipse(highlight_center, disc_radius * 0.20f, disc_radius * 0.055f), brush.Get());
    }


    // ---- 中心 logo ----
    // 设计稿明确要求球心放 logo、**不镂空**（球只有 104px，镂空一个洞更像破了个口）。
    {
        const float logo_radius = disc_radius * 0.30f;
        // 播放中 → 总线色；已暂停/空闲 → 转灰。这是"再点一下会不会继续"的唯一线索，
        // 不能只靠一点透明度差别来表达。
        brush->SetColor(visual.playing
                            ? D2D1::ColorF(visual.bus_color.r, visual.bus_color.g,
                                           visual.bus_color.b, 0.92f)
                            : D2D1::ColorF(kText3.r, kText3.g, kText3.b, 0.85f));
        target.FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(center_x, center_y), logo_radius, logo_radius), brush.Get());
        // 一小段内环当标记，不用文字：104px 的球上写字只会糊成一团
        brush->SetColor(D2D1::ColorF(kWindowBg.r, kWindowBg.g, kWindowBg.b, 0.85f));
        target.DrawEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), logo_radius * 0.52f,
                                        logo_radius * 0.52f),
                           brush.Get(), logo_radius * 0.30f);
    }

    // ---- 进度弧：从 12 点方向顺时针 ----
    if (visual.progress > 0.0f) {
        // 几何与描边样式都必须取自 **render target 自己的工厂**。
        // 另建一个 ID2D1Factory 去 CreateStrokeStyle，再把它的样式传给这个
        // render target，D2D 不会给你 HRESULT —— 它直接 abort，现场一句解释都没有。
        // 实测被咬过一次：所有 trace 都打了出来，唯独带描边样式的那次绘制之后再无输出。
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        target.GetFactory(factory.GetAddressOf());

        Microsoft::WRL::ComPtr<ID2D1PathGeometry> arc;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (factory && SUCCEEDED(factory->CreatePathGeometry(arc.GetAddressOf())) &&
            SUCCEEDED(arc->Open(sink.GetAddressOf()))) {
            const float sweep = visual.progress * 6.2831853f;
            const float start_angle = -1.5707963f;   // 12 点
            const D2D1_POINT_2F start =
                D2D1::Point2F(center_x + std::cos(start_angle) * ring_radius,
                              center_y + std::sin(start_angle) * ring_radius);
            sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
            const D2D1_ARC_SEGMENT segment = D2D1::ArcSegment(
                D2D1::Point2F(center_x + std::cos(start_angle + sweep) * ring_radius,
                              center_y + std::sin(start_angle + sweep) * ring_radius),
                D2D1::SizeF(ring_radius, ring_radius), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                sweep > 3.1415927f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL);
            sink->AddArc(segment);
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            // 几何在 sink 释放之后才能被使用（Open 的文档里写明）。
            // 和上面那条一样：违反了不会给 HRESULT，只会 abort。
            sink.Reset();

            Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_caps;
            if (SUCCEEDED(factory->CreateStrokeStyle(
                    D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_ROUND,
                                                D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND,
                                                10.0f),
                    nullptr, 0, round_caps.GetAddressOf()))) {
                const D2D1_COLOR_F arc_color = toward_dark(visual.bus_color, 0.45f);
                brush->SetColor(D2D1::ColorF(arc_color.r, arc_color.g, arc_color.b,
                                             visual.playing ? 1.0f : 0.75f));
                target.DrawGeometry(arc.Get(), brush.Get(), kOrbRingWidth, round_caps.Get());
            }
        }
    }
}

// 悬浮球自检：直接问窗口"这个点算谁的"，比模拟鼠标点桌面可靠得多。
// 阶段 2 的标题栏自检用的是同一个办法。
int run_orb_self_test(OrbWindow& orb) {
    int total = 0;
    int failures = 0;
    const auto check = [&total, &failures](bool ok, const std::string& label) {
        ++total;
        if (!ok) {
            ++failures;
        }
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str());
    };

    // 球心在窗口正中（184 逻辑像素 → 96 DPI 下 92,92），命中半径是进度环外半径 62。
    //
    // 坐标口径很要紧：WM_NCHITTEST 收的是**屏幕坐标**，窗口自己会 ScreenToClient。
    // 直接把客户区坐标当屏幕坐标发过去是错的 —— 屏幕原点在左上角、而窗口不在原点，
    // 那样测的每个点都不是你以为的那个点（而它还可能"看起来通过了"）。
    POINT origin{0, 0};
    ::ClientToScreen(orb.handle(), &origin);

    struct Case final {
        int client_x;
        int client_y;
        LRESULT expected;
        const char* label;
    };
    const Case cases[] = {
        {92, 92, HTCLIENT, "球心 → HTCLIENT"},
        {92, 40, HTCLIENT, "盘面内 → HTCLIENT"},
        {153, 92, HTCLIENT, "进度环内侧 → HTCLIENT（环也是球的一部分）"},
        {154, 92, HTCLIENT, "进度环外沿 → HTCLIENT（边界算命中）"},
        {155, 92, HTTRANSPARENT, "环外 1px → HTTRANSPARENT（让路给桌面）"},
        {5, 5, HTTRANSPARENT, "窗口左上角 → HTTRANSPARENT"},
        {179, 5, HTTRANSPARENT, "窗口右上角 → HTTRANSPARENT"},
        {5, 179, HTTRANSPARENT, "窗口左下角 → HTTRANSPARENT"},
        {179, 179, HTTRANSPARENT, "窗口右下角 → HTTRANSPARENT"},
        {92, 179, HTTRANSPARENT, "正下方环外 → HTTRANSPARENT"},
    };
    for (const Case& item : cases) {
        const int screen_x = origin.x + scale_for_dpi(item.client_x, orb.dpi());
        const int screen_y = origin.y + scale_for_dpi(item.client_y, orb.dpi());
        const LRESULT actual = ::SendMessageW(orb.handle(), WM_NCHITTEST, 0,
                                              MAKELPARAM(static_cast<short>(screen_x),
                                                         static_cast<short>(screen_y)));
        check(actual == item.expected,
              std::string(item.label) + "  (客户区 " + std::to_string(item.client_x) + "," +
                  std::to_string(item.client_y) + " -> " +
                  std::to_string(static_cast<long>(actual)) + "，期望 " +
                  std::to_string(static_cast<long>(item.expected)) + ")");
    }

    // ---- 窗口样式：这几个决定"球是不是一个安分的常驻窗口" ----
    {
        const LONG_PTR ex_style = ::GetWindowLongPtrW(orb.handle(), GWL_EXSTYLE);
        const LONG_PTR style = ::GetWindowLongPtrW(orb.handle(), GWL_STYLE);
        check((ex_style & WS_EX_TOOLWINDOW) != 0,
              "WS_EX_TOOLWINDOW 已设（不进任务栏与 Alt+Tab）");
        check((ex_style & WS_EX_NOACTIVATE) != 0, "WS_EX_NOACTIVATE 已设（点击不抢用户的焦点）");
        check((ex_style & WS_EX_TOPMOST) != 0, "WS_EX_TOPMOST 已设（常驻在最上面）");
        check((ex_style & WS_EX_NOREDIRECTIONBITMAP) != 0,
              "WS_EX_NOREDIRECTIONBITMAP 已设（逐像素透明的必要条件）");
        check((ex_style & WS_EX_APPWINDOW) == 0, "没有 WS_EX_APPWINDOW");
        check((style & WS_POPUP) != 0, "WS_POPUP（没有系统标题栏）");
        check((style & WS_CAPTION) == 0, "没有 WS_CAPTION");
    }

    // ---- 尺寸：球心与命中半径都是从物理尺寸算出来的 ----
    {
        RECT client{};
        ::GetClientRect(orb.handle(), &client);
        check(client.right - client.left == orb.physical_width() &&
                  client.bottom - client.top == orb.physical_height(),
              "客户区尺寸 == 按当前 DPI 算出的物理尺寸");
        check(orb.physical_width() == scale_for_dpi(kOrbWindowLogicalSize, orb.dpi()),
              "物理宽度 == 逻辑尺寸 × DPI 缩放");
    }

    std::printf("---- 悬浮球自检: %d/%d 通过 ----\n", total - failures, total);
    return failures == 0 ? 0 : 1;
}

// 面板预览：扫真实曲库 → 建一个主面板窗口 → 画一帧 → 出图。
//
// 只画一帧，不起消息循环（交互与播放是正式程序的事）。
// 它的价值在于：绘制层对着**真素材**（真实中文曲名、真实封面、真实时长）
// 渲染一次，让人能看见结果，而不是只有布局断言。
int run_panel_spike(const std::filesystem::path& library_root,
                    const std::filesystem::path& png_path, bool exit_after) {
    // 不硬编码默认曲库路径：一是这机器上有什么不该写进代码，
    // 二是用窄字符串去构造 std::filesystem::path 会走活动代码页 ——
    // 中文目录名会被打乱（AGENTS.md 第 2 条边界）。要路径就显式给。
    if (library_root.empty()) {
        std::printf("面板预览需要 --library <曲库目录>\n");
        return 1;
    }
    const std::filesystem::path root = library_root;

    ui::PanelState state;
    Failure build_failure;
    if (!app::build_preview_state(root, state, build_failure)) {
        std::printf("构建预览状态失败: %s\n", build_failure.to_string().c_str());
        return 1;
    }

    ui::PanelMetrics metrics;
    const ui::PanelLayout layout = ui::compute_panel_layout(metrics);
    ui::PanelView view;

    CompositionWindow window;
    CompositionWindow::Options options;
    options.window.title = L"音乐板";
    options.window.logical_width = static_cast<int>(metrics.width);
    options.window.logical_height = static_cast<int>(metrics.height);
    options.titlebar_logical_height = static_cast<int>(metrics.titlebar_height);

    bool view_ready = false;
    Failure failure;
    const bool created = window.create(
        options,
        [&view, &layout, &state, &view_ready](ID2D1RenderTarget& target,
                                              const DrawContext& context) {
            if (!view_ready) {
                // 窗口第一次绘制时才拿得到设备上下文，所以初始化放在这里。
                // 拿不到上下文不该让整帧消失 —— 那会是一片空白，看不出哪里错了。
                Microsoft::WRL::ComPtr<ID2D1DeviceContext> device_context;
                if (SUCCEEDED(target.QueryInterface(
                        IID_PPV_ARGS(device_context.GetAddressOf())))) {
                    Failure view_failure;
                    if (view.initialize(target, device_context.Get(), view_failure)) {
                        // 芯片宽度要用 DirectWrite 量，得等工厂就绪 ——
                        // 顺手在这里做一次（芯片只在曲库变化时才变）。
                        view.measure_chips(state.bgm_library);
                        view.measure_chips(state.sfx_library);
                        view_ready = true;
                    } else {
                        ::OutputDebugStringA(
                            ("面板视图初始化失败: " + view_failure.to_string() + "\n").c_str());
                    }
                }
            }
            static_cast<void>(context);
            if (view_ready) {
                view.draw(target, layout, state);
            }
        },
        failure);
    if (!created) {
        std::printf("窗口创建失败: %s\n", failure.to_string().c_str());
        return 1;
    }

    std::printf("面板已创建: %.0f x %.0f 逻辑像素（与原型一致）, DPI %u\n", metrics.width,
                metrics.height, window.dpi());
    std::printf("视图初始化: %s\n", view_ready ? "成功" : "失败（这一帧是空的）");
    if (!view_ready) {
        return 1;
    }

    if (!png_path.empty()) {
        Failure png_failure;
        if (window.save_frame_png(png_path, png_failure)) {
            std::printf("已保存帧: %s\n", path_to_utf8(png_path).c_str());
        } else {
            std::printf("保存帧失败: %s\n", png_failure.to_string().c_str());
            return 1;
        }
    }
    if (exit_after) {
        return 0;
    }
    return window.run_message_loop();
}

// 悬浮球 spike 的入口：建球 → 自检 → 出图或跑消息循环。
int run_orb_spike(const std::filesystem::path& png_path, bool self_test, bool exit_after,
                  bool paused) {
    OrbWindow orb;
    OrbWindow::Options options;
    options.window.title = L"音乐板 · 悬浮球";
    // 放在左上角一块显眼又不挡事的位置
    options.initial_x = 80;
    options.initial_y = 80;

    OrbVisual visual;
    visual.progress = 0.42f;
    visual.rotation_radians = 0.9f;
    visual.amplitude = 0.55f;   // 假装此刻音频能量是 55%，用来看辉光的样子
    visual.playing = !paused;

    Failure failure;
    const bool created = orb.create(
        options,
        [&visual](ID2D1RenderTarget& target, const DrawContext& context) {
            draw_orb(target, context, visual);
        },
        failure);
    if (!created) {
        std::printf("悬浮球创建失败: %s\n", failure.to_string().c_str());
        return 1;
    }

    std::printf("悬浮球已创建: %d x %d 逻辑像素（球 %d + 进度环外径 %d + 两侧辉光余量各 %d），"
                "当前 DPI %u, 物理 %d x %d\n",
                kOrbWindowLogicalSize, kOrbWindowLogicalSize, kOrbLogicalDiameter,
                kOrbRingOuterDiameter, kOrbGlowMargin, orb.dpi(), orb.physical_width(),
                orb.physical_height());

    if (!png_path.empty()) {
        Failure png_failure;
        if (orb.save_frame_png(png_path, png_failure)) {
            std::printf("已保存帧: %s\n", path_to_utf8(png_path).c_str());
        } else {
            std::printf("保存帧失败: %s\n", png_failure.to_string().c_str());
            return 1;
        }
    }

    if (self_test) {
        return run_orb_self_test(orb);
    }
    if (exit_after) {
        return 0;   // 只要图，不进消息循环
    }

    // 交互模式：左键单击打印一行、右键打印一行、拖动会记住位置。
    // 这一步是给"真人在桌面上点一下看看"用的 —— 自检测不到手感。
    orb.set_activate_callback([] { std::printf("左键单击 → 展开面板（阶段 6 接）\n"); });
    orb.set_context_menu_callback([](POINT point) {
        std::printf("右键单击 → 弹出菜单（阶段 6 接），屏幕坐标 %ld,%ld\n",
                    static_cast<long>(point.x), static_cast<long>(point.y));
    });
    orb.set_moved_callback([](POINT top_left) {
        std::printf("拖动结束 → 记住位置 %ld,%ld\n", static_cast<long>(top_left.x),
                    static_cast<long>(top_left.y));
    });
    std::printf("交互模式：左键单击 / 右键单击 / 按住拖动。关掉窗口退出。\n");
    return orb.run_message_loop();
}

}  // namespace

// 必须用 wmain：MSVC 的 char** argv 走活动代码页(936)，中文路径会被打乱
int wmain(int argc, wchar_t** argv) {
    // 不缓冲。Debug CRT 的断言失败会直接 abort()，而重定向时 stdout 是块缓冲的，
    // 崩溃前打印的每一行都会丢掉 —— 现场只剩一个退出码。踩过一次，写进这里当默认。
    static_cast<void>(std::setvbuf(stdout, nullptr, _IONBF, 0));
#if defined(_MSC_VER)
    // 断言写到 stderr 而不是弹一个模态对话框。
    // 在没人看的自动化里，模态框等于永久挂起；而断言文本本身是最有用的现场信息，
    // 不该只出现在一个看不见的窗口里。与测试脚手架同一套做法。
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif

    std::filesystem::path png_path;
    bool exit_after_png = false;
    bool self_test = false;
    bool orb_mode = false;
    bool paused = false;
    bool panel_mode = false;
    std::filesystem::path library_path;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--png" && i + 1 < argc) {
            png_path = std::filesystem::path(argv[++i]);
        } else if (arg == L"--exit") {
            exit_after_png = true;
        } else if (arg == L"--selftest") {
            // 自检块自己会 return，不要在这里也设 exit_after_png ——
            // 那样会在自检之前就提前返回（实测踩过一次，输出里什么都没有）。
            self_test = true;
        } else if (arg == L"--orb") {
            orb_mode = true;
        } else if (arg == L"--paused") {
            paused = true;
        } else if (arg == L"--panel") {
            panel_mode = true;
        } else if (arg == L"--library" && i + 1 < argc) {
            library_path = std::filesystem::path(argv[++i]);
        }
    }

    if (orb_mode) {
        return run_orb_spike(png_path, self_test, exit_after_png, paused);
    }
    if (panel_mode) {
        return run_panel_spike(library_path, png_path, exit_after_png);
    }

    Resources resources;
    Failure failure;
    if (!resources.initialize(failure)) {
        std::printf("资源初始化失败: %s\n", failure.to_string().c_str());
        return 1;
    }

    CompositionWindow window;
    CompositionWindow::Options options;
    options.window.title = L"音乐板";
    options.window.logical_width = kPanelLogicalWidth;
    options.window.logical_height = kPanelLogicalHeight;
    options.titlebar_logical_height = static_cast<int>(kTitlebarHeight);

    const bool created = window.create(
        options,
        [&resources](ID2D1RenderTarget& target, const DrawContext& context) {
            draw_panel(target, context, resources);
        },
        failure);
    if (!created) {
        std::printf("窗口创建失败: %s\n", failure.to_string().c_str());
        return 1;
    }

    std::printf("窗口已创建: %d x %d 逻辑像素, 当前 DPI %u, 物理 %u x %u\n",
                options.window.logical_width, options.window.logical_height, window.dpi(),
                static_cast<unsigned>(scale_for_dpi(options.window.logical_width, window.dpi())),
                static_cast<unsigned>(scale_for_dpi(options.window.logical_height, window.dpi())));

    if (!png_path.empty()) {
        Failure png_failure;
        if (window.save_frame_png(png_path, png_failure)) {
            std::printf("已保存帧: %s\n", path_to_utf8(png_path).c_str());
        } else {
            std::printf("保存帧失败: %s\n", png_failure.to_string().c_str());
            return 1;
        }
    }

    if (exit_after_png) {
        return 0;
    }

    if (self_test) {
        // 主动问窗口"这个点算哪个区域" —— 这测的正是 WM_NCHITTEST 那段逻辑本身，
        // 比模拟鼠标拖动更直接，也不依赖窗口是否在最前台。
        RECT client{};
        ::GetClientRect(window.handle(), &client);
        POINT origin{client.left, client.top};
        ::ClientToScreen(window.handle(), &origin);

        const auto hit_test = [&](int logical_x, int logical_y) -> LRESULT {
            const int x = origin.x + scale_for_dpi(logical_x, window.dpi());
            const int y = origin.y + scale_for_dpi(logical_y, window.dpi());
            return ::SendMessageW(window.handle(), WM_NCHITTEST, 0,
                                  MAKELPARAM(static_cast<short>(x), static_cast<short>(y)));
        };

        struct Case {
            int x;
            int y;
            LRESULT expected;
            const char* label;
        };
        const Case cases[] = {
            {60, 20, HTCAPTION, "标题栏内 → HTCAPTION（拖动走系统）"},
            {1230, 20, HTCAPTION, "标题栏右端 → HTCAPTION"},
            {200, 100, HTCLIENT, "标题栏下方 → HTCLIENT（交给自绘控件）"},
            {600, 400, HTCLIENT, "面板中部 → HTCLIENT"},
            {60, 43, HTCLIENT, "标题栏下边界外 → HTCLIENT"},
        };

        int failures = 0;
        for (const Case& item : cases) {
            const LRESULT actual = hit_test(item.x, item.y);
            const bool ok = (actual == item.expected);
            if (!ok) {
                ++failures;
            }
            std::printf("%s %s  (逻辑 %d,%d -> %ld)\n", ok ? "PASS" : "FAIL", item.label, item.x, item.y, static_cast<long>(actual));
        }
        std::printf("---- spike 自检: %d/%zu 通过 ----\n",
                    static_cast<int>(std::size(cases)) - failures, std::size(cases));
        return failures == 0 ? 0 : 1;
    }

    return window.run_message_loop();
}
