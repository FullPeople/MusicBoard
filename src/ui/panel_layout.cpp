#include "ui/panel_layout.h"

#include <algorithm>
#include <cmath>

namespace musicboard::ui {
namespace {

// 面板小到某个块放不下时，宁可把它压到 0 高也不要给出负宽度 ——
// 负尺寸在下游会以各种面目出现（画反的矩形、命中测试永远失败、
// 文字布局直接崩），而"这里放不下"本身是一句可以说清楚的话。
[[nodiscard]] float at_least_zero(float value) noexcept {
    return value > 0.0f ? value : 0.0f;
}

// 从一块里切出上/下/左/右的一条
[[nodiscard]] Rect take_top(Rect& source, float height) {
    const float taken = std::min(height, at_least_zero(source.height));
    const Rect result{source.x, source.y, source.width, taken};
    source.y += taken;
    source.height = at_least_zero(source.height - taken);
    return result;
}

[[nodiscard]] Rect take_left(Rect& source, float width) {
    const float taken = std::min(width, at_least_zero(source.width));
    const Rect result{source.x, source.y, taken, source.height};
    source.x += taken;
    source.width = at_least_zero(source.width - taken);
    return result;
}

[[nodiscard]] Rect take_bottom(Rect& source, float height) {
    const float taken = std::min(height, at_least_zero(source.height));
    const Rect result{source.x, source.y + at_least_zero(source.height - taken), source.width,
                      taken};
    source.height = at_least_zero(source.height - taken);
    return result;
}

[[nodiscard]] Rect take_right(Rect& source, float width) {
    const float taken = std::min(width, at_least_zero(source.width));
    const Rect result{source.right() - taken, source.y, taken, source.height};
    source.width = at_least_zero(source.width - taken);
    return result;
}

}  // namespace

Rect PanelLayout::volume_track(const Rect& slider) noexcept {
    constexpr float kValueLabelWidth = 44.0f;
    return Rect{slider.x, slider.y, at_least_zero(slider.width - kValueLabelWidth),
                slider.height};
}

Point PanelLayout::disc_center(const Rect& disc) noexcept {
    return disc.center();
}

float PanelLayout::disc_radius(const Rect& disc) noexcept {
    return std::min(disc.width, disc.height) * 0.5f;
}

std::size_t PanelLayout::card_columns(const Rect& grid) const {
    if (dj_mode && grid.x == sfx_grid.x && !sfx_collapsed) return 3;
    // 与 CSS 的 repeat(auto-fill, minmax(158px, 1fr)) 一致：
    // 列数 = floor((可用宽 + gap) / (最小卡宽 + gap))。
    // 用"可用宽 + gap"是因为 N 列之间有 N-1 个 gap，而最后一列右边没有 gap。
    if (grid.width <= 0.0f || track_card_min_width <= 0.0f) {
        return 1;   // 一张都放不下时也返回 1，让调用方永远有个合法值
    }
    const auto columns = static_cast<std::size_t>(
        std::floor((grid.width + track_grid_gap) / (track_card_min_width + track_grid_gap)));
    return std::max<std::size_t>(1, columns);
}

Rect PanelLayout::card_rect(const Rect& grid, std::size_t index, float scroll_offset) const {
    const float kGridGap = track_grid_gap;
    const float card_height = dj_mode && grid.x == sfx_grid.x ? 62.0f : track_card_height;
    const std::size_t columns = card_columns(grid);
    const std::size_t row = index / columns;
    const std::size_t column = index % columns;

    // 每列等宽：可用宽减去 (列数-1) 个 gap 再均分
    const float total_gap = kGridGap * static_cast<float>(columns > 0 ? columns - 1 : 0);
    const float card_width = at_least_zero((grid.width - total_gap) / static_cast<float>(columns));

    return Rect{grid.x + static_cast<float>(column) * (card_width + kGridGap),
                grid.y + static_cast<float>(row) * (card_height + kGridGap) - scroll_offset,
                card_width, card_height};
}

float PanelLayout::card_content_height(const Rect& grid, std::size_t count) const {
    const float kGridGap = track_grid_gap;
    if (count == 0 || grid.width <= 0.0f) {
        return 0.0f;
    }
    const std::size_t columns = card_columns(grid);
    const std::size_t rows = (count + columns - 1) / columns;
    // 末尾留一段，免得最后一行卡片贴着滚动区底部
    constexpr float kBottomPadding = 12.0f;
    const float card_height = dj_mode && grid.x == sfx_grid.x ? 62.0f : track_card_height;
    return static_cast<float>(rows) * card_height +
           kGridGap * static_cast<float>(rows > 0 ? rows - 1 : 0) + kBottomPadding;
}

std::vector<Rect> PanelLayout::chip_rects(const Rect& container,
                                          const std::vector<float>& widths) const {
    std::vector<Rect> rects;
    if (widths.empty() || container.width <= 0.0f) {
        return rects;
    }
    constexpr float kChipGap = 0.0f;
    const float fallback = chip_width > 0.0f ? chip_width : 70.0f;
    const float row_height = chip_height > 0.0f ? chip_height : container.height;
    const float row_step = row_height + chip_row_gap;
    rects.reserve(widths.size());
    float x = container.x;
    float y = container.y; // The layout already applied the container padding.
    for (const float measurable : widths) {
        const float width = std::min(container.width, measurable > 0.0f ? measurable : fallback);
        // 换行：这一行放不下就另起一行。容器高度是按行数算好的，这里只管换。
        if (x > container.x && x + width > container.right()) {
            x = container.x;
            y += row_step;
        }
        rects.push_back(Rect{x, y, width, row_height});
        x += width + kChipGap;
    }
    return rects;
}

std::vector<Rect> PanelLayout::option_rects(const Rect& row,
                                            const std::vector<std::string>& labels) const {
    std::vector<Rect> rects;
    if (row.height <= 0.0f) {
        return rects;
    }
    constexpr float kOptionGap = 6.0f;
    // 宽度按**字符数**估（不是字节数）。中文在 UTF-8 里占 3 个字节，
    // 按字节估会宽三倍：4 个汉字会算成 166px 而不是约 60px，
    // 于是这一行五个开关加起来超出唱片台，"从头播放"被挤到台外看不见。
    // 绘制与命中用的是同一个数，所以框和点击区始终一致。
    const auto label_width = [](const std::string& label) {
        std::size_t characters = 0;
        for (const char ch : label) {
            // UTF-8 的续字节（10xxxxxx）不计入字符数
            if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) {
                ++characters;
            }
        }
        const float text = 13.0f * static_cast<float>(characters);
        return std::min(text + 22.0f, 200.0f);
    };
    // Last three entries are controls. Long filename tags may consume only the
    // remaining space, so they cannot evict a control or change its hit identity.
    const std::size_t metadata_count = labels.size() > 3 ? labels.size() - 3 : 0;
    float metadata_width = 0.0f;
    float control_width = 0.0f;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        (i < metadata_count ? metadata_width : control_width) += label_width(labels[i]);
    }
    const float gaps = static_cast<float>(labels.empty() ? 0 : labels.size()-1)*kOptionGap;
    const float available = std::max(0.0f, row.width - control_width - gaps);
    const float metadata_scale = metadata_width > 0 ? std::min(1.0f, available/metadata_width) : 1.0f;
    float x = row.x;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const float width = label_width(labels[i]) * (i < metadata_count ? metadata_scale : 1.0f);
        if (x + width > row.right() + 0.01f && !rects.empty()) {
            break;   // 放不下就不排了（绘制层同样 break，两边的集合一致）
        }
        rects.push_back(Rect{x, row.y, width, row.height});
        x += width + kOptionGap;
    }
    return rects;
}

