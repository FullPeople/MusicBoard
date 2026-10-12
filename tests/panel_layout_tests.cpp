// 面板布局测试
//
// 全是纯几何，所以可以穷举。这里钉住的每一条都对应一种"看着不对"：
//   · 音波图被挤成 0 高（原型里真的发生过：列方向的 flex 容器里写了 flex:1）
//   · 卡片列数算错，最后一行溢出视口
//   · 滚动内容高度与实际不符，导致滚到底还有空白或最后一行看不到
//   · 台上元素跑到台外（负宽度、越界）
//   · 两列宽度不按 1.46 : 1 分
//
// 另外把原型里量出来的关键数字也断言一遍 —— 哪天有人"顺手调一下"，
// 这些数字会红，提醒他那是在改已经确认过的设计。

#include <cmath>
#include <string>
#include <vector>

#include "tests/test_harness.h"
#include "ui/panel_layout.h"

using namespace musicboard;
using musicboard::test::Registry;
using namespace musicboard::ui;

namespace {

bool inside(const Rect& outer, const Rect& inner, float tolerance = 0.01f) {
    return inner.x >= outer.x - tolerance && inner.y >= outer.y - tolerance &&
           inner.right() <= outer.right() + tolerance &&
           inner.bottom() <= outer.bottom() + tolerance;
}

bool non_negative(const Rect& rect) {
    return rect.width >= 0.0f && rect.height >= 0.0f;
}

bool near(float a, float b, float tolerance = 0.5f) {
    return std::fabs(a - b) <= tolerance;
}

PanelMetrics default_metrics() {
    return PanelMetrics{};
}

}  // namespace

