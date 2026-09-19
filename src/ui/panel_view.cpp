#include "core/localization.h"
#include "ui/panel_view.h"

#include "ui/palette.h"
#include "ui/vector_art.h"

#include <algorithm>
#include <cmath>

#include "audio/waveform.h"
#include "core/path_utf8.h"

#pragma comment(lib, "dwrite.lib")

namespace musicboard::ui {
namespace {

// ui::Point 与 D2D1_POINT_2F 是同构的，但不互通。这里显式转，别做隐式转换 ——
// 那样以后给 Point 加字段时会静默错位。
[[nodiscard]] D2D1_POINT_2F to_d2d(const Point& point) noexcept {
    return D2D1::Point2F(point.x, point.y);
}

[[nodiscard]] D2D1_COLOR_F with_alpha(const D2D1_COLOR_F& color, float alpha) {
    return D2D1::ColorF(color.r, color.g, color.b, alpha);
}

[[nodiscard]] std::wstring to_wide(const std::string& text) {
    std::wstring wide;
    Failure ignored;
    if (!utf8_to_wide(text, wide, ignored)) {
        return tr(L"[乱码]", L"[invalid text]");
    }
    return wide;
}

// 把秒数写成 mm:ss（一小时以上写成 h:mm:ss）
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

}  // namespace

PanelView::~PanelView() {
    // 封面缓存持有位图，位图属于设备上下文 —— 必须先于设备上下文释放。
    // 缓存是成员，所以在这里显式清掉，别等到成员析构（那时可能已经晚了）。
    covers_.clear();
}

bool PanelView::initialize(ID2D1RenderTarget& target, ID2D1DeviceContext* device_context,
                           Failure& failure) {
    // target 只用来确认调用方手里确实有一张可画的 target；画刷与封面都在
    // 真正要画的那张上现建（见 ensure_brush 与 target_supports_covers）。
    static_cast<void>(target);
    if (device_context == nullptr) {
        failure.set(fail_code::invalid_argument, tr("没有设备上下文，面板画不了封面", "Missing device context for cover rendering"));
        return false;
    }
    device_context_ = device_context;
    if (!covers_.initialize(device_context, failure)) {
        return false;
    }

    // 初始化时不建画刷：画刷属于某一张 target，而 initialize 拿到的 target
    // 不一定就是之后每帧画的那个（截图会换成软件 target）。见 ensure_brush。

    HRESULT hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(write_.GetAddressOf()));
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, tr("创建 DirectWrite 工厂失败", "Could not create DirectWrite factory"));
        return false;
    }

    // 字体回退：中文优先用系统里一定有、而且小字号清晰的几款。
    // DirectWrite 默认的字体对中文会回退到一个不太好看的衬线体。
    const wchar_t* family = L"Microsoft YaHei UI";
    const auto make_format = [&](ComPtr<IDWriteTextFormat>& out, float size,
                                 DWRITE_FONT_WEIGHT weight,
                                 DWRITE_TEXT_ALIGNMENT alignment) -> bool {
        const HRESULT format_hr = write_->CreateTextFormat(
            family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
            L"zh-CN", out.GetAddressOf());
        if (FAILED(format_hr)) {
            return false;
        }
        static_cast<void>(out->SetTextAlignment(alignment));
        static_cast<void>(out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
        static_cast<void>(out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
        // Creating a sign from a format that already carries a sign retains
        // that previous sign. Doing this each draw builds an unbounded chain
        // whose recursive release overflows the stack on exit.
        ComPtr<IDWriteInlineObject> ellipsis;
        HRESULT trim_hr = write_->CreateEllipsisTrimmingSign(out.Get(), ellipsis.GetAddressOf());
        if (FAILED(trim_hr)) {
            failure.set(fail_code::platform_api_failed,
                tr("创建省略号失败 HRESULT=", "Could not create ellipsis HRESULT=") + std::to_string(static_cast<unsigned long>(trim_hr)));
            return false;
        }
        trimming_signs_.emplace(out.Get(), std::move(ellipsis));
        return true;
    };

    if (!make_format(title_format_, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                     DWRITE_TEXT_ALIGNMENT_LEADING) ||
        !make_format(section_format_, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                     DWRITE_TEXT_ALIGNMENT_LEADING) ||
        !make_format(body_format_, 12.5f, DWRITE_FONT_WEIGHT_NORMAL,
                     DWRITE_TEXT_ALIGNMENT_LEADING) ||
        !make_format(caption_format_, 10.5f, DWRITE_FONT_WEIGHT_NORMAL,
                     DWRITE_TEXT_ALIGNMENT_LEADING) ||
        !make_format(badge_format_, 8.5f, DWRITE_FONT_WEIGHT_BOLD,
                     DWRITE_TEXT_ALIGNMENT_CENTER) ||
        !make_format(centered_format_, 12.0f, DWRITE_FONT_WEIGHT_NORMAL,
                     DWRITE_TEXT_ALIGNMENT_CENTER)) {
        failure.set(fail_code::platform_api_failed,
                    tr("创建文本格式失败（可能缺少 Microsoft YaHei UI）", "Could not create text format (Microsoft YaHei UI may be unavailable)"));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

void PanelView::measure_chips(LibraryView& library) {
    library.chip_widths.assign(library.chips.size(), 0.0f);
    if (!write_) {
        return;
    }
    for (std::size_t i = 0; i < library.chips.size(); ++i) {
        const std::string& name = library.chips[i];
        const auto cached = std::find_if(
            chip_width_cache_.begin(), chip_width_cache_.end(),
            [&name](const std::pair<std::string, float>& item) { return item.first == name; });
        if (cached != chip_width_cache_.end()) {
            library.chip_widths[i] = cached->second;
            continue;
        }

        // 真的量一遍。用 IDWriteTextLayout 而不是按字数估 ——
        // 中日韩字符与拉丁字母宽度差得远，估出来的宽度会让药丸要么撑不住文字、
        // 要么白留一块，而且命中区域会跟着一起错。
        Microsoft::WRL::ComPtr<IDWriteTextLayout> text_layout;
        const std::wstring wide = to_wide(name);
        const HRESULT hr = write_->CreateTextLayout(
            wide.c_str(), static_cast<UINT32>(wide.size()), caption_format_.Get(), 1024.0f, 32.0f,
            text_layout.GetAddressOf());
        float width = 0.0f;
        if (SUCCEEDED(hr) && text_layout) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(text_layout->GetMetrics(&metrics))) {
                width = metrics.widthIncludingTrailingWhitespace;
            }
        }
        if (width <= 0.0f) {
            width = 12.0f * static_cast<float>(name.size());   // 兜底，不理想但不会是 0
        }
        width += kChipPaddingX * 2.0f;
        library.chip_widths[i] = width;
        chip_width_cache_.emplace_back(name, width);
    }
}

bool PanelView::ensure_brush(ID2D1RenderTarget& target, Failure& failure) {
    if (brush_ && brush_target_ == &target) {
        return true;
    }
    brush_.Reset();
    brush_target_ = nullptr;
    const HRESULT hr = target.CreateSolidColorBrush(D2D1::ColorF(0x000000, 1.0f),
                                                    D2D1::BrushProperties(),
                                                    brush_.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, tr("创建面板画刷失败", "Could not create panel brush"));
        return false;
    }
    brush_target_ = &target;
    return true;
}

void PanelView::fill_rounded(ID2D1RenderTarget& target, const Rect& rect, float radius,
                             const D2D1_COLOR_F& color) {
    if (rect.empty() || color.a <= 0.0f) {
        return;
    }
    brush_->SetColor(color);
    const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(
        D2D1::RectF(rect.x, rect.y, rect.right(), rect.bottom()), radius, radius);
    target.FillRoundedRectangle(&rounded, brush_.Get());
}

void PanelView::stroke_rounded(ID2D1RenderTarget& target, const Rect& rect, float radius,
                               const D2D1_COLOR_F& color, float width) {
    if (rect.empty() || color.a <= 0.0f || width <= 0.0f) {
        return;
    }
    brush_->SetColor(color);
    const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(
        D2D1::RectF(rect.x, rect.y, rect.right(), rect.bottom()), radius, radius);
    target.DrawRoundedRectangle(&rounded, brush_.Get(), width);
}

