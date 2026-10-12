#include "ui/ui_core.h"

#include <algorithm>

namespace musicboard::ui {

bool HitMap::region_accepts(const HitRegion& region, Point point) const noexcept {
    if (region.id < 0 || !region.enabled) {
        return false;
    }
    if (region.circle_radius > 0.0f) {
        // 圆形的区域先按外接矩形粗筛，再按圆精判 ——
        // 唱片与悬浮球是圆的，方块命中会让四个角上多出"看着是空的、点了却有反应"的地方。
        if (!region.bounds.contains(point)) {
            return false;
        }
        return circle_hit(region.circle_center, region.circle_radius, point);
    }
    return region.bounds.contains(point);
}

int HitMap::topmost_at(Point point) const noexcept {
    // 从后往前找：**后加的画在上面，也先被点到**。
    // 这样"看到的层次"和"点得到的层次"天然一致，不需要另外维护 z 值。
    for (auto it = regions_.rbegin(); it != regions_.rend(); ++it) {
        if (region_accepts(*it, point)) {
            return it->id;
        }
    }
    return -1;
}

int HitMap::pressed_target() const noexcept {
    if (!down_) {
        return -1;
    }
    if (!pressed_target_resolved_) {
        pressed_target_cache_ = topmost_at(cursor_);
        pressed_target_resolved_ = true;
    }
    return pressed_target_cache_;
}

int HitMap::hovered() const noexcept {
    // 拖动中：只认被拖的那个。不这么做的话，手一滑出卡片拖动就断了。
    if (captured_ >= 0) {
        return captured_;
    }
    // 按住不放时锁在按下的那个对象上
    if (const int pressed = pressed_target(); pressed >= 0) {
        return pressed;
    }
    return topmost_at(cursor_);
}

int HitMap::hovered_in(int group) const noexcept {
    if (group == 0) {
        return hovered();
    }
    if (captured_ >= 0) {
        return captured_;
    }
    if (const int pressed = pressed_target(); pressed >= 0) {
        // 按住锁定的那个如果属于这一组，就认它
        for (const HitRegion& region : regions_) {
            if (region.id == pressed && region.group == group) {
                return pressed;
            }
        }
    }
    // 同一组里只有最上面那个算命中：卡片叠在一起时不能两块同时亮。
    for (auto it = regions_.rbegin(); it != regions_.rend(); ++it) {
        if (it->group != group) {
            continue;
        }
        if (region_accepts(*it, cursor_)) {
            return it->id;
        }
    }
    return -1;
}

bool HitMap::clicked(int id) const noexcept {
    if (!pressed_ || id < 0) {
        return false;
    }
    if (captured_ >= 0) {
        return captured_ == id;
    }
    return topmost_at(cursor_) == id;
}



bool HitMap::released_on(int id) const noexcept {
    if (!released_ || id < 0) {
        return false;
    }
    // 拖动过就不算"在这块上松开" —— 那是拖动结束，不是点击。
    // 调用方自己按 drag_delta() 判断是拖动还是点击。
    if (captured_ >= 0) {
        return false;
    }
    return topmost_at(cursor_) == id;
}

float ScrollState::maximum_offset() const noexcept {
    const float maximum = content_height_ - view_height_;
    return maximum > 0.0f ? maximum : 0.0f;
}

void ScrollState::set_offset(float offset) noexcept {
    // 夹取到合法区间。夹取本身不丢信息（界面上没有"滚到 -50"这种状态），
    // 所以不需要像别处那样额外留一条 note。
    offset_ = std::clamp(offset, 0.0f, maximum_offset());
}

bool ScrollState::scroll_by(float delta) noexcept {
    const float before = offset_;
    set_offset(offset_ + delta);
    return offset_ != before;
}

bool ScrollState::scroll_into_view(float item_top, float item_height) noexcept {
    const float before = offset_;
    if (item_top < offset_) {
        set_offset(item_top);
    } else if (item_top + item_height > offset_ + view_height_) {
        set_offset(item_top + item_height - view_height_);
    }
    return offset_ != before;
}

}  // namespace musicboard::ui
