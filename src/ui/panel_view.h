#pragma once
#include <unordered_map>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include "core/failure.h"
#include "core/product_settings.h"
#include "ui/cover_cache.h"
#include "ui/panel_layout.h"

namespace musicboard::ui {

// spike 与既有代码都用这个写法：写全 Microsoft::WRL::ComPtr 太吵
using Microsoft::WRL::ComPtr;

// ============================================================================
// 面板绘制
//
// 这一层只做一件事：把 PanelState 按 PanelLayout 画出来。它不决定任何行为 ——
// 状态怎么变、点了谁、拖到哪里，全部由调用方（app 层）拿着 HitMap 决定。
//
// 这样切的理由：绘制是最难自动验证的一层（只能靠人看图），
// 而行为是最该被测的一层。把它们分开之后，"点击唱片暂停""拖到台上播放"
// 这些可以脱离像素去测，绘制也可以脱离行为去改。
//
// 关于音波图的进度：**用遮罩，不用进度线**（这是用户在原型阶段明确要求的）。
// 未播部分是常态灰，播过的部分是总线色。实现是同一份波形画两遍 ——
// 先整条画灰，再用 PushAxisAlignedClip 裁出已播的那一段画成总线色。
// 试过"一根橙色的竖线"，那读起来像播放头，不像进度。
// ============================================================================

// 曲目卡上要显示的东西
struct TrackView final {
    std::string name;
    std::string category;
    std::string badge;              // "MP3" / "OGG" / "WAV"
    double duration_seconds = 0.0;
    bool playing = false;
    bool selected = false;
    std::vector<std::string> tags;
    float progress = 0.0f;
    std::filesystem::path path;     // 封面按它查
};

// 一个唱片台的状态
struct DeckView final {
    bool has_track = false;
    std::string name;
    std::string category;
    std::string format;
    double duration_seconds = 0.0;
    double position_seconds = 0.0;
    bool playing = false;
    bool looping = true;
    bool fading = false;
    bool mixing = false;
    std::uint16_t volume_percent = 100;
    float level = 0.0f;                    // 电平表 0..1
    std::vector<float> envelope;           // 音波图（空则画一条平线）
    std::filesystem::path path;            // 封面按它查
    // 拖动中的轨道落点高亮
    bool drop_target = false;
    // 唱针过渡量：0 = 抬起（空闲/暂停），1 = 落在盘上（播放中）。
    // 由 app 层按时间推进 —— 绘制层只拿它插值角度，不自己算时间。
    float tonearm = 0.0f;
    float angle_degrees = 0.0f;
    float hover = 0.0f;
};

// 一列曲库的全部状态。
//
// 左列的"音乐"与右列的"音效"是**两份独立的列表**：各自有分类、搜索框、滚动位置、
// 悬停预览。原来只有一份，于是两列画的是同一批曲目、悬停也是同一个 ——
// 音效列里显示的其实是音乐。
struct LibraryView final {
    bool pads = false;
    std::vector<std::string> chips;
    std::vector<float> chip_widths;
    int active_chip = -1;
    std::vector<TrackView> tracks;
    float grid_scroll = 0.0f;
    std::string search_text;
    std::string search_placeholder;
    bool search_focused = false;
    double search_edit_time = 0;
    bool peek_active = false;
    std::string peek_name;
    std::vector<float> peek_envelope;
    int hovered_track = -1;
    int hovered_chip = -1;

    // 悬停过渡量（0..1），每张卡 / 每枚芯片一个。
    //
    // 原型里悬停是 120ms 的缓动过渡（底色、边框、上浮、阴影一起变）。
    // 之前这里是一步到位 —— 鼠标一动颜色瞬间跳过去，"差得远"主要就差在这儿：
    // 没有过渡，鼠标效果看起来就是"硬"的。
    // 由 app 层按时间推进（见 Application 里的 hover 动画），绘制层只读。
    std::vector<float> hover_amounts;
    std::vector<float> chip_hover_amounts;
};

struct PanelState final {
    bool dj_mode = false;
    bool english = false;
    bool show_all_categories = false;
    bool loading = false;
    double load_fraction = 0.0;
    std::string load_title, load_current, load_detail;
    // 标题栏
    std::string title;
    std::string status_chip;               // 标题栏中间那枚胶囊
    std::string library_label;             // 底栏右侧：曲库路径或一句提示
    bool always_on_top = true;

    DeckView bgm;
    std::array<DeckView, sfx_slot_count> sfx;
    // 每台音效的快捷键角标（"Ctrl+Alt+1"）。空则不画。
    std::array<std::string, sfx_slot_count> sfx_hotkeys{};

    std::uint16_t bgm_volume_percent = 100;
    std::uint16_t sfx_volume_percent = 100;
    float bgm_level = 0.0f;
    float sfx_level = 0.0f;

    // 曲库：左列是音乐、右列是音效
    LibraryView bgm_library;
    LibraryView sfx_library;