void PanelView::draw_text(ID2D1RenderTarget& target, IDWriteTextFormat& format,
                          const std::string& text, const Rect& rect, const D2D1_COLOR_F& color,
                          DWRITE_TEXT_ALIGNMENT alignment) {
    if (text.empty() || rect.empty()) {
        return;
    }
    static_cast<void>(format.SetTextAlignment(alignment));
    const DWRITE_TRIMMING no_trimming{};
    static_cast<void>(format.SetTrimming(&no_trimming, nullptr));
    brush_->SetColor(color);
    const std::wstring wide = to_wide(text);
    target.DrawTextW(wide.c_str(), static_cast<UINT32>(wide.size()), &format,
                     D2D1::RectF(rect.x, rect.y, rect.right(), rect.bottom()), brush_.Get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void PanelView::draw_text_ellipsis(ID2D1RenderTarget& target, IDWriteTextFormat& format,
                                   const std::string& text, const Rect& rect,
                                   const D2D1_COLOR_F& color, DWRITE_TEXT_ALIGNMENT alignment) {
    if (text.empty() || rect.empty()) {
        return;
    }

    // 用 DirectWrite 自己的裁剪做省略号，而不是自己数字符 ——
    // 中日韩字符宽度不一，按字符数猜一定会算错。做法是给一个很长的尾随
    // 省略号签名，由布局引擎在放不下时自动省略。
    const auto ellipsis = trimming_signs_.find(&format);
    DWRITE_TRIMMING trimming{};
    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    if (ellipsis != trimming_signs_.end()) {
        static_cast<void>(format.SetTrimming(&trimming, ellipsis->second.Get()));
    }
    static_cast<void>(format.SetTextAlignment(alignment));
    brush_->SetColor(color);
    const std::wstring wide = to_wide(text);
    target.DrawTextW(wide.c_str(), static_cast<UINT32>(wide.size()), &format,
                     D2D1::RectF(rect.x, rect.y, rect.right(), rect.bottom()), brush_.Get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// ---------------------------------------------------------------------------
// 音波图（带进度遮罩）
// ---------------------------------------------------------------------------

void PanelView::draw_waveform(ID2D1RenderTarget& target, const Rect& area,
                              const std::vector<float>& envelope, double progress,
                              const D2D1_COLOR_F& accent, bool playing) {
    if (area.empty()) {
        return;
    }
    const float center_y = area.y + area.height * 0.5f;
    const float half = area.height * 0.5f - 1.0f;
    if (half <= 0.0f || area.width <= 1.0f) {
        return;
    }

    // 没有包络（没选曲、或这首歌解不出来）时画一条居中的细线，
    // **不伪造一条波形** —— 假的波形会让人以为有声音。
    if (envelope.empty()) {
        brush_->SetColor(with_alpha(kHairline, 0.8f));
        target.DrawLine(D2D1::Point2F(area.x, center_y), D2D1::Point2F(area.right(), center_y),
                        brush_.Get(), 1.0f);
        return;
    }

    // 每根竖条一个矩形。桶数通常就是像素宽，所以一桶一像素。
    const std::size_t bars = std::min<std::size_t>(envelope.size(),
                                                   static_cast<std::size_t>(std::max(1.0f, area.width)));
    const float bar_width = area.width / static_cast<float>(bars);
    const float bar_thickness = std::max(1.0f, bar_width - 1.0f);

    const auto paint = [&](float clip_right, const D2D1_COLOR_F& color) {
        if (clip_right <= area.x) {
            return;
        }
        target.PushAxisAlignedClip(
            D2D1::RectF(area.x, area.y, clip_right, area.bottom()),
            D2D1_ANTIALIAS_MODE_ALIASED);
        brush_->SetColor(color);
        for (std::size_t i = 0; i < bars; ++i) {
            const float value = envelope[i * envelope.size() / bars];
            const float bar_height = std::max(1.0f, value * half * 2.0f);
            const float x = area.x + static_cast<float>(i) * bar_width;
            target.FillRectangle(
                D2D1::RectF(x, center_y - bar_height * 0.5f, x + bar_thickness,
                            center_y + bar_height * 0.5f),
                brush_.Get());
        }
        target.PopAxisAlignedClip();
    };

    // 先整条画灰，再把已播的那一段裁出来画成总线色。
    // 这就是"遮罩"：未播部分是常态灰，播过部分是总线色。
    paint(area.right(), kWaveDim);
    if (progress > 0.0) {
        const double clamped = progress > 1.0 ? 1.0 : progress;
        paint(area.x + area.width * static_cast<float>(clamped),
              playing ? accent : with_alpha(accent, 0.55f));
    }
}

// ---------------------------------------------------------------------------
// 唱片
// ---------------------------------------------------------------------------

bool PanelView::target_supports_covers(ID2D1RenderTarget& target) const noexcept {
    return device_context_ != nullptr &&
           static_cast<ID2D1RenderTarget*>(device_context_) == &target;
}

ID2D1EllipseGeometry* PanelView::circular_mask(ID2D1RenderTarget& target, const Point& center,
                                                 float radius) {
    if (radius <= 0.0f) {
        return nullptr;
    }
    const bool same = mask_geometry_ != nullptr && mask_target_ == &target &&
                      std::fabs(mask_radius_ - radius) < 0.01f &&
                      std::fabs(mask_center_.x - center.x) < 0.01f &&
                      std::fabs(mask_center_.y - center.y) < 0.01f;
    if (same) {
        return mask_geometry_.Get();
    }
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    target.GetFactory(&factory);
    if (factory == nullptr) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<ID2D1EllipseGeometry> geometry;
    if (FAILED(factory->CreateEllipseGeometry(
            D2D1::Ellipse(to_d2d(center), radius, radius), &geometry)) ||
        geometry == nullptr) {
        return nullptr;
    }
    mask_geometry_ = std::move(geometry);
    mask_target_ = &target;
    mask_radius_ = radius;
    mask_center_ = center;
    return mask_geometry_.Get();
}

void PanelView::draw_disc(ID2D1RenderTarget& target, const Rect& disc, const DeckView& deck,
                          const D2D1_COLOR_F& accent, bool show_center_dot, bool dim_when_idle,
                          float animation_time) {
    const Point center = PanelLayout::disc_center(disc);
    const float radius = PanelLayout::disc_radius(disc);
    if (radius <= 1.0f) {
        return;
    }

    // 暂停时整体压暗。**压暗是往背景暗色混，不是降 alpha** ——
    // 降 alpha 会让盘面变半透明，看起来像破了个洞。
    //
    // 但只压 BGM 那一台：音效台的"在放"是零点几秒的事，暂停一个音效
    // 不该让它看起来"坏掉了"（用户明确要求：暂停音效就是单纯停下，不变暗）。
    const float dim = !dim_when_idle
                          ? 0.0f
                          : (deck.playing ? 0.0f : (deck.has_track ? 0.35f : 0.55f));

    const float angle_degrees = deck.angle_degrees;
    const float angle_radians = angle_degrees * 3.14159265f / 180.0f;

    // 有封面 → 整盘是封面（像光盘的印刷面）；没有 → 黑胶。
    ID2D1Bitmap1* cover = nullptr;
    if (deck.has_track && !deck.path.empty() && target_supports_covers(target)) {
        Failure ignored;
        cover = covers_.get(deck.path, ignored);
    }

    const D2D1_ELLIPSE ellipse = D2D1::Ellipse(to_d2d(center), radius, radius);

    // 盘体始终是黑胶：外圈永远露出唱片本身（原型的 .vinyl 底色 + 沟槽）。
    // 封面只是**盘心那一圈**的印刷面 —— 之前把封面铺满整盘并且用矩形裁剪，
    // 于是它是个会转的方块，这正是用户说的"唱片没有圆形裁剪"。
    {
        const D2D1_COLOR_F base = D2D1::ColorF(
            kSurface2.r * (1.0f - dim) + kBgVoid.r * dim,
            kSurface2.g * (1.0f - dim) + kBgVoid.g * dim,
            kSurface2.b * (1.0f - dim) + kBgVoid.b * dim, 1.0f);
        brush_->SetColor(base);
        target.FillEllipse(ellipse, brush_.Get());

        // 沟槽：一圈圈同心细线（原型的 repeating-radial-gradient 近似）。
        // 盘心那一圈被封面盖住，所以从 0.40 起画。
        constexpr int kGrooves = 14;
        for (int i = 1; i <= kGrooves; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kGrooves + 1);
            const float r = radius * (0.42f + t * 0.55f);
            brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.045f - dim * 0.02f));
            target.DrawEllipse(D2D1::Ellipse(to_d2d(center), r, r), brush_.Get(), 1.0f);
        }
        // 盘缘内高光：一条细亮线，把盘从背景里"提"出来
        brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f - dim * 0.05f));
        target.DrawEllipse(D2D1::Ellipse(to_d2d(center), radius - 1.0f, radius - 1.0f),
                           brush_.Get(), 1.2f);
    }

    // 封面：**圆形裁剪**在盘心（半径 0.62×），跟着播放转。
    // 裁剪用 PushLayer + 椭圆几何遮罩 —— 矩形 clip 做不到圆。
    if (cover != nullptr) {
        const float label_radius = radius * 0.62f;
        ID2D1EllipseGeometry* mask = circular_mask(target, center, label_radius);
        bool masked = false;
        if (mask != nullptr) {
            const D2D1_LAYER_PARAMETERS layer = D2D1::LayerParameters(
                D2D1::InfiniteRect(), mask, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            target.PushLayer(layer, nullptr);
            masked = true;
        }
        // 封面跟着播放转 —— 暂停时 position 不再变，角度自然停住
        D2D1_MATRIX_3X2_F base_transform;
        target.GetTransform(&base_transform);
        target.SetTransform(D2D1::Matrix3x2F::Rotation(angle_degrees, to_d2d(center)) * base_transform);
        const D2D1_SIZE_F size = cover->GetSize();
        if (size.width > 0.0f && size.height > 0.0f) {
            // 按"填满"缩放并居中裁切，避免非正方形封面被拉变形
            const float span = label_radius * 2.0f;
            const float scale = std::max(span / size.width, span / size.height);
            const float draw_width = size.width * scale;
            const float draw_height = size.height * scale;
            target.DrawBitmap(cover,
                              D2D1::RectF(center.x - draw_width * 0.5f,
                                          center.y - draw_height * 0.5f,
                                          center.x + draw_width * 0.5f,
                                          center.y + draw_height * 0.5f),
                              1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        target.SetTransform(base_transform);
        if (masked) {
            target.PopLayer();
        }
        // 封面上再压一层暗角（原型 .vinyl-art::after）：边缘暗下去才像贴在盘上
        brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.30f + dim * 0.25f));
        target.DrawEllipse(D2D1::Ellipse(to_d2d(center), label_radius - 0.75f,
                                        label_radius - 0.75f),
                           brush_.Get(), 1.5f);
        // 盘心环：封面与黑胶之间的那道接缝
        brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f));
        target.DrawEllipse(D2D1::Ellipse(to_d2d(center), label_radius, label_radius),
                           brush_.Get(), 2.0f);
    } else if (show_center_dot) {
        draw_vinyl_reflection(target, *brush_.Get(), to_d2d(center), radius, angle_radians);

        // 没有封面：盘心一个总线色圆点表达播放状态（原型 .vinyl-core）
        const float core_radius = radius * 0.30f;
        const D2D1_COLOR_F core = deck.playing
                                      ? with_alpha(accent, 0.90f)
                                      : with_alpha(kSurface3, 0.95f);
        brush_->SetColor(core);
        target.FillEllipse(D2D1::Ellipse(to_d2d(center), core_radius, core_radius), brush_.Get());
        brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f));
        target.DrawEllipse(D2D1::Ellipse(to_d2d(center), core_radius, core_radius), brush_.Get(),
                           1.0f);
    }

    // 中心轴孔：**一直存在**（原型的 .vinyl-hole）。黑胶/CD 的中心本来就是透的，
    // 这里用背景最深的两级色画成"透过去看到唱盘"的观感。
    {
        const float hole_radius = std::max(3.0f, radius * 0.075f);
        brush_->SetColor(D2D1::ColorF(0.035f, 0.040f, 0.050f, 1.0f));
        target.FillEllipse(D2D1::Ellipse(to_d2d(center), hole_radius, hole_radius),
                           brush_.Get());
        brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f));
        target.DrawEllipse(D2D1::Ellipse(to_d2d(center), hole_radius + 0.5f, hole_radius + 0.5f),
                           brush_.Get(), 1.0f);
    }

    if (deck.hover > .001f && deck.has_track) {
        brush_->SetColor(D2D1::ColorF(0,0,0,.62f*deck.hover));
        target.FillEllipse(ellipse,brush_.Get());
        brush_->SetColor(with_alpha(kText1,deck.hover));
        draw_play_icon(target,*brush_.Get(),to_d2d(center),radius*.40f,deck.playing);
    }

    // 拖动中的落点光圈：会呼吸（半径与透明度都由时间算，掉帧也不会变形）
    if (deck.drop_target) {
        const float phase = 0.5f + 0.5f * std::sin(animation_time * 4.2f);
        const float ring_radius = radius + 5.0f + phase * 5.0f;
        brush_->SetColor(with_alpha(accent, 0.85f - phase * 0.35f));
        target.DrawEllipse(D2D1::Ellipse(to_d2d(center), ring_radius, ring_radius), brush_.Get(),
                           2.5f);
        brush_->SetColor(with_alpha(accent, 0.10f));
        target.FillEllipse(D2D1::Ellipse(to_d2d(center), radius, radius), brush_.Get());
    }
}