PanelLayout compute_panel_layout(const PanelMetrics& metrics) {
    PanelLayout layout;
    layout.dj_mode = metrics.dj_mode;
    layout.sfx_collapsed = metrics.sfx_collapsed;

    layout.panel = Rect{0.0f, 0.0f, metrics.width, metrics.height};
    // 用**局部副本**去切，不要切 layout.panel 本身 —— 否则它会被越切越小，
    // "某块是否在面板之内"这类判断就全错了（测试抓到的第一件事就是这个）。
    Rect column_area = layout.panel;
    layout.titlebar = take_top(column_area, metrics.titlebar_height);
    // 底栏也要**从 column_area 里吃掉**，不能只按坐标算一个矩形：
    // 少这一步，两列会一直伸到面板底部，曲库就压在底栏下面了（测试抓到过）。
    layout.busbar = take_bottom(column_area, metrics.busbar_height);
    const float footer_y = layout.busbar.y + 4.0f;
    layout.busbar_reload = Rect{metrics.width - 94.0f, footer_y, 82.0f, 26.0f};
    layout.busbar_change = Rect{metrics.width - 184.0f, footer_y, 82.0f, 26.0f};
    layout.busbar_fold = Rect{12.0f, footer_y, 30.0f, 26.0f};
    layout.busbar_show_all = Rect{50.0f, footer_y, 108.0f, 26.0f};
    layout.busbar_library = Rect{174.0f, footer_y, std::max(0.0f, layout.busbar_change.x - 12.0f - 174.0f), 26.0f};
    layout.title_pin = Rect{metrics.width - 108.0f, 6.0f, 28.0f, metrics.titlebar_height - 12.0f};
    layout.title_cn = Rect{metrics.width - 184.0f, 6.0f, 34.0f, metrics.titlebar_height - 12.0f};
    layout.title_en = Rect{metrics.width - 150.0f, 6.0f, 34.0f, metrics.titlebar_height - 12.0f};
    layout.title_minimize = Rect{metrics.width - 74.0f, 6.0f, 28.0f, metrics.titlebar_height - 12.0f};
    layout.title_close = Rect{metrics.width - 40.0f, 6.0f, 28.0f, metrics.titlebar_height - 12.0f};

    // 底栏的历史条：左半边两个按钮（原型里 hist 铺满左侧那一整行）
    {
        Rect hist = layout.busbar;
        hist.x += 10.0f;
        hist.width = at_least_zero(hist.width - 20.0f);
        // 右边留给"曲库位置"，所以历史条只吃左半
        hist.width = at_least_zero(hist.width * 0.55f);
        hist.y += 5.0f;
        hist.height = at_least_zero(hist.height - 10.0f);
        const float half = at_least_zero(hist.width * 0.5f - 3.0f);
        layout.bus_prev = Rect{hist.x, hist.y, half, hist.height};
        layout.bus_next = Rect{hist.x + half + 6.0f, hist.y, half, hist.height};
    }
    layout.body = column_area;   // 标题栏与底栏之间剩下的全部

    // ---- 两列按 flex 分 ----
    const float flex_total = metrics.bgm_flex + metrics.sfx_flex;
    const float bgm_column_width =
        flex_total > 0.0f ? metrics.width * metrics.bgm_flex / flex_total : metrics.width;
    layout.bgm_column = take_left(column_area, metrics.sfx_collapsed ? metrics.width : bgm_column_width);
    layout.sfx_column = column_area;

    // 两列的内部结构一样：列头 → （各自的台位）→ 曲库。
    // 抽成 lambda 是因为 BGM 与 SFX 只差"中间那块"。
    const auto lay_out_column = [&metrics](const Rect& column, Rect& head, Rect& title,
                                          Rect& volume, Rect& meter, Rect& clear_box,
                                          Rect& content_top) {
        Rect inner = column.inset(0.0f);
        inner.x += metrics.column_padding_x;
        inner.width = at_least_zero(inner.width - metrics.column_padding_x * 2.0f);
        inner.y += metrics.column_padding_top;
        inner.height =
            at_least_zero(inner.height - metrics.column_padding_top - metrics.column_padding_bottom);

        head = take_top(inner, metrics.column_head_height);
        title = head;
        // 标题后面跟一个"清空"小方块。
        // 位置要排在「标题 + 角标」之后：标题是 2 个汉字（约 30px），
        // 角标是 3 个字母（约 24px），所以 30 + 8 + 40 = 78 之后再留一点。
        // 原来写的是 92 —— 而当时角标按字节被算成 81px 宽，于是两者重叠，
        // 用户看到"清空按钮盖住了 BGM 字样"。角标的估算修好之后这个位置才对得上。
        clear_box = Rect{head.x + 104.0f, head.y + 1.0f, 24.0f, head.height - 2.0f};
        // 音量条与电平表贴右边：电平表在最右，音量条在它左边。
        // 切的是 head 的**副本** —— head 本身要保留成完整的列头矩形，
        // 否则"列头里有什么"就只剩标题那一小块了。
        Rect head_rest = head;
        meter = take_right(head_rest, metrics.level_meter_width);
        volume = take_right(head_rest, metrics.volume_slider_width + 8.0f);
        inner.y += metrics.column_head_gap;
        inner.height = at_least_zero(inner.height - metrics.column_head_gap);
        content_top = inner;
    };

    Rect bgm_content{};
    lay_out_column(layout.bgm_column, layout.bgm_head, layout.bgm_title,
                   layout.bgm_volume_slider, layout.bgm_meter, layout.bgm_clear, bgm_content);
    Rect sfx_content{};
    lay_out_column(layout.sfx_column, layout.sfx_head, layout.sfx_title,
                   layout.sfx_volume_slider, layout.sfx_meter, layout.sfx_clear, sfx_content);

    // ---- BGM 台 ----
    {
        // 台位高度 = 上下内边距 + max(唱片, 信息区)
        // 信息区 = 曲名 + 一行标签 + 音波图那一行
        const float info_height =
            metrics.bgm_deck_name_height + metrics.bgm_deck_row_gap + metrics.bgm_deck_line_height +
            metrics.bgm_deck_row_gap + metrics.bgm_waveform_height + metrics.bgm_deck_row_gap +
            metrics.volume_slider_height;
        const float deck_height = metrics.bgm_deck_padding_y * 2.0f +
                                  std::max(metrics.bgm_disc, info_height);
        Rect deck = take_top(bgm_content, deck_height);
        layout.bgm_deck = deck;

        Rect deck_inner = deck;
        deck_inner.x += metrics.bgm_deck_padding_x;
        deck_inner.width = at_least_zero(deck_inner.width - metrics.bgm_deck_padding_x * 2.0f);
        deck_inner.y += metrics.bgm_deck_padding_y;
        deck_inner.height = at_least_zero(deck_inner.height - metrics.bgm_deck_padding_y * 2.0f);

        // 唱片在左，信息在右。切的是副本。
        Rect deck_rest = deck_inner;
        Rect stage = take_left(deck_rest, metrics.bgm_disc);
        layout.bgm_disc = Rect{stage.x, stage.y, std::min(stage.width, metrics.bgm_disc),
                               std::min(stage.height, metrics.bgm_disc)};
        stage.width = at_least_zero(stage.width - metrics.bgm_disc);
        static_cast<void>(take_left(stage, metrics.bgm_deck_gap));   // 吃掉间隙

        Rect info = deck_rest;
        layout.bgm_deck_text = take_top(info, metrics.bgm_deck_name_height);
        info.y += metrics.bgm_deck_row_gap;
        layout.bgm_deck_line = take_top(info, metrics.bgm_deck_line_height);
        info.y += metrics.bgm_deck_row_gap;

        // 音波图那一行：左边时间、右边总时长，中间是波形
        Rect wave_row = take_top(info, metrics.bgm_waveform_height);
        layout.bgm_time_left = take_left(wave_row, 46.0f);
        layout.bgm_time_right = take_right(wave_row, 46.0f);
        static_cast<void>(take_left(wave_row, 6.0f));
        static_cast<void>(take_right(wave_row, 6.0f));
        layout.bgm_waveform = wave_row;
        info.y += metrics.bgm_deck_row_gap;

        // 音波图下面那行：上一首 / 下一首。
        // 这一行本来是空的（台位高度里留给它了），把历史条放这儿正好 ——
        // 用户明确要求"放到之前规划好的区域，音波图下方那块空闲处"。
        {
            Rect history_row = take_top(info, metrics.bgm_deck_line_height);
            const float half = at_least_zero(history_row.width * 0.5f - 4.0f);
            layout.bgm_prev = Rect{history_row.x, history_row.y, half, history_row.height};
            layout.bgm_next =
                Rect{history_row.x + half + 8.0f, history_row.y, half, history_row.height};
        }
        // 电平表那一行留给绘制层，布局上不需要单独的矩形

        // 唱针：挂在唱片右上角外侧
        layout.bgm_tonearm = Rect{layout.bgm_disc.right() - 26.0f, layout.bgm_disc.y - 6.0f, 22.0f,
                                  74.0f};
    }

    // ---- BGM 曲库 ----
    const auto lay_out_library = [&metrics](Rect& content, float gap, int rows, Rect& library,
                                           Rect& head_box,
                                           Rect& searchbox, Rect& icon, Rect& input,
                                           Rect& peek_area, Rect& peek_text, Rect& peek_wave,
                                           Rect& chips, Rect& grid) {
        content.y += gap;
        content.height = at_least_zero(content.height - gap);
        library = content;

        // 芯片区高度 = 行数 × 一行高 + 行距 + 上下留白。
        // 网格起点因此随行数自动下移 —— 不然多出来的行会压在第一排卡片上。
        const int safe_rows = rows > 0 ? rows : 1;
        const float rows_tall = static_cast<float>(safe_rows) * metrics.chip_height +
                                static_cast<float>(safe_rows - 1) * metrics.chip_row_gap;
        const float chips_area = rows_tall + metrics.chip_vertical_padding * 2.0f;

        Rect head_area = take_top(library, metrics.searchbox_height + metrics.lib_head_gap +
                                               chips_area);
        head_box = head_area;

        Rect box = take_top(head_area, metrics.searchbox_height);
        searchbox = box;   // 保留完整的盒子矩形（"输入框与预览在同一个盒子里"）

        // 切的是 box 的副本：searchbox 必须保持完整，否则它会被越切越小，
        // 后面"输入框是否在搜索框内"之类的判断就全部错位（测试抓到过）。
        Rect box_rest = box;
        icon = take_left(box_rest, metrics.searchbox_icon_width);
        input = take_left(box_rest, metrics.searchbox_input_width);
        // 预览区吃满剩下的宽度（这就是"输入框与预览在同一个盒子里"）
        peek_area = box_rest;

        // 名字在前、波形在后。右边距要**先**吃掉，否则后吃会把已经放好的
        // 波形挤出预览区，于是"波形在预览区内"就假了。
        Rect peek_rest = box_rest;
        static_cast<void>(take_right(peek_rest, 12.0f));            // 右边距
        peek_wave = take_right(peek_rest, metrics.peek_wave_width); // 波形贴右边
        static_cast<void>(take_right(peek_rest, 12.0f));            // 名字与波形之间
        static_cast<void>(take_left(peek_rest, 12.0f));             // 与输入框的分隔
        peek_text = peek_rest;   // 名字吃满剩下的宽度（在前）

        // box 已经被 take_top 吃掉了 searchbox_height，这里只再加"头与芯片之间的间距"。
        // 再加一次 searchbox_height 会让芯片整体下移 46px，压到第一行卡片上
        // （渲染出来一眼就能看见，但只看代码不容易发现 —— 这正是布局断言存在的理由）。
        head_area.y += metrics.lib_head_gap;
        chips = take_top(head_area, chips_area);
        // 留白让给容器，芯片本身从留白下面开始排（chip_rects 也按这个偏移排）
        chips.y += metrics.chip_vertical_padding;
        chips.height = rows_tall;

        // 网格视口：给滚动条让出宽度
        grid = library;
        grid.width = at_least_zero(grid.width - metrics.scrollbar_width);
    };

    Rect bgm_lib_head{};
    lay_out_library(bgm_content, metrics.library_gap, metrics.bgm_chip_rows, layout.bgm_library,
                    bgm_lib_head,
                    layout.bgm_searchbox, layout.bgm_search_icon, layout.bgm_search_input,
                    layout.bgm_peek_area, layout.bgm_peek_text, layout.bgm_peek_wave,
                    layout.bgm_chips, layout.bgm_grid);

    layout.sfx_mode = take_top(sfx_content, 30.0f);
    layout.sfx_mode.width = 176.0f;
    static_cast<void>(take_top(sfx_content, 8.0f));
    // ---- 音效台 2×2 ----
    if (!metrics.dj_mode) {
        // 台高要放得下：舞台 + 间隙 + 名字 + 间隙 + 音波图，再加上下内边距。
        // 只按"内边距 + 舞台"算会漏掉名字与音波图那一行 —— 它们会被压成 0 高
        // （原型里也发生过一次音波图消失，根因是 flex 方向的分配）。
        const float stage_column_height = metrics.sfx_stage + metrics.sfx_stage_wave_gap +
                                          metrics.sfx_name_height + metrics.sfx_stage_wave_gap +
                                          metrics.sfx_waveform_height;
        const float deck_height = metrics.sfx_deck_padding_y * 2.0f + stage_column_height;
        const float grid_height = deck_height * 2.0f + metrics.sfx_grid_gap;
        Rect deck_grid_area = take_top(sfx_content, grid_height);
        layout.sfx_deck_grid = deck_grid_area;

        const float deck_width = at_least_zero(
            (deck_grid_area.width - metrics.sfx_grid_gap) / 2.0f);
        for (std::size_t i = 0; i < sfx_slot_count; ++i) {
            const std::size_t row = i / 2;
            const std::size_t column = i % 2;
            const Rect deck{
                deck_grid_area.x + static_cast<float>(column) * (deck_width + metrics.sfx_grid_gap),
                deck_grid_area.y + static_cast<float>(row) * (deck_height + metrics.sfx_grid_gap),
                deck_width, deck_height};
            layout.sfx_decks[i] = deck;

            Rect inner = deck;
            inner.x += metrics.sfx_deck_padding_x;
            inner.width = at_least_zero(inner.width - metrics.sfx_deck_padding_x * 2.0f);
            inner.y += metrics.sfx_deck_padding_y;
            inner.height = at_least_zero(inner.height - metrics.sfx_deck_padding_y * 2.0f);

            // 布局顺序（与设计稿的 h{v{h{v{功能按钮},唱片},音波图},音量条} 一致）：
            //   最左：竖向的循环 / 淡入淡出按钮
            //   中间：唱片（上）与音波图（下）
            //   最右：竖向音量条
            const Rect left_column = take_left(inner, metrics.sfx_side_control_width);
            const Rect right_column = take_right(inner, metrics.sfx_side_control_width);
            static_cast<void>(take_left(inner, metrics.sfx_deck_gap * 0.5f));
            static_cast<void>(take_right(inner, metrics.sfx_deck_gap * 0.5f));

            // 左侧两个按钮上下分，整组垂直居中。
            // 高度用按钮的尺寸（26），不是音量条的 56 ——
            // 共用 56 会让两个按钮加起来超出内区，直接跑到台外（测试抓到的）。
            Rect left_stack = left_column;
            const float buttons_height =
                metrics.sfx_button_height * 2.0f + metrics.sfx_button_gap;
            left_stack.y += (left_stack.height - buttons_height) * 0.5f;
            layout.sfx_loop[i] = take_top(left_stack, metrics.sfx_button_height);
            left_stack.y += metrics.sfx_button_gap;
            layout.sfx_fade[i] = take_top(left_stack, metrics.sfx_button_height);

            // 右侧竖向音量条：长条，垂直居中
            Rect volume_stack = right_column;
            volume_stack.y += (volume_stack.height - metrics.sfx_volume_height) * 0.5f;
            layout.sfx_volumes[i] = take_top(volume_stack, metrics.sfx_volume_height);

            // 中间：唱片舞台在上、名字与音波图在下
            Rect middle = inner;
            layout.sfx_stages[i] = take_top(middle, metrics.sfx_stage);
            const Rect& stage = layout.sfx_stages[i];
            layout.sfx_discs[i] =
                Rect{stage.x + (stage.width - metrics.sfx_disc) * 0.5f,
                     stage.y + (stage.height - metrics.sfx_disc) * 0.5f, metrics.sfx_disc,
                     metrics.sfx_disc};

            middle.y += metrics.sfx_stage_wave_gap;
            layout.sfx_names[i] = take_top(middle, metrics.sfx_name_height);
            middle.y += metrics.sfx_stage_wave_gap;
            layout.sfx_waveforms[i] = take_top(middle, metrics.sfx_waveform_height);
        }
    }

    // ---- SFX 曲库 ----
    Rect sfx_lib_head{};
    lay_out_library(sfx_content, metrics.library_gap, metrics.sfx_chip_rows, layout.sfx_library,
                    sfx_lib_head,
                    layout.sfx_searchbox, layout.sfx_search_icon, layout.sfx_search_input,
                    layout.sfx_peek_area, layout.sfx_peek_text, layout.sfx_peek_wave,
                    layout.sfx_chips, layout.sfx_grid);

    // 派生尺寸：绘制与命中测试都用这里的值，避免各处再算一遍
    layout.chip_height = metrics.chip_height;
    layout.chip_row_gap = metrics.chip_row_gap;
    layout.chip_vertical_padding = metrics.chip_vertical_padding;

    layout.track_card_height = metrics.track_badge + metrics.track_card_padding_y * 2.0f;
    layout.track_grid_gap = metrics.track_grid_gap;
    layout.track_card_min_width = metrics.track_card_min_width;
    // 卡片宽度：网格宽度按列数均分（与 card_rect 同一算法，别各算一份）
    {
        const std::size_t columns = layout.card_columns(layout.bgm_grid);
        const float usable =
            layout.bgm_grid.width - static_cast<float>(columns > 0 ? columns - 1 : 0) *
                                        layout.track_grid_gap;
        layout.track_card_width =
            columns > 0 ? at_least_zero(usable / static_cast<float>(columns)) : 0.0f;
    }
    layout.bgm_ring_width = metrics.bgm_ring_width;
    layout.scrollbar_width = metrics.scrollbar_width;
    // 芯片基准宽度：名字长度未知，给一个"三四个汉字 + 内边距"的保守值。
    // 它是横向滚动的一行，超出被裁掉是设计内的行为。
    layout.chip_width = 12.0f * 4.0f + 11.0f * 2.0f;

    // Hidden section has no visible or clickable geometry, including its mode buttons.
    if (metrics.sfx_collapsed) {
        layout.sfx_column = {}; layout.sfx_head = {}; layout.sfx_title = {};
        layout.sfx_volume_slider = {}; layout.sfx_meter = {}; layout.sfx_clear = {};
        layout.sfx_mode = {}; layout.sfx_deck_grid = {}; layout.sfx_library = {};
        layout.sfx_searchbox = {}; layout.sfx_search_icon = {}; layout.sfx_search_input = {};
        layout.sfx_peek_area = {}; layout.sfx_peek_text = {}; layout.sfx_peek_wave = {};
        layout.sfx_chips = {}; layout.sfx_grid = {};
        layout.sfx_decks = {}; layout.sfx_stages = {}; layout.sfx_discs = {}; layout.sfx_names = {};
        layout.sfx_loop = {}; layout.sfx_fade = {}; layout.sfx_waveforms = {}; layout.sfx_volumes = {};
    }

    return layout;
}

}  // namespace musicboard::ui
