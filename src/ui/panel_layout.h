#pragma once

#include <array>
#include <string>
#include <cstddef>
#include <vector>

#include "core/product_settings.h"   // sfx_slot_count
#include "ui/ui_core.h"

namespace musicboard::ui {

// ============================================================================
// 面板布局
//
// 全部数字来自 `design/mockup.html` —— 不是估的，是从原型里量出来的
// （面板 1256×788、标题栏 42、底栏 34、两列 flex 1.46 : 1、
// 列内边距 14/16/16、BGM 台 padding 16/18 + 唱片 160、
// 音效台 2×2 gap 9 + 唱片 76、曲库 minmax(158px,1fr) gap 7、
// 搜索框高 46 且"输入框 + 悬停预览"在同一个盒子里）。
// 于是原生面板和那份已确认的原型能对得上，而不是各画各的。
//
// 为什么把布局单独抽成可测的纯函数：面板里的每一处"看着不对"几乎都是布局算错 ——
// 音波图被挤成 0 高（原型里真的发生过一次：`flex:1` 在列方向的 flex 容器里）、
// 卡片列数算错导致最后一行溢出、滚动内容高度与实际不符。这些都能只看几个数就判定。
//
// 坐标全部是**逻辑坐标**（96 DPI）。窗口负责物理像素缩放。
// ============================================================================

struct PanelMetrics final {
    bool dj_mode = false;
    bool sfx_collapsed = false;
    float width = 1256.0f;
    float height = 788.0f;
    float corner_radius = 16.0f;

    float titlebar_height = 42.0f;
    float busbar_height = 34.0f;

    // 两列：BGM 比 SFX 宽
    float bgm_flex = 1.46f;
    float sfx_flex = 1.0f;

    float column_padding_x = 16.0f;
    float column_padding_top = 14.0f;
    float column_padding_bottom = 16.0f;
    float column_head_height = 20.0f;      // 标题行（含音量条）
    float column_head_gap = 12.0f;         // margin-bottom

    // 音量条（列头右侧）。
    // 原来是 104 —— 在 1256 宽的面板上太短，一格滚轮就跳 4%，微调不动。
    // 加长到 208（两倍）之后同样的像素距离对应一半的百分比变化。
    float volume_slider_width = 208.0f;
    float volume_slider_height = 20.0f;
    float level_meter_width = 34.0f;

    // BGM 台
    float bgm_deck_padding_y = 16.0f;
    float bgm_deck_padding_x = 18.0f;
    float bgm_deck_gap = 24.0f;            // 唱片与信息之间
    float bgm_disc = 160.0f;               // 唱片直径
    float bgm_ring_width = 3.0f;
    float bgm_waveform_height = 42.0f;
    float bgm_deck_line_height = 26.0f;    // 标签 + 选项那一行
    float bgm_deck_name_height = 26.0f;
    float bgm_deck_row_gap = 11.0f;

    // 音效台
    float sfx_grid_gap = 9.0f;
    float sfx_deck_padding_y = 10.0f;
    float sfx_deck_padding_x = 10.0f;
    float sfx_deck_gap = 9.0f;
    float sfx_stage = 76.0f;               // 唱片舞台（方形）
    float sfx_disc = 62.0f;                // 唱片本体
    float sfx_waveform_height = 18.0f;
    float sfx_side_control_width = 20.0f;  // 左侧竖向按钮 / 右侧竖向音量条的宽度
    // 左侧两个按钮（循环 / 淡入淡出）小，右侧竖向音量条长 —— 它们不是一回事，
    // 一开始共用一个 56 的高度，结果两个按钮加起来 116px 塞不进 76px 的内区，
    // 按钮直接跑到台外（测试抓到的）。
    float sfx_button_height = 26.0f;
    float sfx_button_gap = 6.0f;
    float sfx_volume_height = 56.0f;
    float sfx_name_height = 16.0f;
    float sfx_stage_wave_gap = 4.0f;

    // 曲库
    float library_gap = 12.0f;             // 与上方块之间
    float searchbox_height = 46.0f;
    float searchbox_input_width = 172.0f;
    float searchbox_icon_width = 32.0f;
    float peek_wave_width = 118.0f;
    float peek_wave_height = 22.0f;
    float peek_gap = 10.0f;
    // 分类芯片：一行的高度 + 行间距 + 容器上下留白。
    // 行数由实测宽度算出来（bgm_chip_rows / sfx_chip_rows），因为分类名长短差别
    // 很大，固定一行会把后面的分类整个挤出可见区。
    // 上下留白是必需的：选中态的高亮描边与悬停光晕都画在药丸之外，
    // 一点余量不留就会被裁剪 —— 用户看到的正是"高亮的上下被切了"。
    float chip_height = 26.0f;
    float chip_row_gap = 6.0f;
    float chip_vertical_padding = 5.0f;
    int bgm_chip_rows = 1;
    int sfx_chip_rows = 1;
    float lib_head_gap = 9.0f;
    float lib_head_margin_bottom = 10.0f;

    float track_grid_gap = 7.0f;
    float track_card_min_width = 158.0f;
    float track_card_padding_y = 8.0f;
    float track_card_padding_x = 9.0f;
    float track_badge = 30.0f;
    float track_grid_padding_bottom = 12.0f;
    float scrollbar_width = 8.0f;
};

// 一帧的布局结果。全部是逻辑坐标，相对面板左上角。
struct PanelLayout final {
    bool dj_mode = false;
    bool sfx_collapsed = false;
    Rect title_pin, title_minimize, title_close;
    Rect title_cn, title_en;
    Rect busbar_change, busbar_reload, busbar_fold, busbar_show_all;
    Rect sfx_mode;
    Rect busbar_library;
    Rect panel;
    Rect titlebar;
    Rect busbar;
    // 底栏里的历史条：两个按钮（上一首 / 下一首）
    Rect bus_prev;
    Rect bus_next;
    Rect body;