// ---------------------------------------------------------------------------
// 电平表与音量条
// ---------------------------------------------------------------------------

void PanelView::draw_level_meter(ID2D1RenderTarget& target, const Rect& area,
                                 const D2D1_COLOR_F& accent, float level) {
    if (area.empty()) {
        return;
    }
    constexpr int kBars = 5;
    const float gap = 2.0f;
    const float bar_width = (area.width - gap * (kBars - 1)) / static_cast<float>(kBars);
    if (bar_width <= 0.5f) {
        return;
    }
    const int lit = static_cast<int>(std::lround(std::clamp(level, 0.0f, 1.0f) * kBars));
    for (int i = 0; i < kBars; ++i) {
        const float height = area.height * (0.35f + 0.65f * static_cast<float>(i + 1) / kBars);
        const Rect bar{area.x + static_cast<float>(i) * (bar_width + gap),
                       area.bottom() - height, bar_width, height};
        // 亮起的部分是总线色，其余是暗底 —— 与音波图同一套读法
        fill_rounded(target, bar, 1.5f,
                     i < lit ? with_alpha(accent, 0.9f) : with_alpha(kSurface3, 0.9f));
    }
}

void PanelView::draw_volume_slider(ID2D1RenderTarget& target, const Rect& area,
                                   std::uint16_t volume_percent, const D2D1_COLOR_F& accent) {
    if (area.empty()) {
        return;
    }
    const float track_height = 4.0f;
    const Rect track{area.x, area.y + (area.height - track_height) * 0.5f, area.width,
                     track_height};
    fill_rounded(target, track, track_height * 0.5f, kSurface3);
    const float ratio = static_cast<float>(std::min<std::uint16_t>(volume_percent, 100)) / 100.0f;
    const Rect filled{track.x, track.y, track.width * ratio, track.height};
    fill_rounded(target, filled, track_height * 0.5f, accent);

    // 旋钮
    const float knob_x = track.x + track.width * ratio;
    const float knob_radius = 6.0f;
    brush_->SetColor(kText1);
    target.FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob_x, area.y + area.height * 0.5f),
                                    knob_radius, knob_radius),
                       brush_.Get());
}

void PanelView::draw_vertical_volume(ID2D1RenderTarget& target, const Rect& area,
                                     std::uint16_t volume_percent, const D2D1_COLOR_F& accent) {
    if (area.empty()) {
        return;
    }
    // 竖向：从下往上长
    const float track_width = 4.0f;
    const Rect track{area.x + (area.width - track_width) * 0.5f, area.y, track_width,
                     area.height};
    fill_rounded(target, track, track_width * 0.5f, kSurface3);

    const float ratio = static_cast<float>(std::min<std::uint16_t>(volume_percent, 100)) / 100.0f;
    const float filled_height = track.height * ratio;
    const Rect filled{track.x, track.bottom() - filled_height, track.width, filled_height};
    fill_rounded(target, filled, track_width * 0.5f, accent);

    const float knob_y = track.bottom() - filled_height;
    brush_->SetColor(kText1);
    target.FillEllipse(D2D1::Ellipse(D2D1::Point2F(track.x + track_width * 0.5f, knob_y), 5.0f,
                                    5.0f),
                       brush_.Get());
}

// ---------------------------------------------------------------------------
// 标题栏与底栏
// ---------------------------------------------------------------------------