int main() {
    Registry registry;

    const PanelMetrics metrics = default_metrics();
    const PanelLayout layout = compute_panel_layout(metrics);

    // =======================================================================
    // 与原型一致的关键数字
    // =======================================================================
    {
        registry.check(near(layout.panel.width, 1256.0f) && near(layout.panel.height, 788.0f),
                       "面板 1256 × 788（与 design/mockup.html 一致）");
        registry.check(near(layout.titlebar.height, 42.0f), "标题栏高 42");
        registry.check(near(layout.busbar.height, 34.0f), "底栏高 34");
        registry.check(near(layout.busbar.bottom(), 788.0f), "底栏贴住面板底部");
        registry.check(near(layout.titlebar.y, 0.0f), "标题栏在最上面");
        registry.check(near(layout.body.height, 788.0f - 42.0f - 34.0f),
                       "主体高度 = 788 - 42 - 34");
    }

    // =======================================================================
    // 两列按 1.46 : 1 分
    // =======================================================================
    {
        const float expected_bgm = 1256.0f * 1.46f / 2.46f;
        registry.check_eq(near(layout.bgm_column.width, expected_bgm, 1.0f),
                          "BGM 列宽 = 1256 × 1.46/2.46",
                          std::to_string(layout.bgm_column.width) + " vs " +
                              std::to_string(expected_bgm));
        registry.check(near(layout.bgm_column.right(), layout.sfx_column.x),
                       "两列严丝合缝相接（没有缝也没有重叠）");
        registry.check(near(layout.sfx_column.right(), 1256.0f), "SFX 列贴住面板右缘");
        registry.check(layout.bgm_column.width > layout.sfx_column.width,
                       "BGM 列比 SFX 列宽（1.46 > 1）");
    }

    // =======================================================================
    // 一块都不能越界、不能是负尺寸
    // =======================================================================
    {
        struct Named {
            const Rect& rect;
            const char* name;
        };
        const Named blocks[] = {
            {layout.titlebar, "标题栏"},   {layout.busbar, "底栏"},
            {layout.bgm_column, "BGM 列"}, {layout.sfx_column, "SFX 列"},
            {layout.bgm_head, "BGM 列头"}, {layout.bgm_deck, "BGM 台"},
            {layout.bgm_disc, "BGM 唱片"}, {layout.bgm_waveform, "BGM 音波图"},
            {layout.bgm_library, "BGM 曲库"},
            {layout.bgm_searchbox, "BGM 搜索框"},
            {layout.bgm_grid, "BGM 网格"},
            {layout.sfx_head, "SFX 列头"}, {layout.sfx_deck_grid, "SFX 台位网格"},
            {layout.sfx_library, "SFX 曲库"},
            {layout.sfx_searchbox, "SFX 搜索框"},
            {layout.sfx_grid, "SFX 网格"},
        };
        for (const Named& block : blocks) {
            registry.check(non_negative(block.rect),
                           std::string(block.name) + " 尺寸非负");
            registry.check(inside(layout.panel, block.rect),
                           std::string(block.name) + " 在面板之内");
        }
        for (std::size_t i = 0; i < sfx_slot_count; ++i) {
            const std::string tag = "音效台 " + std::to_string(i + 1);
            registry.check(non_negative(layout.sfx_decks[i]) &&
                               inside(layout.sfx_deck_grid, layout.sfx_decks[i]),
                           tag + " 在台位网格之内");
            registry.check(inside(layout.sfx_decks[i], layout.sfx_discs[i]),
                           tag + " 的唱片在台内");
            registry.check(inside(layout.sfx_decks[i], layout.sfx_waveforms[i]),
                           tag + " 的音波图在台内");
            registry.check(inside(layout.sfx_decks[i], layout.sfx_loop[i]),
                           tag + " 的循环按钮在台内");
            registry.check(inside(layout.sfx_decks[i], layout.sfx_fade[i]),
                           tag + " 的淡入淡出按钮在台内");
            registry.check(inside(layout.sfx_decks[i], layout.sfx_volumes[i]),
                           tag + " 的音量条在台内");
        }
        registry.check(inside(layout.bgm_deck, layout.bgm_disc), "BGM 唱片在台内");
        registry.check(inside(layout.bgm_deck, layout.bgm_waveform), "BGM 音波图在台内");
        registry.check(inside(layout.bgm_deck, layout.bgm_deck_line), "BGM 标签行在台内");
        registry.check(inside(layout.bgm_searchbox, layout.bgm_search_input),
                       "搜索输入框在搜索框内");
        registry.check(inside(layout.bgm_searchbox, layout.bgm_peek_area),
                       "悬停预览区在搜索框内（两者是同一个盒子）");
        registry.check(inside(layout.bgm_peek_area, layout.bgm_peek_wave),
                       "预览波形在预览区内");
    }

    // =======================================================================
    // 音波图不能是 0 高 —— 原型里真的被挤成过 0
    // =======================================================================
    {
        registry.check(layout.bgm_waveform.height > 10.0f,
                       "BGM 音波图高度正常（原型里被挤成 0 高过一次）");
        registry.check(layout.bgm_waveform.width > 100.0f, "BGM 音波图宽度正常");
        for (std::size_t i = 0; i < sfx_slot_count; ++i) {
            registry.check(layout.sfx_waveforms[i].height > 5.0f,
                           "音效台 " + std::to_string(i + 1) + " 的音波图有高度");
            registry.check(layout.sfx_waveforms[i].width > 10.0f,
                           "音效台 " + std::to_string(i + 1) + " 的音波图有宽度");
        }
    }

    // =======================================================================
    // 搜索框：输入框与悬停预览是同一个盒子
    // =======================================================================
    {
        registry.check(near(layout.bgm_search_input.width, 172.0f),
                       "输入框固定 172 宽（与原型一致）");
        registry.check(layout.bgm_peek_wave.width > 0.0f, "预览波形有宽度");
        registry.check(near(layout.bgm_peek_wave.width, 118.0f), "预览波形 118 宽");
        // 名字在前、波形在后：名字的左边界要在波形的左边界左边
        registry.check(layout.bgm_peek_text.x < layout.bgm_peek_wave.x,
                       "预览区里名字在波形左边（先名字、后波形）");
        registry.check(layout.bgm_peek_area.x > layout.bgm_search_input.x,
                       "预览区在输入框右边（不覆盖输入框）");
        // 同一个盒子：两者的上下边界一致
        registry.check(near(layout.bgm_search_input.y, layout.bgm_searchbox.y) &&
                           near(layout.bgm_peek_area.y, layout.bgm_searchbox.y),
                       "输入框与预览区顶边对齐（同一个盒子）");
    }

    // =======================================================================
    // 音效台 2×2，且四台互不重叠
    // =======================================================================
    {
        registry.check(near(layout.sfx_decks[0].y, layout.sfx_decks[1].y),
                       "台 1 与台 2 在同一行");
        registry.check(near(layout.sfx_decks[2].y, layout.sfx_decks[3].y),
                       "台 3 与台 4 在同一行");
        registry.check(layout.sfx_decks[2].y > layout.sfx_decks[0].y, "台 3 在台 1 下面");
        registry.check(near(layout.sfx_decks[1].x, layout.sfx_decks[3].x),
                       "右列的台 x 对齐");
        registry.check(layout.sfx_decks[0].right() <= layout.sfx_decks[1].x + 0.01f,
                       "左右两台不重叠");
        registry.check(layout.sfx_decks[0].bottom() <= layout.sfx_decks[2].y + 0.01f,
                       "上下两台不重叠");
        registry.check(near(layout.sfx_decks[2].y - layout.sfx_decks[0].bottom(), 9.0f),
                       "行间距 9（与原型一致）");
    }

    // =======================================================================
    // 唱片圆心与半径
    // =======================================================================
    {
        const Point center = PanelLayout::disc_center(layout.bgm_disc);
        registry.check(near(center.x, layout.bgm_disc.x + 80.0f) &&
                           near(center.y, layout.bgm_disc.y + 80.0f),
                       "BGM 唱片圆心在它的中心");
        registry.check(near(PanelLayout::disc_radius(layout.bgm_disc), 80.0f),
                       "BGM 唱片半径 80（直径 160）");
        registry.check(near(PanelLayout::disc_radius(layout.sfx_discs[0]), 31.0f),
                       "音效台唱片半径 31（直径 62）");
        // 唱片的圆形命中要用半径，这里保证它比台位小 ——
        // 否则圆会跑到台外，命中区域就会伸到邻居身上
        registry.check(PanelLayout::disc_radius(layout.sfx_discs[0]) * 2.0f <=
                           layout.sfx_stages[0].width + 0.01f,
                       "音效台唱片直径不超过它的舞台");
    }

    // =======================================================================
    // 卡片网格
    // =======================================================================
    {
        registry.check(layout.track_card_height > 0.0f, "卡片高度算出来了");
        registry.check(near(layout.track_card_height, 46.0f),
                       "卡片高度 = badge 30 + 上下内边距 16");

        const std::size_t columns = layout.card_columns(layout.bgm_grid);
        registry.check(columns >= 1, "至少一列");
        // 手算一遍：列数 = floor((宽 + 7) / (158 + 7))
        const float expected =
            std::floor((layout.bgm_grid.width + 7.0f) / (158.0f + 7.0f));
        registry.check_eq(static_cast<float>(columns) == expected,
                          "列数与 CSS 的 auto-fill(minmax(158px,1fr)) 算法一致",
                          "实际 " + std::to_string(columns) + "，手算 " +
                              std::to_string(static_cast<int>(expected)));

        // 每张卡片都必须落在网格的横向范围里
        const std::size_t count = columns * 3 + 1;   // 三行多一点
        bool all_inside = true;
        bool all_non_negative = true;
        for (std::size_t i = 0; i < count; ++i) {
            const Rect card = layout.card_rect(layout.bgm_grid, i, 0.0f);
            if (!non_negative(card)) {
                all_non_negative = false;
            }
            if (card.x < layout.bgm_grid.x - 0.01f ||
                card.right() > layout.bgm_grid.right() + 0.01f) {
                all_inside = false;
            }
        }
        registry.check(all_non_negative, "所有卡片尺寸非负");
        registry.check(all_inside, "所有卡片横向都在网格之内（最后一行不溢出）");

        // 卡片不互相重叠
        bool no_overlap = true;
        for (std::size_t i = 0; i + 1 < count; ++i) {
            const Rect a = layout.card_rect(layout.bgm_grid, i, 0.0f);
            const Rect b = layout.card_rect(layout.bgm_grid, i + 1, 0.0f);
            if (i % columns != columns - 1) {
                // 同一行相邻：a 的右边不该超过 b 的左边
                if (a.right() > b.x + 0.01f) {
                    no_overlap = false;
                }
            } else {
                // 换行：b 应该在下一行
                if (b.y <= a.y) {
                    no_overlap = false;
                }
            }
        }
        registry.check(no_overlap, "卡片互不重叠，换行正确");
    }

    // =======================================================================
    // 滚动偏移
    // =======================================================================
    {
        const std::size_t count = 40;
        const float content = layout.card_content_height(layout.bgm_grid, count);
        registry.check(content > 0.0f, "内容高度为正");

        const Rect first = layout.card_rect(layout.bgm_grid, 0, 0.0f);
        registry.check(near(first.y, layout.bgm_grid.y),
                       "没有滚动时第一张卡片贴着网格顶部");

        const Rect scrolled = layout.card_rect(layout.bgm_grid, 0, 30.0f);
        registry.check(near(scrolled.y, layout.bgm_grid.y - 30.0f),
                       "滚动 30 之后整体上移 30");

        // 内容高度必须覆盖最后一张卡片
        const std::size_t columns = layout.card_columns(layout.bgm_grid);
        const std::size_t last = count - 1;
        const Rect last_card = layout.card_rect(layout.bgm_grid, last, 0.0f);
        const float last_bottom_relative = last_card.bottom() - layout.bgm_grid.y;
        registry.check_eq(content >= last_bottom_relative - 0.01f,
                          "内容高度覆盖得到最后一行（否则最后一行滚不出来）",
                          "内容高 " + std::to_string(content) + "，最后一行底 " +
                              std::to_string(last_bottom_relative));
        static_cast<void>(columns);

        // 空曲库：内容高为 0，一张卡片也没有
        registry.check(layout.card_content_height(layout.bgm_grid, 0) == 0.0f,
                       "空曲库的内容高为 0");
    }

    // =======================================================================
    // 分类芯片
    // =======================================================================
    {
        const std::vector<float> widths = {70.0f, 90.0f, 60.0f, 120.0f, 80.0f};
        const std::vector<Rect> chips = layout.chip_rects(layout.bgm_chips, widths);
        registry.check(chips.size() == 5, "五个分类 → 五个芯片");
        registry.check(near(chips[0].width, 70.0f) && near(chips[3].width, 120.0f),
                       "芯片宽度用实测值（长名字的芯片更宽）");
        registry.check(layout.chip_rects(layout.bgm_chips, {}).empty(),
                       "没有分类时没有芯片（空宽度表 → 空结果）");
        // 宽度表里有 0 的项要退回固定宽度，而不是产生一个 0 宽的芯片
        const std::vector<Rect> mixed = layout.chip_rects(layout.bgm_chips, {0.0f, 88.0f});
        registry.check(mixed.size() == 2 && mixed[0].width > 0.0f,
                       "宽度为 0 的项退回固定宽度（不会出现 0 宽芯片）");
        registry.check(near(mixed[1].width, 88.0f), "有效宽度照用");
        bool non_overlapping = true;
        for (std::size_t i = 0; i + 1 < chips.size(); ++i) {
            if (chips[i].right() > chips[i + 1].x + 0.01f) {
                non_overlapping = false;
            }
        }
        registry.check(non_overlapping, "芯片不重叠");
        registry.check(near(chips.front().y, layout.bgm_chips.y), "芯片与容器顶对齐");
        registry.check(near(chips.front().height, layout.bgm_chips.height),
                       "芯片高度等于容器高度");
    }

    // =======================================================================
    // 面板小到放不下时：收缩而不是给负尺寸
    // =======================================================================
    {
        PanelMetrics tiny;
        tiny.width = 320.0f;
        tiny.height = 240.0f;
        const PanelLayout small = compute_panel_layout(tiny);

        registry.check(non_negative(small.bgm_column) && non_negative(small.sfx_column),
                       "极窄面板下列宽非负");
        registry.check(non_negative(small.bgm_deck) && non_negative(small.bgm_grid),
                       "极窄面板下台位与网格非负");
        registry.check(non_negative(small.bgm_waveform), "极窄面板下音波图非负");
        registry.check(non_negative(small.bgm_disc), "极窄面板下唱片非负");
        for (std::size_t i = 0; i < sfx_slot_count; ++i) {
            registry.check(non_negative(small.sfx_decks[i]), "极窄面板下音效台非负");
            registry.check(non_negative(small.sfx_waveforms[i]), "极窄面板下音效音波图非负");
        }
        registry.check(small.card_columns(small.bgm_grid) >= 1,
                       "极窄面板下列数至少为 1（不会返回 0 导致后面除零）");
        // 面板内容被截断是可以接受的（窗口会设最小尺寸），
        // 但**不能**出现负宽度或 0 列 —— 那会在下游变成崩或永久不命中
        registry.check(small.card_content_height(small.bgm_grid, 3) >= 0.0f,
                       "极窄面板下内容高度非负");
    }
    {
        // 尺寸恰好为零：整个布局都该是安全的空矩形
        PanelMetrics zero;
        zero.width = 0.0f;
        zero.height = 0.0f;
        const PanelLayout empty = compute_panel_layout(zero);
        registry.check(non_negative(empty.bgm_column) && non_negative(empty.sfx_column),
                       "零尺寸面板不产生负矩形");
        registry.check(non_negative(empty.bgm_searchbox) && non_negative(empty.bgm_grid),
                       "零尺寸面板下搜索框与网格非负");
        registry.check(empty.card_content_height(empty.bgm_grid, 5) == 0.0f,
                       "零尺寸面板的内容高为 0");
    }

    // =======================================================================
    // 布局的确定性
    // =======================================================================
    {
        const PanelLayout again = compute_panel_layout(metrics);
        const bool same = near(again.bgm_deck.x, layout.bgm_deck.x) &&
                          near(again.bgm_deck.y, layout.bgm_deck.y) &&
                          near(again.sfx_decks[3].width, layout.sfx_decks[3].width) &&
                          near(again.bgm_waveform.height, layout.bgm_waveform.height);
        registry.check(same, "同样输入算出同样布局（没有隐藏状态）");
    }

    // =======================================================================
    // 垂直顺序：列头 → 台位 → 曲库
    // =======================================================================
    {
        registry.check(layout.bgm_head.bottom() <= layout.bgm_deck.y + 0.01f,
                       "BGM 列头在台位上方");
        registry.check(layout.bgm_deck.bottom() <= layout.bgm_library.y + 0.01f,
                       "BGM 台位在曲库上方");
        registry.check(layout.bgm_library.bottom() <= layout.busbar.y + 0.01f,
                       "BGM 曲库在底栏上方");
        registry.check(layout.bgm_head.bottom() <= layout.bgm_deck.y + 0.01f &&
                           layout.sfx_head.bottom() <= layout.sfx_deck_grid.y + 0.01f,
                       "两列的列头都排在最前");
        registry.check(layout.sfx_deck_grid.bottom() <= layout.sfx_library.y + 0.01f,
                       "音效台位在 SFX 曲库上方");
        registry.check(layout.bgm_library.width > 0.0f && layout.sfx_library.width > 0.0f,
                       "两列都有曲库区（曲库在每一列里各有一份）");

        // 网格与芯片都必须在**自己那一列**之内 —— 只测"在面板之内"是不够的，
        // 越到隔壁列上照样在面板之内，而那看起来就是两列串了。
        registry.check(inside(layout.bgm_column, layout.bgm_grid), "BGM 网格在 BGM 列内");
        registry.check(inside(layout.sfx_column, layout.sfx_grid), "SFX 网格在 SFX 列内");
        registry.check(inside(layout.bgm_column, layout.bgm_chips), "BGM 芯片行在 BGM 列内");
        registry.check(inside(layout.bgm_column, layout.bgm_searchbox), "BGM 搜索框在 BGM 列内");
        registry.check(inside(layout.sfx_column, layout.sfx_searchbox), "SFX 搜索框在 SFX 列内");
        registry.check(inside(layout.bgm_column, layout.bgm_deck), "BGM 台位在 BGM 列内");
        registry.check(inside(layout.sfx_column, layout.sfx_deck_grid), "SFX 台位网格在 SFX 列内");

        // 芯片行必须在网格上方且不与它重叠。
        // 这里错过一次：芯片整体下移了 46px，直接压在**第一行卡片**上。
        registry.check_eq(layout.bgm_chips.bottom() <= layout.bgm_grid.y + 0.01f,
                          "BGM 芯片行在网格上方（不压住第一行卡片）",
                          "芯片底 " + std::to_string(layout.bgm_chips.bottom()) + "，网格顶 " +
                              std::to_string(layout.bgm_grid.y));
        registry.check_eq(layout.sfx_chips.bottom() <= layout.sfx_grid.y + 0.01f,
                          "SFX 芯片行在网格上方",
                          "芯片底 " + std::to_string(layout.sfx_chips.bottom()) + "，网格顶 " +
                              std::to_string(layout.sfx_grid.y));
        registry.check(layout.bgm_searchbox.bottom() <= layout.bgm_chips.y + 0.01f,
                       "搜索框在芯片行上方");
    }

    {
        PanelMetrics dj_metrics; dj_metrics.dj_mode=true;
        const auto dj=compute_panel_layout(dj_metrics);
        registry.check(dj.card_columns(dj.sfx_grid)==3, "DJ 一行三张独立卡片");
        registry.check(dj.card_rect(dj.sfx_grid,0,0).height==62 && dj.card_rect(dj.sfx_grid,3,0).y>dj.card_rect(dj.sfx_grid,2,0).y,
            "DJ 紧凑卡片按三列换行");
        const auto chips=dj.chip_rects(Rect{10,10,180,100},{60,70,80,90});
        registry.check(chips.size()==4 && near(chips[0].right(),chips[1].x) && near(chips[2].right(),chips[3].x) && chips[2].x==10 && chips[2].y>chips[1].y,
            "分类按钮每行无缝衔接，换行重新从左侧开始");
        dj_metrics.sfx_collapsed=true; dj_metrics.width=746;
        const auto folded=compute_panel_layout(dj_metrics);
        registry.check(folded.sfx_grid.empty() && folded.sfx_mode.empty() && folded.sfx_search_input.empty() && folded.sfx_discs[0].empty(),
            "折叠隐藏整列音效及交互几何");
        registry.check(folded.bgm_column.width==746 && folded.title_pin.right()<folded.title_minimize.x && folded.title_minimize.right()<folded.title_close.x && folded.title_close.right()<746,
            "窄窗 BGM 及图钉、收起、关闭重新排布，无越界");
        registry.check(folded.busbar_library.right()<folded.busbar_change.x && folded.busbar_change.right()<folded.busbar_reload.x,
            "窄窗底栏地址和两个独立按钮不重叠");
        const auto options=folded.option_rects(folded.bgm_deck_line,
            {"很长的标签组合，不能挤掉循环淡入淡出和从头播放", "FLAC", "循环", "淡入淡出", "从头播放"});
        registry.check(options.size()==5 && options.back().right()<=folded.bgm_deck_line.right()+.01f && options.back().width>=74,
            "长标签为三个控制开关保留空间");
        registry.check(near(folded.track_card_width,folded.card_rect(folded.bgm_grid,0,0).width),
            "拖动预览与实际卡片宽度一致");
    }
    return registry.finish("panel_layout_tests");
}
