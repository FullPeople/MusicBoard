// 界面交互内核测试
//
// 这一层全是纯逻辑，所以可以穷举。这里钉住的每一条都对应一种"看一眼发现不了"的
// 界面毛病：
//   · 悬停高亮闪一下（层次判定错了）
//   · 按住拖动时高亮跳到别的卡片上
//   · 拖到一半拖动断了（capture 没生效）
//   · 圆形的唱片用矩形命中，四个角上"看着是空的、点了却有反应"
//   · 滚到底之后内容还在往上跑

#include <string>
#include <vector>

#include "tests/test_harness.h"
#include "ui/ui_core.h"
#include "ui/track_label.h"
#include "ui/disc_motion.h"

using namespace musicboard;
using musicboard::test::Registry;
using namespace musicboard::ui;

namespace {

HitRegion rect_region(Rect bounds, int id, int group = 0) {
    HitRegion region;
    region.bounds = bounds;
    region.id = id;
    region.group = group;
    return region;
}

}  // namespace

int main() {
    Registry registry;

    // =======================================================================
    // 矩形几何
    // =======================================================================
    {
        const Rect rect{10.0f, 20.0f, 100.0f, 50.0f};
        registry.check(rect.right() == 110.0f && rect.bottom() == 70.0f, "右/下边界");
        registry.check(rect.contains(Point{10.0f, 20.0f}), "左上角算命中（闭区间起点）");
        registry.check(rect.contains(Point{109.9f, 69.9f}), "接近右下角算命中");
        registry.check(!rect.contains(Point{110.0f, 40.0f}), "右边界**不**算命中");
        registry.check(!rect.contains(Point{40.0f, 70.0f}), "下边界**不**算命中");
        registry.check(!rect.contains(Point{9.9f, 20.0f}), "左边界外侧不算命中");

        const Rect inset = rect.inset(5.0f);
        registry.check(inset.x == 15.0f && inset.y == 25.0f && inset.width == 90.0f &&
                           inset.height == 40.0f,
                       "内缩");
        const Rect moved = rect.offset(3.0f, -4.0f);
        registry.check(moved.x == 13.0f && moved.y == 16.0f, "平移");

        registry.check(Rect{}.empty(), "零尺寸矩形是空的");
        registry.check(!rect.empty(), "正常矩形不空");
        registry.check(rect.center().x == 60.0f && rect.center().y == 45.0f, "中心点");
    }
    {
        const Rect a{0.0f, 0.0f, 100.0f, 100.0f};
        const Rect b{50.0f, 50.0f, 100.0f, 100.0f};
        const Rect overlap = a.intersect(b);
        registry.check(overlap.x == 50.0f && overlap.y == 50.0f && overlap.width == 50.0f &&
                           overlap.height == 50.0f,
                       "相交区域");
        registry.check(a.intersect(Rect{200.0f, 200.0f, 10.0f, 10.0f}).empty(),
                       "不相交时返回空矩形");
        registry.check(a.intersect(Rect{100.0f, 0.0f, 10.0f, 10.0f}).empty(),
                       "只共边不算相交");
    }
    {
        // 圆命中
        registry.check(circle_hit(Point{0.0f, 0.0f}, 10.0f, Point{0.0f, 0.0f}), "圆心命中");
        registry.check(circle_hit(Point{0.0f, 0.0f}, 10.0f, Point{10.0f, 0.0f}), "圆上算命中");
        registry.check(!circle_hit(Point{0.0f, 0.0f}, 10.0f, Point{10.1f, 0.0f}), "圆外不算");
        // 外接正方形的角 —— 这正是矩形命中会误判的地方
        registry.check(!circle_hit(Point{0.0f, 0.0f}, 10.0f, Point{9.0f, 9.0f}),
                       "外接正方形的角在圆外（唱片不能用矩形命中）");
    }

    // =======================================================================
    // 层次：后加的画在上面，也先被点到
    // =======================================================================
    {
        HitMap hits;
        hits.begin_frame();
        hits.add(rect_region(Rect{0.0f, 0.0f, 100.0f, 100.0f}, 1));
        hits.add(rect_region(Rect{50.0f, 50.0f, 100.0f, 100.0f}, 2));   // 后加 → 在上面

        hits.set_cursor(Point{60.0f, 60.0f});   // 两块重叠区
        registry.check(hits.topmost_at(Point{60.0f, 60.0f}) == 2,
                       "重叠处命中的是后加的那块（绘制顺序就是层次）");
        hits.set_cursor(Point{10.0f, 10.0f});
        registry.check(hits.topmost_at(Point{10.0f, 10.0f}) == 1, "只有下面那块时命中它");
        registry.check(hits.topmost_at(Point{500.0f, 500.0f}) == -1, "都没命中返回 -1");
    }

    // =======================================================================
    // 互斥组：同一组里只有最上面那个算悬停
    // =======================================================================
    {
        HitMap hits;
        hits.begin_frame();
        hits.add(rect_region(Rect{0.0f, 0.0f, 100.0f, 40.0f}, 10, 7));
        hits.add(rect_region(Rect{0.0f, 30.0f, 100.0f, 40.0f}, 11, 7));

        hits.set_cursor(Point{50.0f, 35.0f});   // 落在两块的重叠处
        registry.check_eq(hits.hovered_in(7) == 11, "同组重叠处只亮上面那块",
                          std::to_string(hits.hovered_in(7)));
        registry.check(hits.hovered_in(8) == -1, "别的组里没人被悬停");

        hits.set_cursor(Point{50.0f, 5.0f});
        registry.check(hits.hovered_in(7) == 10, "只落在下面那块时亮它");
    }

    // =======================================================================
    // 禁用区域完全不参与
    // =======================================================================
    {
        HitMap hits;
        hits.begin_frame();
        HitRegion disabled = rect_region(Rect{0.0f, 0.0f, 100.0f, 100.0f}, 5);
        disabled.enabled = false;
        hits.add(disabled);
        registry.check(hits.topmost_at(Point{50.0f, 50.0f}) == -1,
                       "禁用的区域不参与命中（空盘上划过鼠标不该有反应）");

        hits.begin_frame();
        hits.add(rect_region(Rect{0.0f, 0.0f, 100.0f, 100.0f}, -1));
        registry.check(hits.topmost_at(Point{50.0f, 50.0f}) == -1, "id = -1 表示不接受命中");
    }

    // =======================================================================
    // 圆形命中（唱片、悬浮球）
    // =======================================================================
    {
        HitMap hits;
        hits.begin_frame();
        HitRegion disc = rect_region(Rect{0.0f, 0.0f, 100.0f, 100.0f}, 3);
        disc.circle_center = Point{50.0f, 50.0f};
        disc.circle_radius = 50.0f;
        hits.add(disc);

        registry.check(hits.topmost_at(Point{50.0f, 50.0f}) == 3, "圆心命中");
        registry.check(hits.topmost_at(Point{50.0f, 1.0f}) == 3, "正上方圆内命中");
        registry.check(hits.topmost_at(Point{2.0f, 2.0f}) == -1,
                       "外接矩形的角不命中（这里最容易出错）");
        registry.check(hits.topmost_at(Point{98.0f, 98.0f}) == -1, "右下角同样不命中");
    }

    // =======================================================================
    // 按下锁定：按住之后小幅移动，高亮不能跳
    // =======================================================================
    {
        HitMap hits;
        hits.begin_frame();
        hits.add(rect_region(Rect{0.0f, 0.0f, 100.0f, 100.0f}, 1));
        hits.add(rect_region(Rect{100.0f, 0.0f, 100.0f, 100.0f}, 2));

        hits.set_cursor(Point{50.0f, 50.0f});
        hits.set_left_pressed(true);
        registry.check(hits.hovered() == 1, "按下时悬停是第 1 块");
        registry.check(hits.clicked(1), "第 1 块收到了点击");
        registry.check(!hits.clicked(2), "第 2 块没有收到点击");

        // 下一帧：鼠标已经越过边界，但还没松开
        hits.begin_frame();
        hits.add(rect_region(Rect{0.0f, 0.0f, 100.0f, 100.0f}, 1));
        hits.add(rect_region(Rect{100.0f, 0.0f, 100.0f, 100.0f}, 2));
        hits.set_cursor(Point{150.0f, 50.0f});
        hits.set_left_down(true);
        registry.check_eq(hits.hovered() == 1,
                          "按住不放时悬停仍锁定在按下那块（高亮不跳）",
                          std::to_string(hits.hovered()));

        // 松开之后锁定解除
        hits.begin_frame();
        hits.add(rect_region(Rect{0.0f, 0.0f, 100.0f, 100.0f}, 1));
        hits.add(rect_region(Rect{100.0f, 0.0f, 100.0f, 100.0f}, 2));
        hits.set_cursor(Point{150.0f, 50.0f});
        hits.set_left_down(false);
        registry.check(hits.hovered() == 2, "松开之后悬停跟着鼠标走");
    }

    // =======================================================================
    // capture：拖动
    // =======================================================================
    {
        HitMap hits;
        hits.begin_frame();
        hits.add(rect_region(Rect{0.0f, 0.0f, 100.0f, 100.0f}, 1));
        hits.add(rect_region(Rect{300.0f, 300.0f, 100.0f, 100.0f}, 2));

        hits.set_cursor(Point{50.0f, 50.0f});
        hits.set_left_pressed(true);
        hits.capture(1);
        hits.set_capture_origin(Point{50.0f, 50.0f});
        hits.set_drag_position(Point{50.0f, 50.0f});

        // 拖到目标位置：手已经离开第 1 块，甚至在空白处
        hits.set_cursor(Point{400.0f, 400.0f});
        hits.set_drag_position(Point{400.0f, 400.0f});
        registry.check(hits.hovered() == 1, "拖动中悬停一直是拖动对象（划过别处也不断）");
        registry.check(hits.captured() == 1 && hits.is_captured(), "capture 生效");
        registry.check(hits.drag_delta().x == 350.0f && hits.drag_delta().y == 350.0f,
                       "拖动位移算得对");
        registry.check(hits.topmost_at(Point{400.0f, 400.0f}) == -1,
                       "落点在别的控件上时 topmost 仍然如实回答（拖动落点判定要用它）");
        registry.check(hits.topmost_at(Point{350.0f, 350.0f}) == 2,
                       "落到第 2 块上时 topmost 是它（拖放要用这个）");

        // 拖动中松开：released_on 必须返回 false —— 那是拖动结束，不是点击
        hits.set_left_released(true);
        registry.check(!hits.released_on(2), "拖动之后松开不算'在第 2 块上点击'");

        hits.release_capture();
        registry.check(!hits.is_captured(), "释放 capture");
        registry.check(hits.hovered() == -1, "释放之后悬停回到鼠标位置（这里是空白）");
    }

    // =======================================================================
    // 滚动
    // =======================================================================
    {
        ScrollState scroll;
        scroll.set_view_height(300.0f);
        scroll.set_content_height(1000.0f);
        registry.check(scroll.maximum_offset() == 700.0f, "最大偏移 = 内容高 - 视口高");
        registry.check(scroll.scrollable(), "内容比视口长时可以滚");

        registry.check(scroll.scroll_by(100.0f), "向下滚 100");
        registry.check(scroll.offset() == 100.0f, "偏移正确");
        registry.check(scroll.scroll_by(-50.0f), "向上滚 50");
        registry.check(scroll.offset() == 50.0f, "偏移正确");

        // 滚到顶之后继续往上：偏移不变，且**返回 false**（界面不该重绘）
        scroll.set_offset(0.0f);
        registry.check(!scroll.scroll_by(-100.0f), "已经在顶还往上滚 → 没变化");
        registry.check(scroll.offset() == 0.0f, "偏移被夹在 0（不会变成负数）");

        // 滚到底之后继续往下
        scroll.set_offset(700.0f);
        registry.check(!scroll.scroll_by(100.0f), "已经在底还往下滚 → 没变化");
        registry.check(scroll.offset() == 700.0f, "偏移被夹在最大值");
        registry.check(scroll.scroll_by(-100.0f), "从底部往上滚是有效的");
    }
    {
        // 内容比视口短：不允许滚动，也不能出现负的最大偏移
        ScrollState scroll;
        scroll.set_view_height(500.0f);
        scroll.set_content_height(200.0f);
        registry.check(scroll.maximum_offset() == 0.0f, "内容比视口短 → 最大偏移是 0");
        registry.check(!scroll.scrollable(), "不可滚动");
        registry.check(!scroll.scroll_by(100.0f), "想滚也没用（返回 false）");
        registry.check(scroll.offset() == 0.0f, "偏移保持 0");
        // 直接设一个超范围的偏移也要被夹住
        scroll.set_offset(999.0f);
        registry.check(scroll.offset() == 0.0f, "直接设超范围偏移被夹到 0");
    }
    {
        // 内容正好等于视口：不可滚动
        ScrollState scroll;
        scroll.set_view_height(400.0f);
        scroll.set_content_height(400.0f);
        registry.check(!scroll.scrollable() && scroll.maximum_offset() == 0.0f,
                       "内容正好装满 → 不可滚动");
    }
    {
        // 滚进视野
        ScrollState scroll;
        scroll.set_view_height(300.0f);
        scroll.set_content_height(2000.0f);

        // 视野是 0..300，这一项在 100..180 —— 本来就完整可见，不该滚
        registry.check(!scroll.scroll_into_view(100.0f, 80.0f),
                       "已经在视野里 → 不滚（返回 false，界面不该重绘）");
        registry.check(scroll.offset() == 0.0f, "偏移不变");

        scroll.set_offset(500.0f);
        registry.check(scroll.scroll_into_view(100.0f, 80.0f), "在上面视野外 → 向上滚");
        registry.check(scroll.offset() == 100.0f, "滚到该项的顶部");

        scroll.set_offset(0.0f);
        registry.check(scroll.scroll_into_view(280.0f, 80.0f), "在下边界外 → 向下滚");
        registry.check(scroll.offset() == 60.0f, "滚到该项的底部刚好进视野");

        scroll.set_offset(0.0f);
        // 286..366 的下半截在视野外（视野只到 300），所以要滚到 66
        registry.check(scroll.scroll_into_view(286.0f, 80.0f), "下半截在外 → 滚动");
        registry.check(scroll.offset() == 66.0f, "滚到该项底部刚好进视野");

        scroll.set_offset(0.0f);
        registry.check(!scroll.scroll_into_view(150.0f, 80.0f),
                       "完整可见（150..230 在 0..300 之内）→ 不滚");
        registry.check(scroll.offset() == 0.0f, "偏移不变");

        // 最后一项：滚进视野会被夹在最大值
        scroll.set_offset(0.0f);
        registry.check(scroll.scroll_into_view(1950.0f, 200.0f), "最后一项 → 滚动");
        registry.check(scroll.offset() == scroll.maximum_offset(),
                       "被夹在最大偏移（不会滚过头）");
    }

    {
        const auto label=parse_track_label("（低）[休整]{和平}【水边】(轻松) Fast Food [live]");
        registry.check(label.title=="Fast Food [live]" && label.tags==std::vector<std::string>{"低","休整","和平","水边","轻松"},
            "只提取连续前置的五种括号标签，保留标题中的括号");
        registry.check(parse_track_label("[未闭合 曲名").title=="[未闭合 曲名", "未闭合括号不会吞掉名字");
        registry.check(parse_track_label("[曲名]").title=="[曲名]" && parse_track_label("[曲名]").tags.empty(), "纯括号名字不会显示空标题");
        registry.check(parse_track_label("[低][低] 曲子").tags.size()==1, "重复前置标签只显示一次");
        DiscMotion motion;
        motion.set_playing(true,10);
        const auto start=motion.angle_at(11);
        registry.check(std::abs(motion.angle_at(24)-start-360)<.0001, "稳定转速为 13 秒一圈，速度减半");
        motion.set_playing(false,24);
        const auto paused=motion.angle_at(24);
        registry.check(motion.angle_at(24.14)>paused && motion.speed_at(24.14)>0, "暂停时平滑减速，不瞬停");
        registry.check(motion.speed_at(25)==0 && motion.angle_at(25)==motion.angle_at(60), "暂停过渡结束后角度保持不动");
        registry.check(motion.arm_at(24.14)>0 && motion.arm_at(24.14)<1 && motion.arm_at(25)==0, "唱针跟随暂停状态抬起");
        const auto fixed=motion.angle_at(60);
        motion.set_playing(true,60);
        registry.check(motion.angle_at(60)==std::fmod(fixed,360.0), "恢复时从暂停角度继续，不跳回起点");
        const auto expected=motion.angle_at(90);
        for(int i=0;i<2000;++i) static_cast<void>(motion.angle_at(60+static_cast<double>(i)/100));
        registry.check(motion.angle_at(90)==expected, "掉帧和查询次数不会改变绝对时间动画");
    }
    return registry.finish("ui_core_tests");
}