void PanelView::draw_titlebar(ID2D1RenderTarget& target, const PanelLayout& layout,
                              const PanelState& state) {
    const Rect& bar = layout.titlebar;
    fill_rounded(target,
                 Rect{bar.x, bar.y, bar.width, bar.height + kWindowRadius},
                 kWindowRadius, with_alpha(kSurface1, 0.55f));
    brush_->SetColor(kHairlineSoft);
    target.DrawLine(D2D1::Point2F(bar.x + 1.0f, bar.bottom() - 0.5f),
                    D2D1::Point2F(bar.right() - 1.0f, bar.bottom() - 0.5f), brush_.Get(), 1.0f);

    // 左侧的圆点标记
    brush_->SetColor(with_alpha(kAmber, 0.9f));
    target.FillEllipse(D2D1::Ellipse(D2D1::Point2F(bar.x + 23.0f, bar.center().y), 8.0f, 8.0f),
                       brush_.Get());

    draw_text(target, *title_format_.Get(), state.title,
              Rect{bar.x + 38.0f, bar.y, 112.0f, bar.height}, kText1);

    // 状态胶囊：吃满中间的剩余宽度。长曲名绝大多数在这就能整条放下。
    const Rect chip{bar.x + 155.0f, bar.y + 9.0f, std::max(0.0f, bar.width - 155.0f - 200.0f),
                    bar.height - 18.0f};
    if (!chip.empty()) {
        fill_rounded(target, chip, chip.height * 0.5f, with_alpha(kSurface2, 0.9f));
        draw_text_ellipsis(target, *body_format_.Get(), state.status_chip,
                           Rect{chip.x + 12.0f, chip.y, chip.width - 24.0f, chip.height}, kText2);
    }

    for (const auto& [id, tab] : std::array<std::pair<int,Rect>,2>{{{11,layout.title_cn},{12,layout.title_en}}}) {
        const bool selected = state.english == (id == 12);
        const Rect shape{tab.x - (id == 12 ? 6.0f : 0.0f), tab.y, tab.width + 6.0f, tab.height};
        target.PushAxisAlignedClip(D2D1::RectF(tab.x,tab.y-1,tab.right(),tab.bottom()+1),D2D1_ANTIALIAS_MODE_ALIASED);
        fill_rounded(target,shape,6,selected?with_alpha(kAmber,.16f):(state.hovered_id==id?kSurface3:kSurface1));
        stroke_rounded(target,shape,6,selected?kAmber:kHairline,1);
        target.PopAxisAlignedClip();
        draw_text(target,*caption_format_.Get(),id==11?"CN":"EN",tab,selected?kAmber:kText2,DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    for (const auto& [id,box] : std::array<std::pair<int,Rect>,3>{{
        {3,layout.title_pin},{4,layout.title_minimize},{2,layout.title_close}}}) {
        const bool hovered=state.hovered_id==id, on=id==3 && state.always_on_top;
        fill_rounded(target,box,6,on?with_alpha(kAmber,.16f):(hovered?kSurface3:D2D1::ColorF(0,0.0f)));
        brush_->SetColor(on?kAmber:(hovered?kText1:kText2));
        const auto center=box.center();
        if(id==3) draw_pin_icon(target,*brush_.Get(),to_d2d(center),on);
        else if(id==4) target.DrawLine({center.x-5,center.y+2},{center.x+5,center.y+2},brush_.Get(),1.6f);
        else {
            if(hovered) brush_->SetColor(kDanger);
            target.DrawLine({center.x-4,center.y-4},{center.x+4,center.y+4},brush_.Get(),1.6f);
            target.DrawLine({center.x+4,center.y-4},{center.x-4,center.y+4},brush_.Get(),1.6f);
        }
    }
}

void PanelView::draw_busbar(ID2D1RenderTarget& target, const PanelLayout& layout,
                            const PanelState& state) {
    const Rect& bar = layout.busbar;
    fill_rounded(target, Rect{bar.x, bar.y - kWindowRadius, bar.width, bar.height + kWindowRadius},
                 kWindowRadius, with_alpha(kBgVoid, 0.55f));
    brush_->SetColor(kHairline);
    target.DrawLine(D2D1::Point2F(bar.x + 1.0f, bar.y + 0.5f),
                    D2D1::Point2F(bar.right() - 1.0f, bar.y + 0.5f), brush_.Get(), 1.0f);

    // 上一首 / 下一首**不在这里**：它们挪到了 BGM 台音波图下面那一行
    // （用户要求放那块空处）。底栏留给总线状态与曲库位置。


    // 底栏右侧：曲库位置。这里原来复用了搜索框的占位文字 ——
    // 底栏上冒出"搜索音乐…"是一眼就能看出的错，但那行代码单独看很合理。
    draw_text_ellipsis(target, *caption_format_.Get(), state.library_label,
                       layout.busbar_library, kText3, DWRITE_TEXT_ALIGNMENT_TRAILING);
    for (const auto& [id,area] : std::array<std::pair<int,Rect>,2>{{{6,layout.busbar_change},{5,layout.busbar_reload}}}) {
        fill_rounded(target,area,5,state.hovered_id==id?kSurface3:kSurface2);
        draw_text(target,*caption_format_.Get(),id==6?tr("更换曲库", "Change"):tr("重新加载", "Reload"),area,state.loading?kText3:kText2,DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    const auto show_all = layout.busbar_show_all;
    fill_rounded(target, show_all, 5, state.hovered_id == 10 ? kSurface3 : kSurface2);
    const Rect toggle{show_all.x + 9, show_all.y + 7, 24, 12};
    fill_rounded(target, toggle, 6, state.show_all_categories ? kAmber : kHairline);
    brush_->SetColor(state.show_all_categories ? kSurface1 : kText3);
    target.FillEllipse(D2D1::Ellipse({toggle.x + (state.show_all_categories ? 18.0f : 6.0f), toggle.y + 6}, 4, 4), brush_.Get());
    draw_text(target, *caption_format_.Get(), tr("显示全部", "Show All"),
        Rect{show_all.x + 40, show_all.y, show_all.width - 44, show_all.height},
        state.show_all_categories ? kText2 : kText3, DWRITE_TEXT_ALIGNMENT_CENTER);
    const auto fold=layout.busbar_fold;
    fill_rounded(target,fold,5,state.hovered_id==9?kSurface3:kSurface2);
    brush_->SetColor(kText2);
    const auto c=fold.center();
    target.DrawRectangle(D2D1::RectF(c.x-8,c.y-6,c.x+8,c.y+6),brush_.Get(),1);
    target.DrawLine({c.x+2,c.y-6},{c.x+2,c.y+6},brush_.Get(),1);
    const float direction=layout.sfx_collapsed?1.0f:-1.0f;
    target.DrawLine({c.x-3-direction*2,c.y-3},{c.x-3+direction*2,c.y},brush_.Get(),1.4f);
    target.DrawLine({c.x-3+direction*2,c.y},{c.x-3-direction*2,c.y+3},brush_.Get(),1.4f);
}

void PanelView::draw_column_head(ID2D1RenderTarget& target, const Rect& head, const Rect& volume,
                                 const Rect& meter, const Rect& clear_box, int clear_id,
                                 int hovered_id, bool has_content, const std::string& title,
                                 const std::string& tag, const D2D1_COLOR_F& accent,
                                 std::uint16_t volume_percent, float level) {
    draw_text(target, *section_format_.Get(), title,
              Rect{head.x, head.y, head.width * 0.5f, head.height}, accent);
    if (!tag.empty()) {
        // 按**字符数**估宽，不按字节数：中文一个字 3 字节，按字节会把"音乐"
        // 算成 81px（实际约 25px），角标就被推到很远的地方、还会被后面的
        // 清空按钮压住 —— 用户看到的是"清空按钮盖住了 BGM 字样"。
        const auto text_width = [](const std::string& text, float per_char) {
            float width = 0;
            for (const char ch : text) {
                const auto code = static_cast<unsigned char>(ch);
                if (code < 0x80) width += per_char * 0.52f;
                else if ((code & 0xC0) != 0x80) width += per_char;
            }
            return width;
        };
        const float title_width = text_width(title, 15.0f) + 8.0f;
        draw_text(target, *caption_format_.Get(), tag,
                  Rect{head.x + title_width, head.y,
                       std::max(0.0f, clear_box.x - head.x - title_width - 6.0f), head.height},
                  with_alpha(accent, 0.55f));
    }
    if (!volume.empty()) {
        // 与命中/拖动同源：真正可拖的那一段（右侧留出百分比文字）
        draw_volume_slider(target, PanelLayout::volume_track(volume), volume_percent, accent);
        draw_text(target, *caption_format_.Get(),
                  std::to_string(volume_percent) + "%",
                  Rect{volume.right() - 40.0f, volume.y, 40.0f, volume.height}, kText3,
                  DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    if (!meter.empty()) {
        draw_level_meter(target, meter.inset(4.0f), accent, level);
    }
    // "清空"：台上有东西时才出现。没有东西还画一个能点的按钮，
    // 用户点了没反应就会以为坏了。
    if (!clear_box.empty() && has_content) {
        const bool hovered = hovered_id == clear_id;
        fill_rounded(target, clear_box, 6.0f,
                     hovered ? with_alpha(kDanger, 0.85f) : with_alpha(kSurface2, 0.9f));
        stroke_rounded(target, clear_box, 6.0f,
                       hovered ? with_alpha(kDanger, 1.0f) : kHairline, 1.0f);
        // 一个叉：比"清空"两个字省地方，而且与标题栏那个关闭键同义
        const Point center = clear_box.center();
        const float arm = 4.0f;
        brush_->SetColor(hovered ? D2D1::ColorF(0xFFFFFF) : kText2);
        target.DrawLine(D2D1::Point2F(center.x - arm, center.y - arm),
                        D2D1::Point2F(center.x + arm, center.y + arm), brush_.Get(), 1.5f);
        target.DrawLine(D2D1::Point2F(center.x + arm, center.y - arm),
                        D2D1::Point2F(center.x - arm, center.y + arm), brush_.Get(), 1.5f);
    }
}

// ---------------------------------------------------------------------------
// 唱片台
// ---------------------------------------------------------------------------

void PanelView::draw_bgm_deck(ID2D1RenderTarget& target, const PanelLayout& layout,
                              const PanelState& state) {
    const DeckView& deck = state.bgm;
    const Rect& box = layout.bgm_deck;
    fill_rounded(target, box, kPanelRadius, with_alpha(kSurface1, 0.75f));
    stroke_rounded(target, box, kPanelRadius,
                   deck.drop_target ? with_alpha(kAmber, 0.9f) : kHairline, 1.0f);

    draw_disc(target, layout.bgm_disc, deck, kAmber, true, true, state.animation_time);

    // 唱针：播放时落在盘上，暂停/空闲时抬起
    {
        const Rect& arm = layout.bgm_tonearm;
        const Point pivot{arm.x + arm.width * 0.5f, arm.y + 6.0f};
        // 唱针：播放时落到盘上、暂停/空闲时抬起。
        // 过渡量 deck.tonearm 由 app 层按**状态变化**推进 ——
        // 原来这里用的是"从启动算起的秒数"，启动 0.25 秒后它就已经是 1 了，
        // 于是这段动画实际上从来没演过。
        const float blend = std::clamp(deck.tonearm, 0.0f, 1.0f);
        const float angle = 14.0f - 30.0f * blend;
        const float radians = angle * 3.14159265f / 180.0f;
        const float length = arm.height - 12.0f;
        const Point tip{pivot.x + std::sin(radians) * length, pivot.y + std::cos(radians) * length};
        brush_->SetColor(with_alpha(kText2, 0.85f));
        target.DrawLine(D2D1::Point2F(pivot.x, pivot.y), D2D1::Point2F(tip.x, tip.y), brush_.Get(),
                        3.0f);
        // 支点：底座圆盘 + 中心轴 + 一圈亮边（"唱臂装在轴上"的观感）
        brush_->SetColor(with_alpha(kBgVoid, 0.95f));
        target.FillEllipse(D2D1::Ellipse(to_d2d(pivot), 7.0f, 7.0f), brush_.Get());
        brush_->SetColor(with_alpha(kSurface3, 1.0f));
        target.FillEllipse(D2D1::Ellipse(to_d2d(pivot), 5.5f, 5.5f), brush_.Get());
        brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f));
        target.DrawEllipse(D2D1::Ellipse(to_d2d(pivot), 5.5f, 5.5f), brush_.Get(), 1.0f);
        // 针头：落下时贴盘，抬起时朝外
        brush_->SetColor(with_alpha(kText1, 0.85f));
        target.FillEllipse(D2D1::Ellipse(to_d2d(tip), 3.2f, 2.2f), brush_.Get());
    }

    // 曲名：卡片与这里都允许省略号，完整名字由悬停预览承担
    draw_text_ellipsis(target, *body_format_.Get(),
                       deck.has_track ? deck.name : tr("-- 空闲 --", "-- Idle --"), layout.bgm_deck_text,
                       deck.has_track ? kText1 : kText3);

    // 一行标签与开关。
    //
    // 矩形来自 layout.option_rects —— 命中测试用的是**同一个函数**。
    // 原来是这里自己算宽度、命中区那边没有对应物，于是按钮画得出来却点不着。
    {
        // 没有曲目时不排"分类 / 格式"两个标签 —— 它们只会显示成一个空框，
        // 而空框看起来就像"这里有个按钮但点不动"。
        std::vector<std::string> labels;
        if (deck.has_track) {
            labels.push_back(deck.category.empty() ? std::string(tr("未分类", "Uncategorized")) : deck.category);
            if (!deck.format.empty()) {
                labels.push_back(deck.format);
            }
        }
        labels.push_back(tr("循环", "Loop"));
        labels.push_back(tr("淡入淡出", "Fade"));
        labels.push_back(tr("混音", "Mix"));
        const std::vector<Rect> items = layout.option_rects(layout.bgm_deck_line, labels);
        for (std::size_t i = 0; i < items.size(); ++i) {
            // 从右往左数：最后三个才是开关（循环 / 淡入淡出 / 混音）
            const std::size_t from_end = items.size() - 1 - i;
            const bool is_toggle = from_end < 3;
            const bool on = (from_end == 2 && deck.looping) || (from_end == 1 && deck.fading) ||
                            (from_end == 0 && deck.mixing);
            const bool hovered = is_toggle && state.hovered_id == 106 - static_cast<int>(from_end);
            fill_rounded(target, items[i], items[i].height * 0.5f,
                         on ? with_alpha(kAmber, 0.18f)
                            : (hovered ? kSurface3 : with_alpha(kSurface1, 0.9f)));
            stroke_rounded(target, items[i], items[i].height * 0.5f,
                           on ? with_alpha(kAmber, 0.85f)
                              : (hovered ? kHoverBorder : kHairline),
                           on || hovered ? 1.2f : 1.0f);
            const auto ink = on ? kAmber : (hovered ? kText1 : kText2);
            if (is_toggle) {
                draw_text(target, *caption_format_.Get(), labels[i], items[i], ink, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                const Rect text_box{items[i].x+5,items[i].y,std::max(0.0f,items[i].width-10),items[i].height};
                draw_text_ellipsis(target, *caption_format_.Get(), labels[i], text_box, ink, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
    }

    // 音波图 + 时间
    const double progress = deck.duration_seconds > 0.0
                                ? deck.position_seconds / deck.duration_seconds
                                : 0.0;
    draw_text(target, *caption_format_.Get(), format_time(deck.position_seconds),
              layout.bgm_time_left, kText3, DWRITE_TEXT_ALIGNMENT_TRAILING);
    draw_text(target, *caption_format_.Get(), format_time(deck.duration_seconds),
              layout.bgm_time_right, kText3);
    draw_waveform(target, layout.bgm_waveform, deck.envelope, progress, kAmber, deck.playing);

    // 上一首 / 下一首：音波图下面那一行（原型把历史条放在台位里，
    // 用户也明确要求放这块空处）。按钮上写邻曲名 —— 名字是唯一的线索。
    {
        const auto draw_history = [&](const Rect& box, const std::string& name, bool enabled,
                                      bool forward, int hover_id) {
            const bool hovered = enabled && state.hovered_id == hover_id;
            fill_rounded(target, box, 6.0f, hovered ? kSurface3 : with_alpha(kSurface1, 0.9f));
            stroke_rounded(target, box, 6.0f, hovered ? kHoverBorder : kHairline, 1.0f);
            const float arrow_x = forward ? box.right() - 12.0f : box.x + 12.0f;
            const float center_y = box.center().y;
            const D2D1_COLOR_F ink = enabled ? (hovered ? kText1 : kText2)
                                            : with_alpha(kText3, 0.45f);
            brush_->SetColor(ink);
            const float dir = forward ? 1.0f : -1.0f;
            target.DrawLine(D2D1::Point2F(arrow_x - 3.0f * dir, center_y - 4.0f),
                            D2D1::Point2F(arrow_x + 3.0f * dir, center_y), brush_.Get(), 1.5f);
            target.DrawLine(D2D1::Point2F(arrow_x + 3.0f * dir, center_y),
                            D2D1::Point2F(arrow_x - 3.0f * dir, center_y + 4.0f), brush_.Get(), 1.5f);
            const float text_left = forward ? box.x + 10.0f : box.x + 22.0f;
            const float text_right = forward ? box.right() - 22.0f : box.right() - 10.0f;
            draw_text_ellipsis(target, *caption_format_.Get(),
                               enabled ? name : std::string("—"),
                               Rect{text_left, box.y, std::max(0.0f, text_right - text_left),
                                    box.height},
                               ink);
        };
        draw_history(layout.bgm_prev, state.history_prev, state.can_prev, false, 110);
        draw_history(layout.bgm_next, state.history_next, state.can_next, true, 111);
    }
}

void PanelView::draw_sfx_deck(ID2D1RenderTarget& target, const PanelLayout& layout,
                              const PanelState& state, std::size_t index) {
    const DeckView& deck = state.sfx[index];
    const Rect& box = layout.sfx_decks[index];
    fill_rounded(target, box, kCardRadius, with_alpha(kSurface1, 0.75f));
    stroke_rounded(target, box, kCardRadius,
                   deck.drop_target ? with_alpha(kTeal, 0.9f) : kHairline, 1.0f);

    // 左侧竖向按钮：循环 / 淡入淡出
    const auto side_button = [&](const Rect& area, const char* label, bool on) {
        fill_rounded(target, area, 5.0f,
                     on ? with_alpha(kTeal, 0.18f) : with_alpha(kSurface2, 0.9f));
        stroke_rounded(target, area, 5.0f, on ? with_alpha(kTeal, 0.7f) : kHairline, 1.0f);
        draw_text(target, *caption_format_.Get(), label, area, on ? kTeal : kText3,
                  DWRITE_TEXT_ALIGNMENT_CENTER);
    };
    side_button(layout.sfx_loop[index], tr("循", "L"), deck.looping);
    side_button(layout.sfx_fade[index], tr("淡", "F"), deck.fading);

    // dim_when_idle = false：音效暂停不压暗
    draw_disc(target, layout.sfx_discs[index], deck, kTeal, true, false, state.animation_time);

    const auto center = layout.sfx_discs[index].center();
    const float radius = PanelLayout::disc_radius(layout.sfx_discs[index]);
    const D2D1_POINT_2F pivot{center.x + radius*.82f, center.y - radius};
    const float angle = (14.0f - 40.0f*deck.tonearm) * 3.14159265f / 180.0f;
    const D2D1_POINT_2F tip{pivot.x + std::sin(angle)*radius*1.08f,
                           pivot.y + std::cos(angle)*radius*1.08f};
    brush_->SetColor(with_alpha(kText2,.7f));
    target.DrawLine(pivot,tip,brush_.Get(),1.6f);
    brush_->SetColor(kSurface3);
    target.FillEllipse(D2D1::Ellipse(pivot,3,3),brush_.Get());
    brush_->SetColor(kText2);
    target.FillEllipse(D2D1::Ellipse(tip,1.8f,1.8f),brush_.Get());

    draw_text_ellipsis(target, *caption_format_.Get(),
                       deck.has_track ? deck.name : tr("-- 空闲 --", "-- Idle --"), layout.sfx_names[index],
                       deck.has_track ? kText1 : kText3);

    const double progress = deck.duration_seconds > 0.0
                                ? deck.position_seconds / deck.duration_seconds
                                : 0.0;
    draw_waveform(target, layout.sfx_waveforms[index], deck.envelope, progress, kTeal,
                  deck.playing);

    draw_vertical_volume(target, layout.sfx_volumes[index], deck.volume_percent, kTeal);
}

// ---------------------------------------------------------------------------
// 曲库
// ---------------------------------------------------------------------------

namespace {

// 两色之间按 t 线性混合。悬停过渡要靠它 —— 悬停不再是"换一套颜色"，
// 而是"从常态颜色走 120ms 走到悬停颜色"。
[[nodiscard]] D2D1_COLOR_F mix_color(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    const float k = std::clamp(t, 0.0f, 1.0f);
    return D2D1::ColorF(a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k,
                        a.a + (b.a - a.a) * k);
}

// 缓出曲线 cubic-bezier(.22,1,.36,1) 的近似。
// 原型里所有过渡都用它；线性过渡在"上浮/显隐"这类位移上会显得发死。
[[nodiscard]] float ease_out(float t) {
    const float k = std::clamp(t, 0.0f, 1.0f);
    const float inv = 1.0f - k;
    return 1.0f - inv * inv * inv;
}

// 正在播放时的小均衡器：三根柱子按 700ms 一个周期起伏（与原型同节奏）。
// 相位从 animation_time 推，不用累加 —— 掉帧不会让它变快或变慢。
void draw_equalizer(ID2D1RenderTarget& target, ID2D1SolidColorBrush* brush, const Rect& area,
                    const D2D1_COLOR_F& color, float animation_time) {
    constexpr float kPeriod = 0.7f;
    const float base = animation_time / kPeriod;
    brush->SetColor(color);
    const float bar_width = 2.0f;
    const float gap = 2.0f;
    const float total = bar_width * 3.0f + gap * 2.0f;
    const float left = area.center().x - total * 0.5f;
    for (int i = 0; i < 3; ++i) {
        // 三根柱子错相 130ms，与原型一致
        const float phase = base - static_cast<float>(i) * 0.13f;
        const float wave = 0.5f - 0.5f * std::cos(phase * 6.2831853f);
        const float height = area.height * (0.35f + 0.65f * wave);
        const float x = left + static_cast<float>(i) * (bar_width + gap);
        const Rect bar{x, area.bottom() - height, bar_width, height};
        target.FillRectangle(D2D1::RectF(bar.x, bar.y, bar.right(), bar.bottom()), brush);
    }
}

}  // namespace

void PanelView::draw_tags(ID2D1RenderTarget& target,const Rect& area,const TrackView& track) {
    const auto duration=format_time(track.duration_seconds);
    const float duration_width=46.0f;
    draw_text(target,*caption_format_.Get(),duration,Rect{area.right()-duration_width,area.y,duration_width,area.height},kText3,DWRITE_TEXT_ALIGNMENT_TRAILING);
    float x=area.x;
    for(const auto& tag:track.tags) {
        float width=10;
        for(const unsigned char c:tag) if((c&0xC0)!=0x80) width+=c<128?6.0f:11.0f;
        width=std::min(width,area.right()-duration_width-6-x);
        if(width<20) break;
        const Rect box{x,area.y,std::min(width,100.0f),area.height};
        fill_rounded(target,box,3,with_alpha(kText2,.07f));
        draw_text_ellipsis(target,*caption_format_.Get(),tag,box.inset(2),kText2,DWRITE_TEXT_ALIGNMENT_CENTER);
        x+=box.width+4;
    }
}

void PanelView::draw_track_card(ID2D1RenderTarget& target, const Rect& card,
                                const TrackView& track, const D2D1_COLOR_F& accent, float hover,
                                float animation_time) {
    // 悬停是**渐变量**。原来是一步到位的布尔，屏幕上就是"颜色啪一下变了"；
    // 原型是 120ms 缓动，底色、边框、上浮、阴影一起走 —— 手感差的就是这一段。
    const float h = ease_out(std::clamp(hover, 0.0f, 1.0f));
    const bool hovered = h > 0.0f;
    // 上浮 1px（原型 translateY(-1px)）。命中区是按未位移的卡片算的，
    // 差 1px 在阈值之内，换来的是"卡片被拾起来"的感觉。
    const Rect box = card.offset(0.0f, -h);
    // 悬停要有**看得见**的变化。原来只是底色从 #1A1D24 抬到 #20242C（差 2%），
    // 屏幕上基本看不出来 —— 那等于没有悬停反馈。现在三处一起变：
    // 底色抬一档、边框提亮到 #3A414D、外面再加一圈总线色的淡光。
    const D2D1_COLOR_F background = (track.playing || track.selected)
                                        ? with_alpha(accent, 0.16f)
                                        : mix_color(kSurface1, kSurface3, h);
    const D2D1_COLOR_F border = (track.playing || track.selected)
                                    ? with_alpha(accent, 0.9f)
                                    : mix_color(kHairline, kHoverBorder, h);
    // 被拖动的那张卡：整张抬起来（外发光 + 加粗描边），让人知道搬的是它
    const bool lifting = track.selected;
    if (hovered || lifting) {
        fill_rounded(target, box.inset(-2.0f), kCardRadius + 2.0f,
                     with_alpha(accent, lifting ? 0.20f : 0.10f));
    }
    // 悬停阴影（原型 box-shadow: 0 6px 14px）。D2D 里没有便宜的软阴影，
    // 用三层递减透明度的圆角矩形叠一下，观感够用而不用引入位图效果链。
    if (h > 0.01f && !lifting) {
        for (int layer = 3; layer >= 1; --layer) {
            const float spread = static_cast<float>(layer) * 2.0f;
            const float alpha = 0.10f * h / static_cast<float>(layer);
            fill_rounded(target, box.offset(0.0f, 3.0f + spread * 0.5f).inset(-spread * 0.5f),
                         kCardRadius + spread * 0.5f, D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha));
        }
    }
    fill_rounded(target, box, kCardRadius, background);
    stroke_rounded(target, box, kCardRadius, border, lifting ? 2.0f : 1.0f);

    // 最左侧原来是一个格式徽章（MP3 / OGG / WAV）。用户明确说不需要 ——
    // 格式对"放哪首曲子"没有帮助，却占掉了卡片最显眼的位置。
    // 现在整条留给曲名与"分类 · 时长"。
    // 在播时用一小段总线色竖条代替原来那个徽章，状态还是看得见。

    // 在播的那张卡左侧一小段总线色竖条：原来那个"格式徽章"的位置还在被用到，
    // 只是承载的信息换成了真正有用的那一条。
    if (track.playing) {
        const Rect marker{box.x + 8.0f, box.y + 8.0f, 3.0f, box.height - 16.0f};
        fill_rounded(target, marker, 1.5f, with_alpha(accent, 0.95f));
    }

    // 曲名（单行省略号）。右侧留出抓手/均衡器的位置，免得长名字压在上面。
    const float tail = 18.0f;
    const float text_left = box.x + 14.0f;
    const Rect text_area{text_left, box.y + 7.0f,
                         std::max(0.0f, box.right() - text_left - tail),
                         box.height * 0.5f - 6.0f};
    draw_text_ellipsis(target, *body_format_.Get(), track.name, text_area,
                       track.playing ? accent : kText1);

    // 第二行：分类 · 时长（原型里的 track-meta 就是这个）
    const Rect meta_area{text_left, box.center().y + 1.0f,
                         std::max(0.0f, box.right() - text_left - tail),
                         box.height * 0.5f - 6.0f};
    draw_tags(target,meta_area,track);

    // 右上角：在响就画会跳的小均衡器，否则悬停时淡入"可以拖"的抓手。
    // 与原型一致 —— 正在播放的卡片不显示抓手（它已经不需要提示了，
    // 而两样东西挤在同一个角上会互相糊住）。
    const Rect corner{box.right() - 20.0f, box.center().y - 6.0f, 12.0f, 12.0f};
    if (track.playing) {
        draw_equalizer(target, brush_.Get(), corner, accent, animation_time);
    } else if (h > 0.02f) {
        const float grip_alpha = 0.55f * h;
        brush_->SetColor(D2D1::ColorF(kText2.r, kText2.g, kText2.b, grip_alpha));
        for (int row = 0; row < 3; ++row) {
            const float y = corner.y + static_cast<float>(row) * 4.0f;
            target.FillRectangle(D2D1::RectF(corner.x + 2.0f, y, corner.x + 4.0f, y + 2.0f),
                                 brush_.Get());
            target.FillRectangle(D2D1::RectF(corner.x + 7.0f, y, corner.x + 9.0f, y + 2.0f),
                                 brush_.Get());
        }
    }
}

void PanelView::draw_library(ID2D1RenderTarget& target, const PanelLayout& layout,
                             const PanelState& state_ref, const LibraryView& library,
                             const LibraryIds& ids, int hovered_id, const Rect& searchbox,
                             const Rect& icon, const Rect& input, const Rect& peek_text,
                             const Rect& peek_wave, const Rect& chips, const Rect& grid,
                             const D2D1_COLOR_F& accent, const std::vector<Rect>& chip_rects) {
    static_cast<void>(layout);
    // 搜索框：输入框与悬停预览是**同一个盒子**。
    // 于是"不悬停时搜索照常可用"是结构上保证的，不需要 z-index 或事件穿透去补。
    fill_rounded(target, searchbox, kCardRadius,
                 library.search_focused ? with_alpha(accent, 0.10f) : kSurface1);
    stroke_rounded(target, searchbox, kCardRadius,
                   library.search_focused || library.peek_active ? accent : kHairline, 1.0f);

    if (hovered_id == ids.search_input) {
        stroke_rounded(target, searchbox, kCardRadius, accent, 1.5f);
    }

    // 放大镜
    {
        const Point center{icon.x + icon.width * 0.5f - 1.0f, icon.center().y - 1.0f};
        brush_->SetColor(kText3);
        target.DrawEllipse(D2D1::Ellipse(to_d2d(center), 5.0f, 5.0f), brush_.Get(), 1.6f);
        target.DrawLine(D2D1::Point2F(center.x + 4.0f, center.y + 4.0f),
                        D2D1::Point2F(center.x + 8.0f, center.y + 8.0f), brush_.Get(), 1.6f);
    }

    const Rect field{input.x-3,input.y+7,input.width+2,input.height-14};
    fill_rounded(target,field,4,kBgVoid);
    stroke_rounded(target,field,4,library.search_focused?accent:kHoverBorder,1);
    const Rect text_box{field.x+7,field.y,field.width-14,field.height};
    if (library.search_text.empty()) {
        draw_text_ellipsis(target,*body_format_.Get(),library.search_placeholder,text_box,kText3);
    } else {
        draw_text_ellipsis(target,*body_format_.Get(),library.search_text,text_box,kText1);
    }
    if(library.search_focused) {
        const auto wide=to_wide(library.search_text);
        ComPtr<IDWriteTextLayout> caret_layout;
        float text_width=0;
        HRESULT caret_hr=write_->CreateTextLayout(wide.c_str(),static_cast<UINT32>(wide.size()),body_format_.Get(),100000.0f,text_box.height,caret_layout.GetAddressOf());
        DWRITE_TEXT_METRICS measured{};
        if (SUCCEEDED(caret_hr)) caret_hr=caret_layout->GetMetrics(&measured);
        if (FAILED(caret_hr)) {
            Failure failure; failure.set("ui.search_caret","Search text measurement failed hr="+std::to_string(caret_hr));
            report_vector_failure(failure);
        } else text_width=measured.widthIncludingTrailingWhitespace;
        if(text_width>text_box.width-3 && caret_layout) {
            target.PushAxisAlignedClip(D2D1::RectF(text_box.x,text_box.y,text_box.right(),text_box.bottom()),D2D1_ANTIALIAS_MODE_ALIASED);
            brush_->SetColor(kBgVoid); target.FillRectangle(D2D1::RectF(text_box.x,text_box.y,text_box.right(),text_box.bottom()),brush_.Get());
            brush_->SetColor(kText1); target.DrawTextLayout({text_box.right()-text_width-3,text_box.y},caret_layout.Get(),brush_.Get());
            target.PopAxisAlignedClip();
        }
        if (SUCCEEDED(caret_hr) && std::fmod(std::max(0.0,static_cast<double>(state_ref.animation_time)-library.search_edit_time),1.0)<.65) {
            const float x=text_box.x+std::min(text_width,text_box.width-3);
            brush_->SetColor(accent); target.DrawLine({x,text_box.y+5},{x,text_box.bottom()-5},brush_.Get(),1.4f);
        }
    }

    // 悬停预览区：一条竖线分隔，名字在前、波形在后。
    //
    // **不悬停时这里什么都不画**。原来没有悬停就退回画一次占位文字，
    // 于是搜索框里出现两遍"搜索音乐…" —— 看起来就像渲染出了 bug。
    // 这一块只承担"悬停时给你看的东西"，没有悬停就空着。
    if (library.peek_active) {
        brush_->SetColor(kHairline);
        target.DrawLine(D2D1::Point2F(peek_wave.x - 6.0f, searchbox.y + 8.0f),
                        D2D1::Point2F(peek_wave.x - 6.0f, searchbox.bottom() - 8.0f), brush_.Get(),
                        1.0f);
        draw_text_ellipsis(target, *caption_format_.Get(), library.peek_name,
                           Rect{peek_text.x, peek_text.y, peek_text.width, peek_text.height},
                           kText1);
    }
    if (library.peek_active && !library.peek_envelope.empty()) {
        // 预览里的波形从开头画，并用总线色标出"这张卡在库里是什么" ——
        // 名字在前、波形在后（与设计稿一致）。
        draw_waveform(target, peek_wave, library.peek_envelope, 0.0, accent, true);
    }

    // 分类芯片。**必须裁在容器里**：分类有二十来个时总宽远超一列，
    // 不裁的话它会一路画到隔壁列上。
    // 裁剪区要把**上下留白**也包进来：选中的高亮描边与悬停光晕都画在药丸之外，
    // 只裁到药丸边界就会把它们切掉一截（用户看到的正是"高亮上下被切了"）。
    const float pad = layout.chip_vertical_padding + 2.0f;
    target.PushAxisAlignedClip(
        D2D1::RectF(chips.x, chips.y - pad, chips.right(), chips.bottom() + pad),
        D2D1_ANTIALIAS_MODE_ALIASED);
    for (std::size_t i = 0; i < chip_rects.size(); ++i) {
        if (chip_rects[i].x > chips.right()) {
            break;
        }
        const bool on = static_cast<int>(i) == library.active_chip;
        const float chip_hover =
            i < library.chip_hover_amounts.size() ? ease_out(library.chip_hover_amounts[i]) : 0.0f;
        const bool hovered = chip_hover > 0.01f;
        const auto box=chip_rects[i];
        const bool first=i==0 || chip_rects[i-1].y!=box.y;
        const bool last=i+1==chip_rects.size() || chip_rects[i+1].y!=box.y;
        const float radius=7;
        const Rect shape{box.x-(first?0:radius),box.y,box.width+(first?0:radius)+(last?0:radius),box.height};
        target.PushAxisAlignedClip(D2D1::RectF(box.x,box.y-1,box.right(),box.bottom()+1),D2D1_ANTIALIAS_MODE_ALIASED);
        fill_rounded(target,shape,radius,on?with_alpha(accent,.20f):mix_color(kSurface1,kSurface3,chip_hover));
        stroke_rounded(target,shape,radius,on?with_alpha(accent,.9f):kHairline,1);
        target.PopAxisAlignedClip();
        if(!last) {brush_->SetColor(kHairline);target.DrawLine({box.right(),box.y+5},{box.right(),box.bottom()-5},brush_.Get(),1);}
        draw_text(target, *caption_format_.Get(), library.chips[i], chip_rects[i],
                  on ? accent : (hovered ? kText1 : kText2), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    target.PopAxisAlignedClip();

    // 卡片网格（裁在视口里，滚出去的不画）
    target.PushAxisAlignedClip(D2D1::RectF(grid.x, grid.y, grid.right(), grid.bottom()),
                               D2D1_ANTIALIAS_MODE_ALIASED);
    for (std::size_t i = 0; i < library.tracks.size(); ++i) {
        const Rect card = layout.card_rect(grid, i, library.grid_scroll);
        if (card.bottom() < grid.y - 1.0f || card.y > grid.bottom() + 1.0f) {
            continue;
        }
        // 过渡量由 app 层按时间推进；这里退一步用"是不是悬停项"兜底，
        // 这样即便上面没填（比如截图那条路）也不会失去悬停反馈。
        const float hover = i < library.hover_amounts.size()
                                ? library.hover_amounts[i]
                                : (static_cast<int>(i) == library.hovered_track ? 1.0f : 0.0f);
        if (library.pads) {
            const auto& track = library.tracks[i];
            fill_rounded(target,card,7,hover>.01f?kSurface3:kSurface1);
            if(track.playing) {
                target.PushAxisAlignedClip(D2D1::RectF(card.x,card.y,card.x+card.width*std::clamp(track.progress,0.0f,1.0f),card.bottom()),D2D1_ANTIALIAS_MODE_ALIASED);
                fill_rounded(target,card,7,with_alpha(accent,.25f));
                target.PopAxisAlignedClip();
            }
            stroke_rounded(target,card,7,track.playing?accent:kHairline,1);
            draw_text_ellipsis(target,*body_format_.Get(),track.name,Rect{card.x+9,card.y+6,card.width-18,24},kText1);
            draw_tags(target,Rect{card.x+9,card.y+35,card.width-18,18},track);
        } else draw_track_card(target, card, library.tracks[i], accent, hover, state_ref.animation_time);
    }
    if (library.tracks.empty()) {
        draw_text(target, *body_format_.Get(),
                  library.search_text.empty() ? tr("这一栏还是空的", "This library is empty") : tr("没有匹配的曲目", "No matching tracks"), grid, kText3,
                  DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    target.PopAxisAlignedClip();

    // 滚动条：内容比视口长的时候才画
    {
        const float content = layout.card_content_height(grid, library.tracks.size());
        if (content > grid.height && content > 0.0f) {
            const float ratio = grid.height / content;
            const float thumb_height = std::max(24.0f, grid.height * ratio);
            const float maximum_scroll = content - grid.height;
            const float t =
                maximum_scroll > 0.0f ? library.grid_scroll / maximum_scroll : 0.0f;
            const Rect thumb{grid.right() + 2.0f,
                             grid.y + (grid.height - thumb_height) *
                                          std::clamp(t, 0.0f, 1.0f),
                             layout.scrollbar_width - 4.0f, thumb_height};
            fill_rounded(target, thumb, thumb.width * 0.5f, with_alpha(kSurface3, 0.9f));
        }
    }
}

// ---------------------------------------------------------------------------
// 总入口
// ---------------------------------------------------------------------------

void PanelView::draw(ID2D1RenderTarget& target, const PanelLayout& layout,
                     const PanelState& state) {
    Failure brush_failure;
    if (!ensure_brush(target, brush_failure)) {
        // 一把画刷都建不出来就什么都画不了。留痕而不是静默画一片空白。
        ::OutputDebugStringA((tr("面板画刷创建失败: ", "Panel brush failed: ") + brush_failure.to_string() + "\n").c_str());
        return;
    }
    // 窗口底：一层从 #181B21 到 #131519 的竖向渐变。
    // D2D 的线性渐变画刷要每次重建，这里用两条矩形近似 —— 面板底色本身很暗，
    // 肉眼分不出渐变的中间过渡，而少一个每帧重建的对象。
    fill_rounded(target, layout.panel, kWindowRadius, kWindowBgTop);
    fill_rounded(target, Rect{layout.titlebar.bottom(), 0.0f, layout.panel.width,
                              layout.panel.height - layout.titlebar.bottom() - 200.0f},
                 kWindowRadius * 0.5f, with_alpha(kWindowBgBottom, 0.55f));
    stroke_rounded(target, layout.panel, kWindowRadius, with_alpha(kHairline, 0.9f), 1.0f);

    draw_titlebar(target, layout, state);
    draw_busbar(target, layout, state);

    // BGM 列（底纹微亮一点，两列在视觉上分开）
    fill_rounded(target, Rect{layout.bgm_column.x + 1.0f, layout.bgm_column.y + 1.0f,
                              layout.bgm_column.width - 2.0f, layout.bgm_column.height - 2.0f},
                 kPanelRadius, with_alpha(kSurface1, 0.22f));
    brush_->SetColor(kHairlineSoft);
    target.DrawLine(D2D1::Point2F(layout.bgm_column.right() - 0.5f, layout.bgm_column.y),
                    D2D1::Point2F(layout.bgm_column.right() - 0.5f, layout.bgm_column.bottom()),
                    brush_.Get(), 1.0f);

    draw_column_head(target, layout.bgm_head, layout.bgm_volume_slider, layout.bgm_meter,
                     layout.bgm_clear, 130, state.hovered_id, state.can_clear_bgm, tr("音乐", "Music"), "BGM",
                     kAmber, state.bgm_volume_percent, state.bgm_level);
    if (!layout.sfx_collapsed) {
    draw_column_head(target, layout.sfx_head, layout.sfx_volume_slider, layout.sfx_meter,
                     layout.sfx_clear, 131, state.hovered_id, state.can_clear_sfx, tr("音效", "Effects"), "SFX",
                     kTeal, state.sfx_volume_percent, state.sfx_level);

    for (int mode = 0; mode < 2; ++mode) {
        const Rect tab{layout.sfx_mode.x + static_cast<float>(mode) * 88.0f, layout.sfx_mode.y, 88.0f, layout.sfx_mode.height};
        const bool selected = state.dj_mode == (mode == 1);
        const Rect shape{tab.x-(mode==1?6.0f:0.0f),tab.y,tab.width+6.0f,tab.height};
        target.PushAxisAlignedClip(D2D1::RectF(tab.x,tab.y-1,tab.right(),tab.bottom()+1),D2D1_ANTIALIAS_MODE_ALIASED);
        fill_rounded(target, shape, 6.0f, selected ? with_alpha(kTeal, 0.16f) : kSurface1);
        stroke_rounded(target, shape, 6.0f, selected ? kTeal : kHairline, 1.0f);
        target.PopAxisAlignedClip();
        if (mode==0) {
            brush_->SetColor(kHairline);
            target.DrawLine({tab.right(),tab.y+1},{tab.right(),tab.bottom()-1},brush_.Get(),1.0f);
        }
        draw_text(target, *body_format_.Get(), mode == 0 ? tr("唱盘", "Decks") : tr("DJ 按钮", "DJ Pads"), tab,
            selected ? kTeal : kText2, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    draw_text(target, *caption_format_.Get(), state.dj_mode ? tr("点按播放 · 右键停止", "Click to play · Right-click to stop") : tr("拖入分配 · 独立控制", "Drag to assign · Independent controls"),
        Rect{layout.sfx_mode.right() + 12.0f, layout.sfx_mode.y, 260.0f, layout.sfx_mode.height}, kText3);
    }
    draw_bgm_deck(target, layout, state);
    for (std::size_t i = 0; !layout.sfx_collapsed && !state.dj_mode && i < sfx_slot_count; ++i) {
        draw_sfx_deck(target, layout, state, i);
    }

    const LibraryIds bgm_ids{300, 400, 1000};
    const LibraryIds sfx_ids{301, 500, 2000};
    draw_library(target, layout, state, state.bgm_library, bgm_ids, state.hovered_id,
                 layout.bgm_searchbox, layout.bgm_search_icon, layout.bgm_search_input,
                 layout.bgm_peek_text, layout.bgm_peek_wave, layout.bgm_chips, layout.bgm_grid,
                 kAmber, layout.chip_rects(layout.bgm_chips, state.bgm_library.chip_widths));
    if (!layout.sfx_collapsed) draw_library(target, layout, state, state.sfx_library, sfx_ids, state.hovered_id,
                 layout.sfx_searchbox, layout.sfx_search_icon, layout.sfx_search_input,
                 layout.sfx_peek_text, layout.sfx_peek_wave, layout.sfx_chips, layout.sfx_grid,
                 kTeal, layout.chip_rects(layout.sfx_chips, state.sfx_library.chip_widths));

    if (state.loading) {
        fill_rounded(target, layout.panel, kWindowRadius, D2D1::ColorF(0.03f, 0.04f, 0.05f, 0.94f));
        const Rect box{layout.panel.width * 0.5f - 260.0f, layout.panel.height * 0.5f - 126.0f, 520.0f, 250.0f};
        fill_rounded(target, box, 14.0f, kSurface1);
        stroke_rounded(target, box, 14.0f, kHairline, 1.0f);
        draw_text(target, *title_format_.Get(), state.load_title,
            Rect{box.x + 28.0f, box.y + 22.0f, box.width - 56.0f, 34.0f}, kText1);
        draw_text_ellipsis(target, *body_format_.Get(), state.load_current,
            Rect{box.x + 28.0f, box.y + 70.0f, box.width - 56.0f, 28.0f}, kText2);
        const Rect rail{box.x + 28.0f, box.y + 113.0f, box.width - 56.0f, 8.0f};
        fill_rounded(target, rail, 4.0f, kSurface3);
        fill_rounded(target, Rect{rail.x, rail.y, rail.width * static_cast<float>(state.load_fraction), rail.height}, 4.0f, kTeal);
        draw_text(target, *caption_format_.Get(), state.load_detail + "  ·  " +
            std::to_string(static_cast<int>(state.load_fraction * 100.0)) + "%",
            Rect{box.x + 28.0f, box.y + 137.0f, box.width - 56.0f, 25.0f}, kText2);
        const Rect cancel{layout.panel.width * 0.5f - 45.0f, layout.panel.height * 0.5f + 76.0f, 90.0f, 32.0f};
        fill_rounded(target, cancel, 6.0f, state.hovered_id == 8 ? kSurface3 : kSurface2);
        draw_text(target, *body_format_.Get(), tr("取消加载", "Cancel"), cancel, kText1, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // 拖动中的幽灵：**就是被拖的那张卡**，从抓取点起跟着光标。
    //
    // 原来是光标右下角的一个 30px 小标签 —— 用户看到的和"拎起了一张卡"
    // 完全不是一回事。现在按卡片的实际尺寸与配色画，并且保持按下时的相对位置，
    // 手感才是"这张卡在我手里"。
    if (state.dragging && state.drag_origin_valid) {
        const bool valid_drop = state.drop_target_id >= 0;
        const float ghost_w = state.drag_size.x > 1.0f ? state.drag_size.x
                                                       : layout.track_card_width;
        const float ghost_h = state.drag_size.y > 1.0f ? state.drag_size.y
                                                       : layout.track_card_height;
        const Rect ghost{state.drag_origin.x, state.drag_origin.y, ghost_w, ghost_h};
        // 落点有效时给一圈总线色描边：松手之前就能看出"这一下能不能放"
        const D2D1_COLOR_F tint = valid_drop ? kTeal : kHoverBorder;
        // 底下垫一层阴影，让它看起来是浮在面板之上
        for (int layer = 3; layer >= 1; --layer) {
            const float spread = static_cast<float>(layer) * 3.0f;
            fill_rounded(target, ghost.offset(0.0f, 4.0f + spread * 0.6f).inset(-spread * 0.5f),
                         kCardRadius + spread * 0.5f,
                         D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f / static_cast<float>(layer)));
        }
        draw_track_card(target, ghost, state.drag_track,
                        state.drag_track.playing ? kAmber : kTeal, 0.0f, state.animation_time);
        stroke_rounded(target, ghost.inset(-1.0f), kCardRadius + 1.0f,
                       with_alpha(tint, valid_drop ? 0.95f : 0.6f), valid_drop ? 2.0f : 1.0f);
    }
}

}  // namespace musicboard::ui