    Rect bgm_column;
    Rect bgm_head;
    Rect bgm_title;
    Rect bgm_volume_slider;
    Rect bgm_meter;
    Rect bgm_deck;
    Rect bgm_disc;              // 外接正方形；圆心用 center()，半径用 width/2
    Rect bgm_tonearm;
    Rect bgm_deck_text;         // 曲名等文字区
    Rect bgm_deck_line;         // 标签 + 选项那一行
    Rect bgm_waveform;
    Rect bgm_time_left;
    Rect bgm_time_right;
    // 音波图下面那行空位：上一首 / 下一首（原型把历史条放在这里）
    Rect bgm_prev;
    Rect bgm_next;
    // 列头标题后面的"清空"（有曲目时才画/才可点）
    Rect bgm_clear;
    Rect sfx_clear;
    Rect bgm_library;
    Rect bgm_searchbox;
    Rect bgm_search_icon;
    Rect bgm_search_input;
    Rect bgm_peek_area;
    Rect bgm_peek_text;
    Rect bgm_peek_wave;
    Rect bgm_chips;
    Rect bgm_grid;              // 视口

    Rect sfx_column;
    Rect sfx_head;
    Rect sfx_title;
    Rect sfx_volume_slider;
    Rect sfx_meter;
    // 四个台位（数量是已定死的 4）
    std::array<Rect, sfx_slot_count> sfx_decks;
    std::array<Rect, sfx_slot_count> sfx_stages;
    std::array<Rect, sfx_slot_count> sfx_discs;
    std::array<Rect, sfx_slot_count> sfx_names;
    std::array<Rect, sfx_slot_count> sfx_loop;      // 左侧竖向按钮（上）
    std::array<Rect, sfx_slot_count> sfx_fade;      // 左侧竖向按钮（下）
    std::array<Rect, sfx_slot_count> sfx_waveforms;
    std::array<Rect, sfx_slot_count> sfx_volumes;   // 右侧竖向音量条
    Rect sfx_deck_grid;
    Rect sfx_library;
    Rect sfx_searchbox;
    Rect sfx_search_icon;
    Rect sfx_search_input;
    Rect sfx_peek_area;
    Rect sfx_peek_text;
    Rect sfx_peek_wave;
    Rect sfx_chips;
    Rect sfx_grid;

    // 绘制与命中测试要用到的几个派生尺寸。
    // 放在这里而不是让调用方各自算：算两遍就一定会有一处算错。
    float chip_width = 0.0f;            // 一个分类芯片的基准宽度
    float chip_height = 26.0f;          // 一行芯片的高（chip_rects 分行要用）
    float chip_row_gap = 6.0f;          // 芯片行间距
    float chip_vertical_padding = 5.0f; // 芯片容器上下留白（高亮不被裁）
    float track_card_height = 0.0f;     // 曲目卡片高度
    float track_card_width = 0.0f;      // 曲目卡片宽度（拖拽幽灵要按它画）
    float track_grid_gap = 0.0f;        // 卡片间距（card_columns 要用）
    float track_card_min_width = 0.0f;  // 每列最小宽度（决定列数）
    float bgm_ring_width = 0.0f;        // BGM 唱片的进度环宽度
    float scrollbar_width = 0.0f;

    // 分类芯片的矩形。
    //
    // widths 是**实测出来的**每个芯片宽度（由 PanelView 用 DirectWrite 量）。
    // 传空则退回一个保守的固定宽度 —— 那个固定宽度会让长名字溢出药丸、
    // 也会让命中区域与看得见的区域不一致，所以正式路径一定会传实测值。
    // **flow 换行**：一行放不下就换到下一行。
    [[nodiscard]] std::vector<Rect> chip_rects(const Rect& container,
                                               const std::vector<float>& widths) const;

    // 一行里若干个按文字宽度排布的小开关（BGM 台的 循环 / 淡入淡出 / 从头播放）。
    //
    // 绘制与命中测试**必须**走同一个函数：各写一份的话，框和点击区迟早对不上，
    // 而症状是"这个按钮点不着 / 点了隔壁"，极难从代码上看出来。
    [[nodiscard]] std::vector<Rect> option_rects(const Rect& row,
                                                 const std::vector<std::string>& labels) const;
    // 曲目卡片：列数由视口宽度算，行按滚动偏移排
    [[nodiscard]] std::size_t card_columns(const Rect& grid) const;
    [[nodiscard]] Rect card_rect(const Rect& grid, std::size_t index, float scroll_offset) const;
    // 全部卡片的内容总高（滚动条与滚动上限要用）
    [[nodiscard]] float card_content_height(const Rect& grid, std::size_t count) const;

    // 横向音量条**真正可拖的那一段**：右边留出百分比文字的宽度。
    // 绘制、命中、拖动换算三处都必须用它 —— 各算各的就会出现"看得见的位置
    // 和设进去的值对不上"。
    [[nodiscard]] static Rect volume_track(const Rect& slider) noexcept;

    // 唱片圆心与半径（圆形的命中测试与绘制都用它）
    [[nodiscard]] static Point disc_center(const Rect& disc) noexcept;
    [[nodiscard]] static float disc_radius(const Rect& disc) noexcept;
};

// 按面板尺寸与分类数量算出一整套矩形。
//
// 只做几何，不碰任何设备。传入的 metrics 一般是默认值；
// 小窗口下会自动收紧（见实现里的 minimum 收口），所以它不会算出负宽度。
[[nodiscard]] PanelLayout compute_panel_layout(const PanelMetrics& metrics);

}  // namespace musicboard::ui
