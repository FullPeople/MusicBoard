#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/failure.h"

namespace musicboard::ui {

// ============================================================================
// 界面交互内核
//
// 这一层不碰 D2D、不碰窗口，只回答三个问题：
//   1. 这个点在哪个控件里？（命中测试）
//   2. 现在是不是在拖？拖的是谁？
//   3. 曲库该滚到哪里？（滚动状态）
//
// 为什么值得单独一层：原型里最容易出错、也最难靠"看一眼"发现的就是这些 ——
// 悬停有特效但点不动、拖到一半丢掉了拖动对象、滚到底之后内容还在往上跑。
// 抽成纯逻辑之后它们可以被穷举测，而不是靠真的去点。
//
// 坐标一律是**逻辑坐标**（96 DPI）。窗口负责物理像素的缩放（见 TransparentWindow）。
// ============================================================================

struct Point final {
    float x = 0.0f;
    float y = 0.0f;
};

struct Rect final {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    [[nodiscard]] float right() const noexcept { return x + width; }
    [[nodiscard]] float bottom() const noexcept { return y + height; }
    [[nodiscard]] Point center() const noexcept {
        return Point{x + width * 0.5f, y + height * 0.5f};
    }
    [[nodiscard]] bool empty() const noexcept { return width <= 0.0f || height <= 0.0f; }

    // 命中的边界算命中（半开还是闭区间这种事，不说清就会出现"共边时点到谁"的随机性）
    [[nodiscard]] bool contains(Point point) const noexcept {
        return point.x >= x && point.x < x + width && point.y >= y && point.y < y + height;
    }

    [[nodiscard]] Rect inset(float amount) const noexcept {
        return Rect{x + amount, y + amount, width - amount * 2.0f, height - amount * 2.0f};
    }
    [[nodiscard]] Rect offset(float dx, float dy) const noexcept {
        return Rect{x + dx, y + dy, width, height};
    }
    // 与另一块的交叉区域（不相交时返回一个空矩形）
    [[nodiscard]] Rect intersect(const Rect& other) const noexcept {
        const float left = x > other.x ? x : other.x;
        const float top = y > other.y ? y : other.y;
        const float right_edge = right() < other.right() ? right() : other.right();
        const float bottom_edge = bottom() < other.bottom() ? bottom() : other.bottom();
        if (right_edge <= left || bottom_edge <= top) {
            return Rect{};
        }
        return Rect{left, top, right_edge - left, bottom_edge - top};
    }
};

// 圆：唱片与悬浮球都是圆的，命中测试也必须按圆来 ——
// 方框命中会让四个角上多出"看着是空的、点了却有反应"的区域。
[[nodiscard]] inline bool circle_hit(Point center, float radius, Point point) noexcept {
    const float dx = point.x - center.x;
    const float dy = point.y - center.y;
    return dx * dx + dy * dy <= radius * radius;
}

// ---------------------------------------------------------------------------
// 命中测试
//
// 用法：每一帧分两趟
//   1. begin_frame() + 喂输入状态
//   2. **布局趟**：把所有控件按绘制顺序 add() 进来
//   3. **绘制趟**：边画边用 hovered()/clicked() 问"现在谁被指着"
//
// 两趟是必要的。如果边画边 add、边画边问，那么"我后面还有没有更上层的控件"
// 这个问题在问的那一刻还没有答案 —— 于是底下那层会先把自己画成高亮，
// 上面那层再覆盖掉，看起来就是"悬停时闪了一下"。
//
// 规则三条，都是为了让"看到的"和"点得到的"一致：
//   · **后加的画在上面，也先被点到。** 绘制顺序就是层次顺序，不需要再维护一份 z 值。
//   · **同一个 group 里只有最上面那个算命中。** 悬停时给哪块高亮必须唯一，
//     否则卡片叠在一起时会两块同时亮。
//   · **capture 一设，别的区域全部让路。** 拖动中的对象必须一路收到 mouse_up，
//     否则手一滑出卡片，拖动就断了。
// ---------------------------------------------------------------------------

struct HitRegion final {
    Rect bounds;
    // 调用方给的稳定标识（曲目下标、台位号之类）。-1 表示"这块不接受命中"。
    int id = -1;
    // 互斥组：同一组里只有最上面那个算悬停。0 表示不参与互斥。
    int group = 0;
    bool enabled = true;
    // 明确要求"圆内才算命中"时的圆心与半径（radius <= 0 表示按矩形算）
    Point circle_center{};
    float circle_radius = 0.0f;
};

class HitMap final {
public:
    void begin_frame() noexcept {
        regions_.clear();
        pressed_ = false;
        released_ = false;
        // 注意：**不**清 pressed_target_cache_。按住不放的那几帧要继续认同一个对象。
        // 它只在 down_ 变化时失效（见 set_left_down / set_left_pressed）。
    }

    void add(const HitRegion& region) { regions_.push_back(region); }

