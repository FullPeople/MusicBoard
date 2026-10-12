#pragma once

#include <windows.h>

#include <d2d1.h>
#include <d2d1_1.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <wrl/client.h>

#include "core/failure.h"
#include "ui/cover_cache.h"
#include "ui/ui_core.h"

namespace musicboard::ui {

// ============================================================================
// 悬浮球的绘制
//
// 为什么单独有这一份：球窗口一直存在、命中测试也一直是对的，但**从来没画过**——
// 正式程序里那段渲染回调是空的（"球的内容由 OrbWindow 自己画"这句注释是错的，
// OrbWindow 只处理消息，不画东西）。于是用户看到的是"我的悬浮球呢？"：
// 一个 184×184 的完全透明的窗口，点得到、看不见。
//
// 视觉与 spike 里验证过的那一份一致（阶段 3.5 / 6）：能量辉光 + 进度环 + 盘面 +
// 封面 + 中心轴孔。
//
// 与 PanelView 相同的两条纪律：
//   * 画刷/位图是**目标相关**资源，按 target 缓存；
//   * 文字格式来自 IDWriteFactory，是设备无关的，建一次就够。
// ============================================================================

struct OrbViewState final {
    int hover_zone = 0;
    float hover = 0.0f;
    float progress = 0.0f;          // 0..1 播放进度
    float rotation_radians = 0.0f;  // 封面转动角
    float amplitude = 0.0f;         // 当前能量 0..1，驱动辉光
    bool playing = false;
    bool has_track = false;
    // 球的环用哪条总线的颜色（放的是 BGM 就是琥珀）
    D2D1_COLOR_F accent = D2D1::ColorF(0xE8A33D);
    std::filesystem::path art_path;   // 有封面就贴在盘面上
};

class OrbView final {
public:
    OrbView() = default;
    ~OrbView();

    OrbView(const OrbView&) = delete;
    OrbView& operator=(const OrbView&) = delete;

    // 设备上下文由窗口持有，必须比 OrbView 活得久（与 PanelView 同约定）。
    [[nodiscard]] bool initialize(ID2D1RenderTarget& target, ID2D1DeviceContext* device_context,
                                  Failure& failure);

    void draw(ID2D1RenderTarget& target, float logical_width, float logical_height,
              const OrbViewState& state);
    void clear_covers() { covers_.clear(); }

private:
    [[nodiscard]] bool ensure_brush(ID2D1RenderTarget& target, Failure& failure);
    // 圆形遮罩（盘面裁剪）——与面板那边同理：几何是目标相关资源
    [[nodiscard]] ID2D1EllipseGeometry* circular_mask(ID2D1RenderTarget& target, const Point& center,
                                                      float radius);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    ID2D1RenderTarget* brush_target_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1EllipseGeometry> mask_geometry_;
    ID2D1RenderTarget* mask_target_ = nullptr;
    float mask_radius_ = -1.0f;
    Point mask_center_{};
    CoverCache covers_;
    ID2D1DeviceContext* device_context_ = nullptr;
};

}  // namespace musicboard::ui