    // 底栏的历史条：上一首 / 下一首的曲名（空 = 没有，按钮不可点）
    std::string history_prev;
    std::string history_next;
    bool can_prev = false;
    bool can_next = false;

    // 拖动中的幽灵：**就是那张卡片**，从抓取点起跟着光标走。
    // 原来只有一个 30px 高的小标签，而且起点固定在光标右下角 ——
    // 用户要的是"看起来像真的把这张卡拎起来了"。
    ui::TrackView drag_track;
    Point drag_origin{};        // 幽灵左上角（逻辑坐标）
    Point drag_size{};          // 幽灵尺寸（= 被拖卡片的尺寸）
    bool drag_origin_valid = false;

    // 清空按钮是否可点（台上有东西时才画出来）
    bool can_clear_bgm = false;
    bool can_clear_sfx = false;

    // 拖动中
    bool dragging = false;
    std::string drag_label;
    Point drag_cursor{};

    // 当前命中的区域 id（与 app 里 HitMap 的 id 同一套编号）。
    // 有了它，绘制层不必自己猜"鼠标在哪个部件上"。
    int hovered_id = -1;
    // 拖动中的落点区域 id（-1 = 没落在任何可放置的地方）。
    // 三种状态要分得开：没在拖、在拖但落点无效、在拖且落点有效。
    int drop_target_id = -1;

    // 动画用的时间（秒）。**从启动那一刻算起的绝对值**，不是每帧累加 ——
    // 累加的话掉帧会让动画变慢，而"该转到哪个角度"本该只由时间决定。
    float animation_time = 0.0f;
    // 播放进度（秒）也参与动画（唱片角度由它决定，暂停就停住）
    float bgm_position = 0.0f;

    // 面板显隐动效进度（0 = 完全收起，1 = 完全展开）。
    // 原理图里是 scale(.93) translateY(10px) → scale(1) translateY(0)，
    // 420ms ease-out。绘制层照这个把整块内容变换一下即可。
    float reveal = 1.0f;
};

// 绘制资源。字体与常用画刷建一次，之后复用 ——
// 每帧重建 DirectWrite 对象是这个面板最容易被忽略的性能坑。
class PanelView final {
public:
    PanelView() = default;
    ~PanelView();

    PanelView(const PanelView&) = delete;
    PanelView& operator=(const PanelView&) = delete;

    // 设备上下文由窗口持有，必须比 PanelView 活得久。
    [[nodiscard]] bool initialize(ID2D1RenderTarget& target, ID2D1DeviceContext* device_context,
                                  Failure& failure);

    // 量出每个分类芯片的实际宽度，填进 library.chip_widths。
    // 调用方在"填好 chips 之后、算命中区域之前"调一次即可 ——
    // 芯片只在这两者变化时才变，不必每帧量。结果按名字缓存。
    void measure_chips(LibraryView& library);

    // 不收 DrawContext：布局已经带了全部几何，绘制不需要知道窗口的事。
    // UI 层不该依赖 platform（那是分层反向）。
    void draw(ID2D1RenderTarget& target, const PanelLayout& layout, const PanelState& state);

    [[nodiscard]] const CoverCache& covers() const noexcept { return covers_; }
    [[nodiscard]] CoverCache& covers() noexcept { return covers_; }

