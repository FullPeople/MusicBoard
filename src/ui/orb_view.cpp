#include "core/localization.h"
#include "ui/orb_view.h"

#include <cmath>

#include "platform/orb_window.h"
#include "ui/palette.h"
#include "ui/vector_art.h"

#pragma comment(lib, "dwrite.lib")

namespace musicboard::ui {
namespace {

[[nodiscard]] D2D1_POINT_2F to_d2d(const Point& point) noexcept {
    return D2D1::Point2F(point.x, point.y);
}

[[nodiscard]] D2D1_COLOR_F with_alpha(const D2D1_COLOR_F& color, float alpha) noexcept {
    return D2D1::ColorF(color.r, color.g, color.b, alpha);
}

// 往背景暗色上混 —— 透明窗口上"压暗"不能靠降 alpha（那看起来像破了个洞）
[[nodiscard]] D2D1_COLOR_F toward_dark(const D2D1_COLOR_F& color, float amount) noexcept {
    const float t = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
    return D2D1::ColorF(color.r * (1.0f - t) + kBgVoid.r * t,
                        color.g * (1.0f - t) + kBgVoid.g * t,
                        color.b * (1.0f - t) + kBgVoid.b * t, color.a);
}

}  // namespace

OrbView::~OrbView() = default;

bool OrbView::initialize(ID2D1RenderTarget& target, ID2D1DeviceContext* device_context,
                         Failure& failure) {
    device_context_ = device_context;
    if (!ensure_brush(target, failure)) {
        return false;
    }
    // 封面缓存必须在这里初始化 —— 它内部要建 WIC 解码器与位图。
    // 少了这一步，covers_.get() 永远返回空：球面上就永远看不到封面
    // （面板那边一直是初始化过的，所以只有球缺图）。
    if (!covers_.initialize(device_context, failure)) {
        return false;
    }
    // 球上不写任何文字（104px 写不下，写了也糊成一团），所以完全不碰 DirectWrite。
    // 少一个依赖就少一处启动失败的可能。
    return true;
}

bool OrbView::ensure_brush(ID2D1RenderTarget& target, Failure& failure) {
    if (brush_ && brush_target_ == &target) {
        return true;
    }
    brush_.Reset();
    if (FAILED(target.CreateSolidColorBrush(D2D1::ColorF(0x000000, 1.0f), D2D1::BrushProperties(),
                                            brush_.GetAddressOf())) ||
        !brush_) {
        failure.set(fail_code::platform_api_failed, tr("悬浮球画刷创建失败", "Could not create orb brush"));
        return false;
    }
    brush_target_ = &target;
    return true;
}

ID2D1EllipseGeometry* OrbView::circular_mask(ID2D1RenderTarget& target, const Point& center,
                                             float radius) {
    const bool same = mask_geometry_ != nullptr && mask_target_ == &target &&
                      std::fabs(mask_radius_ - radius) < 0.01f &&
                      std::fabs(mask_center_.x - center.x) < 0.01f &&
                      std::fabs(mask_center_.y - center.y) < 0.01f;
    if (same) {
        return mask_geometry_.Get();
    }
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    target.GetFactory(factory.GetAddressOf());
    if (!factory) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<ID2D1EllipseGeometry> geometry;
    if (FAILED(factory->CreateEllipseGeometry(D2D1::Ellipse(to_d2d(center), radius, radius),
                                              geometry.GetAddressOf())) ||
        !geometry) {
        return nullptr;
    }
    mask_geometry_ = std::move(geometry);
    mask_target_ = &target;
    mask_radius_ = radius;
    mask_center_ = center;
    return mask_geometry_.Get();
}

void OrbView::draw(ID2D1RenderTarget& target, float logical_width, float logical_height,
                   const OrbViewState& state) {
    Failure brush_failure;
    if (!ensure_brush(target, brush_failure)) {
        ::OutputDebugStringA(
            (tr("悬浮球画刷创建失败: ", "Orb brush failed: ") + brush_failure.to_string() + "\n").c_str());
        return;
    }

    // **先清干净。** 合成表面的内容是未定义的，不清就会叠上一帧的残留：
    // 进度环与辉光每帧都在动，叠起来看就是"轮盘一直在闪烁"。
    // 面板之所以没这个问题，是因为它第一件事就是铺满不透明的窗口底色。
    target.Clear(D2D1::ColorF(0, 0.0f));

    D2D1_MATRIX_3X2_F base_transform;
    target.GetTransform(&base_transform);
    const float center_x = logical_width * 0.5f;
    const float center_y = logical_height * 0.5f;
    const Point center{center_x, center_y};
    const float ring_radius =
        static_cast<float>(kOrbRingOuterDiameter) * 0.5f - kOrbRingWidth * 0.5f;
    const float disc_radius = static_cast<float>(kOrbLogicalDiameter) * 0.5f;
    // 没在放就压暗。空闲（台上什么都没有）压得更狠一点，"有曲子但暂停"和
    // "什么都没加载"必须分得开 —— 这是用户点不点它的唯一线索。
    const float dim = state.playing ? 0.0f : (state.has_track ? 0.75f : 1.0f);
    const D2D1_COLOR_F accent = state.accent;

    // ---- 辉光：跟着音频能量起伏 ----
    if (state.playing && state.amplitude > 0.001f) {
        constexpr int kLayers = 14;
        for (int layer = kLayers; layer >= 1; --layer) {
            const float t = static_cast<float>(layer) / static_cast<float>(kLayers);
            const float radius =
                ring_radius + t * (static_cast<float>(kOrbGlowMargin) - 2.0f);
            const float alpha = state.amplitude * (1.0f - t) * (1.0f - t) * 0.30f;
            brush_->SetColor(with_alpha(accent, alpha));
            target.FillEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), radius, radius),
                               brush_.Get());
        }
    }

    // ---- 进度环底 ----
    brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f - 0.05f * dim));
    target.DrawEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), ring_radius, ring_radius),
                       brush_.Get(), kOrbRingWidth);
    // 外圈一道细亮边：把球从桌面上"抠"出来（设计稿里 .orb 有 inset 亮线）
    brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f));
    target.DrawEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), disc_radius - 0.5f,
                                    disc_radius - 0.5f),
                       brush_.Get(), 1.0f);

    // ---- 盘面 ----
    // 空闲时也必须是**一眼看得见的一颗球**：压得太暗在深色桌面上就等于没有。
    // 压暗的幅度只用来区分"在放 / 暂停 / 空闲"三种状态，不负责把它藏起来。
    brush_->SetColor(toward_dark(kSurface2, state.has_track ? 0.45f : 0.30f));
    target.FillEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), disc_radius, disc_radius),
                       brush_.Get());

    // ---- 封面：圆形裁剪 + 跟着播放转（与面板上的唱片同一套做法）----
    ID2D1Bitmap1* art = nullptr;
    if (state.has_track && !state.art_path.empty() && device_context_ != nullptr &&
        static_cast<ID2D1RenderTarget*>(device_context_) == &target) {
        Failure ignored;
        art = covers_.get(state.art_path, ignored);
    }

    if (art != nullptr) {
        ID2D1EllipseGeometry* mask = circular_mask(target, center, disc_radius);
        bool masked = false;
        if (mask != nullptr) {
            const D2D1_LAYER_PARAMETERS layer = D2D1::LayerParameters(
                D2D1::InfiniteRect(), mask, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            target.PushLayer(layer, nullptr);
            masked = true;
        }
        target.SetTransform(D2D1::Matrix3x2F::Rotation(
            state.rotation_radians * 180.0f / 3.14159265f, D2D1::Point2F(center_x, center_y)) * base_transform);
        const D2D1_SIZE_F size = art->GetSize();
        if (size.width > 0.0f && size.height > 0.0f) {
            const float span = disc_radius * 2.0f;
            const float scale = std::max(span / size.width, span / size.height);
            const float draw_width = size.width * scale;
            const float draw_height = size.height * scale;
            target.DrawBitmap(art,
                              D2D1::RectF(center_x - draw_width * 0.5f,
                                          center_y - draw_height * 0.5f,
                                          center_x + draw_width * 0.5f,
                                          center_y + draw_height * 0.5f),
                              1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        target.SetTransform(base_transform);
        if (masked) {
            target.PopLayer();
        }
        // 边缘压暗一点，让它看起来是贴在盘上的
        brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.28f + dim * 0.25f));
        target.DrawEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y),
                                        disc_radius - 0.75f, disc_radius - 0.75f),
                           brush_.Get(), 1.5f);
    } else {
        // 没有封面 → 黑胶沟槽 + 一道跟着转的高光（沟槽是旋转对称的，
        // 光看沟槽分不出转没转）
        constexpr int kGrooves = 9;
        for (int i = 1; i <= kGrooves; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kGrooves + 1);
            const float radius = disc_radius * (0.36f + t * 0.60f);
            brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.045f - 0.015f * dim));
            target.DrawEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), radius, radius),
                               brush_.Get(), 1.0f);
        }
        draw_vinyl_reflection(target,*brush_.Get(),to_d2d(center),disc_radius,state.rotation_radians);
    }

    // ---- 中心 logo ----
    // 设计稿明确要求球心放 logo、**不镂空**（球只有 104px，镂空一个洞更像破了个口）。
    // 有封面时只留一圈细环：封面本身就是"这是哪一首"，再压个圆点会挡掉画面中心。
    {
        const float logo_radius = disc_radius * (art != nullptr ? 0.14f : 0.30f);
        brush_->SetColor(state.playing ? with_alpha(accent, 0.92f)
                                       : D2D1::ColorF(kText3.r, kText3.g, kText3.b, 0.85f));
        target.FillEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), logo_radius,
                                        logo_radius),
                           brush_.Get());
        // 一小段内环当标记，不用文字：104px 的球上写字只会糊成一团
        brush_->SetColor(D2D1::ColorF(kBgVoid.r, kBgVoid.g, kBgVoid.b, 0.85f));
        target.DrawEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), logo_radius * 0.52f,
                                        logo_radius * 0.52f),
                           brush_.Get(), logo_radius * 0.30f);
    }

    if (state.hover > .001f) {
        if (auto* mask=circular_mask(target,center,disc_radius)) {
            target.PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),mask),nullptr);
            brush_->SetColor(D2D1::ColorF(0,0,0,.66f*state.hover));
            target.FillEllipse(D2D1::Ellipse(to_d2d(center),disc_radius,disc_radius),brush_.Get());
            const float split=center_y+disc_radius*.4f;
            brush_->SetColor(with_alpha(state.accent,.12f*state.hover));
            if(state.hover_zone==1) target.FillRectangle(D2D1::RectF(center_x-disc_radius,center_y-disc_radius,center_x+disc_radius,split),brush_.Get());
            else if(state.hover_zone==2) target.FillRectangle(D2D1::RectF(center_x-disc_radius,split,center_x+disc_radius,center_y+disc_radius),brush_.Get());
            brush_->SetColor(D2D1::ColorF(1,1,1,.28f*state.hover));
            target.DrawLine({center_x-disc_radius,split},{center_x+disc_radius,split},brush_.Get(),1);
            brush_->SetColor(with_alpha(kText1,state.hover));
            const float top=center_y-12;
            target.DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(center_x-13,top-10,center_x+13,top+9),3,3),brush_.Get(),1.8f);
            target.DrawLine({center_x-13,top-4},{center_x+13,top-4},brush_.Get(),1.5f);
            brush_->SetColor(with_alpha(state.has_track?kText1:kText3,state.hover));
            draw_play_icon(target,*brush_.Get(),{center_x,center_y+disc_radius*.67f},16,state.playing);
            target.PopLayer();
        }
    }

    // ---- 进度弧：从 12 点方向顺时针 ----
    if (state.progress > 0.001f) {
        // 几何与描边样式都必须取自 **render target 自己的工厂**。
        // 另建一个工厂去造样式再交给这个 target，D2D 不给 HRESULT —— 直接 abort
        // （阶段 3.5 被咬过一次，所以这条注释留在代码里）。
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        target.GetFactory(factory.GetAddressOf());
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> arc;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (factory && SUCCEEDED(factory->CreatePathGeometry(arc.GetAddressOf())) &&
            SUCCEEDED(arc->Open(sink.GetAddressOf()))) {
            const float sweep = std::min(state.progress, 1.0f) * 6.2831853f;
            const float start_angle = -1.5707963f;   // 12 点
            const D2D1_POINT_2F start =
                D2D1::Point2F(center_x + std::cos(start_angle) * ring_radius,
                              center_y + std::sin(start_angle) * ring_radius);
            sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(
                D2D1::Point2F(center_x + std::cos(start_angle + sweep) * ring_radius,
                              center_y + std::sin(start_angle + sweep) * ring_radius),
                D2D1::SizeF(ring_radius, ring_radius), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                sweep > 3.1415927f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink.Reset();   // 几何要在 sink 释放之后才能用（文档里写明的，违反只 abort）

            Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_caps;
            if (SUCCEEDED(factory->CreateStrokeStyle(
                    D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_ROUND,
                                                D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f),
                    nullptr, 0, round_caps.GetAddressOf()))) {
                const D2D1_COLOR_F arc_color = toward_dark(accent, 0.45f);
                brush_->SetColor(D2D1::ColorF(arc_color.r, arc_color.g, arc_color.b,
                                              state.playing ? 1.0f : 0.8f));
                target.DrawGeometry(arc.Get(), brush_.Get(), kOrbRingWidth, round_caps.Get());
            }
        }
    }
}

}  // namespace musicboard::ui