    // 本帧的鼠标状态。逻辑坐标。
    void set_cursor(Point cursor) noexcept { cursor_ = cursor; }
    void set_left_pressed(bool pressed) noexcept {
        pressed_ = pressed;
        if (pressed) {
            down_ = true;
            pressed_target_resolved_ = false;
        }
    }
    void set_left_released(bool released) noexcept { released_ = released; }
    void set_left_down(bool down) noexcept {
        if (down_ != down) {
            down_ = down;
            pressed_target_resolved_ = false;
        }
    }

    [[nodiscard]] Point cursor() const noexcept { return cursor_; }
    [[nodiscard]] bool left_down() const noexcept { return down_; }

    // 顶层的命中（不考虑互斥组）。没有命中时返回 -1。
    [[nodiscard]] int topmost_at(Point point) const noexcept;

    // 当前悬停的 id：先看 capture，再看命中。
    [[nodiscard]] int hovered() const noexcept;
    // 指定 group 里悬停的 id（0 表示不按组过滤）
    [[nodiscard]] int hovered_in(int group) const noexcept;
    // 本帧是否在其上按下了左键
    [[nodiscard]] bool clicked(int id) const noexcept;
    // 本帧是否在其上松开了左键（只有没被拖动打断时才算）
    [[nodiscard]] bool released_on(int id) const noexcept;
    // 按下时锁定的那个 id。按住不放期间悬停一直算它 ——
    // 否则按下之后小幅移动鼠标，高亮会跳到旁边那块，看起来像点错了。
    [[nodiscard]] int pressed_target() const noexcept;

    // 拖动：开始之后，hovered() 与 clicked() 都只认它，直到 release_capture()
    void capture(int id) noexcept { captured_ = id; }
    void release_capture() noexcept { captured_ = -1; }
    [[nodiscard]] int captured() const noexcept { return captured_; }
    [[nodiscard]] bool is_captured() const noexcept { return captured_ >= 0; }

    // 拖动中的光标位置（capture 期间由调用方持续更新）
    void set_drag_position(Point point) noexcept { drag_position_ = point; }
    [[nodiscard]] Point drag_position() const noexcept { return drag_position_; }
    // 从 capture 开始点到现在的位移
    [[nodiscard]] Point drag_delta() const noexcept {
        return Point{drag_position_.x - capture_origin_.x, drag_position_.y - capture_origin_.y};
    }
    void set_capture_origin(Point point) noexcept { capture_origin_ = point; }
    [[nodiscard]] Point capture_origin() const noexcept { return capture_origin_; }

    [[nodiscard]] std::size_t region_count() const noexcept { return regions_.size(); }

private:
    // 某个区域是否真的能接住这个点（矩形 + 可选的圆形约束）
    [[nodiscard]] bool region_accepts(const HitRegion& region, Point point) const noexcept;

    std::vector<HitRegion> regions_;
    // 按下时命中的是谁。**惰性求值**：按下那一帧 regions_ 已经装满了，
    // 但求值的时机由第一次查询决定，所以调用方不必关心顺序。
    mutable int pressed_target_cache_ = -1;
    mutable bool pressed_target_resolved_ = false;
    Point cursor_{};
    Point drag_position_{};
    Point capture_origin_{};
    int captured_ = -1;
    bool down_ = false;
    bool pressed_ = false;
    bool released_ = false;
};

// ---------------------------------------------------------------------------
// 滚动状态
//
// 单独抽出来是因为"滚过头"和"内容比视口短时还能滚"是两件很容易漏的事，
// 而它们都只看几个数就能判定。
// ---------------------------------------------------------------------------
class ScrollState final {
public:
    // 内容比视口短时，最大偏移是 0（不是负数）——
    // 允许负的最大偏移会让内容在没有必要时也能往上跑。
    [[nodiscard]] float maximum_offset() const noexcept;

    void set_view_height(float height) noexcept { view_height_ = height; }
    void set_content_height(float height) noexcept { content_height_ = height; }
    void set_offset(float offset) noexcept;
    [[nodiscard]] float offset() const noexcept { return offset_; }
    [[nodiscard]] float view_height() const noexcept { return view_height_; }
    [[nodiscard]] float content_height() const noexcept { return content_height_; }
    [[nodiscard]] bool scrollable() const noexcept { return maximum_offset() > 0.0f; }

    // 滚轮。正数向下。返回值表示"偏移是否真的变了" ——
    // 已经到头还继续滚时返回 false，界面就不该重绘。
    [[nodiscard]] bool scroll_by(float delta) noexcept;

    // 把某一行滚进视野（用于"上一首/下一首"自动定位到当前曲目）
    [[nodiscard]] bool scroll_into_view(float item_top, float item_height) noexcept;

private:
    float offset_ = 0.0f;
    float view_height_ = 0.0f;
    float content_height_ = 0.0f;
};

}  // namespace musicboard::ui