    // 封面位图能不能画到这张 target 上。
    //
    // 封面是**设备相关资源**：从某个设备上下文建的位图不能画到别的 target 上。
    // 违反它 D2D 不返回错误，直接 abort —— 排查过一次（截图那条路就是软件 target）。
    // 所以规则是"只认建它时那个设备上下文"，别的一律跳过封面、回退黑胶。
    [[nodiscard]] bool target_supports_covers(ID2D1RenderTarget& target) const noexcept;

private:
    void draw_tags(ID2D1RenderTarget& target, const Rect& area, const TrackView& track);
    // 各块的绘制。拆开是为了每块都能单独看、单独改。
    void draw_titlebar(ID2D1RenderTarget& target, const PanelLayout& layout,
                       const PanelState& state);
    void draw_busbar(ID2D1RenderTarget& target, const PanelLayout& layout,
                     const PanelState& state);
    void draw_column_head(ID2D1RenderTarget& target, const Rect& head, const Rect& volume,
                          const Rect& meter, const Rect& clear_box, int clear_id,
                          int hovered_id, bool has_content, const std::string& title, const std::string& tag,
                          const D2D1_COLOR_F& accent, std::uint16_t volume_percent, float level);
    void draw_bgm_deck(ID2D1RenderTarget& target, const PanelLayout& layout,
                       const PanelState& state);
    void draw_sfx_deck(ID2D1RenderTarget& target, const PanelLayout& layout,
                       const PanelState& state, std::size_t index);
    // 画一列曲库。ids 是这一列在 HitMap 里的编号段，用来判断"悬停的是我这边的哪个部件"，
    // 两列必须给不同的编号段，否则悬停会串到隔壁列。
    struct LibraryIds final {
        int search_input = 0;
        int chip_base = 0;
        int card_base = 0;
    };
    void draw_library(ID2D1RenderTarget& target, const PanelLayout& layout, const PanelState& state,
                      const LibraryView& library, const LibraryIds& ids, int hovered_id,
                      const Rect& searchbox, const Rect& icon, const Rect& input,
                      const Rect& peek_text, const Rect& peek_wave, const Rect& chips,
                      const Rect& grid, const D2D1_COLOR_F& accent,
                      const std::vector<Rect>& chip_rects);
    // 唱片：有封面就是封面，没有就是黑胶。中心不镂空（球与盘的区别）
    // dim_when_idle：暂停/空闲时是否压暗。
    // 只有 BGM 那一台要 —— 音效被暂停是零点几秒的事，压暗会被读成"坏掉了"
    // （用户明确要求过；上一轮我拿 show_center_dot 代替它，而两台都传 true，等于没改）。
    void draw_disc(ID2D1RenderTarget& target, const Rect& disc, const DeckView& deck,
                   const D2D1_COLOR_F& accent, bool show_center_dot, bool dim_when_idle,
                   float animation_time);
    // 音波图 + 进度遮罩
    void draw_waveform(ID2D1RenderTarget& target, const Rect& area,
                       const std::vector<float>& envelope, double progress,
                       const D2D1_COLOR_F& accent, bool playing);
    void draw_level_meter(ID2D1RenderTarget& target, const Rect& area, const D2D1_COLOR_F& accent,
                          float level);
    void draw_volume_slider(ID2D1RenderTarget& target, const Rect& area,
                            std::uint16_t volume_percent, const D2D1_COLOR_F& accent);
    void draw_vertical_volume(ID2D1RenderTarget& target, const Rect& area,
                              std::uint16_t volume_percent, const D2D1_COLOR_F& accent);
    // hover 是**过渡量**（0..1）而不是布尔：悬停是渐变的，
    // 传布尔就只能一步跳过去，那正是"鼠标效果硬"的来源。
    void draw_track_card(ID2D1RenderTarget& target, const Rect& card, const TrackView& track,
                         const D2D1_COLOR_F& accent, float hover, float animation_time);

    // 小工具
    void fill_rounded(ID2D1RenderTarget& target, const Rect& rect, float radius,
                      const D2D1_COLOR_F& color);
    void stroke_rounded(ID2D1RenderTarget& target, const Rect& rect, float radius,
                        const D2D1_COLOR_F& color, float width);
    void draw_text(ID2D1RenderTarget& target, IDWriteTextFormat& format, const std::string& text,
                   const Rect& rect, const D2D1_COLOR_F& color,
                   DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING);
    // 放不下就用省略号（曲名卡片必须这样，否则长名字会溢出去）
    void draw_text_ellipsis(ID2D1RenderTarget& target, IDWriteTextFormat& format,
                            const std::string& text, const Rect& rect, const D2D1_COLOR_F& color,
                            DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING);

    // 画刷是**目标相关**资源：在 A 上建的画刷画到 B 上，D2D 不返回错误、
    // 直接 abort。所以按 target 缓存一把 —— 屏幕上一直是同一个 target，
    // 截图那条路会换成软件 target，那时必须重建。踩过两次同类问题
    // （另一次是封面位图），所以这条规则在这里写在变量旁边。
    [[nodiscard]] bool ensure_brush(ID2D1RenderTarget& target, Failure& failure);

    // 圆形遮罩几何（唱片封面的圆形裁剪要用）。
    //
    // D2D 里圆裁剪只有 PushLayer + 几何遮罩这一条路 —— 矩形 clip 做不到圆，
    // 而"封面是个转动的方块"就是这么来的。几何同样是**目标相关**资源
    // （画到别的 target 上不报错、直接 abort），所以和画刷一样按 target 缓存。
    [[nodiscard]] ID2D1EllipseGeometry* circular_mask(ID2D1RenderTarget& target,
                                                      const Point& center, float radius);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    ID2D1RenderTarget* brush_target_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1EllipseGeometry> mask_geometry_;
    ID2D1RenderTarget* mask_target_ = nullptr;
    float mask_radius_ = -1.0f;
    Point mask_center_{};
    Microsoft::WRL::ComPtr<IDWriteFactory> write_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> title_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> section_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> body_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> caption_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> badge_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> centered_format_;
    std::unordered_map<IDWriteTextFormat*, Microsoft::WRL::ComPtr<IDWriteInlineObject>> trimming_signs_;
    CoverCache covers_;
    // create 时记下来的设备上下文，用来判断"这张 target 能不能画封面"
    ID2D1DeviceContext* device_context_ = nullptr;
    // 芯片名字 → 宽度。只在名字第一次出现时量一次。
    std::vector<std::pair<std::string, float>> chip_width_cache_;
};

}  // namespace musicboard::ui
