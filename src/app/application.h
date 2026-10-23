#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>   // HDROP（拖入文件）

#include <array>
#include <deque>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/envelope_worker.h"
#include "core/failure.h"
#include "core/product_settings.h"
#include "library/library_scanner.h"
#include "library/library_loader.h"
#include "library/import_job.h"
#include "library/online_job.h"
#include "platform/link_import_window.h"
#include "platform/composition_window.h"
#include "platform/global_hotkeys.h"
#include "platform/orb_window.h"
#include "ui/panel_layout.h"
#include "ui/orb_view.h"
#include "ui/panel_view.h"
#include "ui/track_label.h"
#include "ui/disc_motion.h"

namespace musicboard::app {

// ============================================================================
// 正式程序
//
// 把已经做好的各层接起来：设置 → 曲库 → 窗口 → 音频引擎 → 全局快捷键 → 消息循环。
//
// 交互全部由 HitMap 决定（命中区域来自面板布局），绘制全部由 PanelView 做。
// 这个类只负责"谁在什么时候被调用"以及"状态怎么变"。
//
// 三条与设计稿对应的行为约定：
//   · 单击曲目卡只选中，**不播放** —— 跑团现场最怕误触发
//   · 点击唱片本体 = 暂停/继续；空盘上点击没有反应
//   · 拖音效台音波图立即 seek，拖 BGM 音波图**松手才 seek**
//     （连续 seek 会造成爆音；拖动期间还要让进度刷新停手，
//      否则定时器会按旧位置重画、与拖拽预览互相抢，看起来就是"松手时闪两下"）
// ============================================================================

// 曲库里的一首歌 + 它的包络（按需加载）
struct LibraryEntry final {
    LibraryTrack track;
    ui::TrackLabel label;
    std::string category;
    std::vector<float> envelope;
    bool envelope_attempted = false;
    // 属于哪一栏。两栏共用同一个条目数组，索引因此是全局的 ——
    // ensure_envelope / play / trigger 这些只认索引的地方一行都不用改。
    bool is_sfx = false;
    std::shared_ptr<const PreparedAudio> prepared;
    Failure preload_failure;
};

class Application final {
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // library_override 为空则用设置里记的曲库目录
    [[nodiscard]] bool initialize(const std::filesystem::path& library_override, Failure& failure);

    // 建窗口 + 注册快捷键 + 建通知区域图标。失败时返回 false 并说明原因。
    [[nodiscard]] bool create_windows(Failure& failure);

    int run();

    // 交互自检：用**真实的事件处理函数**驱动一遍主要交互，逐条断言结果。
    //
    // 为什么要在程序内部做这件事：交互是这一层唯一没有测试覆盖的部分
    // （布局有断言、绘制有像素证据），而它恰恰是最容易"看着对、用着不对"的地方。
    // 自检走的是与真人操作完全相同的代码路径（on_left_down → 命中 → 动作），
    // 所以它能证明"接线通了"，而不是"函数写对了"。
    //
    // 需要音频设备的条目在设备不可用时 SKIP 而不是 FAIL ——
    // 测试的成败不该取决于跑测试的机器有没有声卡。
    [[nodiscard]] int self_test();
    [[nodiscard]] int self_test_display();
    [[nodiscard]] int self_test_online(const std::string& url);

    // 给自检用
    [[nodiscard]] const ui::PanelState& panel_state() const noexcept { return state_; }
    [[nodiscard]] ui::PanelState& panel_state() noexcept { return state_; }
    [[nodiscard]] const ui::PanelLayout& layout() const noexcept { return layout_; }
    // 球上部关闭时立即隐藏；其他入口保留收起过渡。
    void set_panel_visible(bool visible, bool immediate = false);
    [[nodiscard]] bool toggle_panel_topmost(Failure& failure);

    // 这个客户区坐标是不是落在标题栏的**按钮**上。
    //
    // 窗口层要用它决定 WM_NCHITTEST 返回什么：整条标题栏返回 HTCAPTION
    // （拖动走系统），但那会把三个按钮一起吞掉 —— 画得出来、点不着。
    // 所以按钮所在的位置必须返回 HTCLIENT，让点击真的到得了这里。
    [[nodiscard]] bool is_titlebar_button_point(POINT client) const noexcept;
    [[nodiscard]] bool panel_visible() const noexcept { return panel_visible_; }

private:
    // 曲库分两份：BGM 与 SFX。它们各自有分类、筛选结果、滚动位置与搜索词。
    // 原来只有一份，于是两列画的是同一批曲目（音效列里显示音乐）。
    struct Column final {
        // 这一列里可见的条目（entries_ 的下标），已按分类与搜索筛过
        std::vector<std::size_t> visible;
        std::vector<std::string> categories;
        ui::ScrollState scroll;
        int active_chip = -1;              // -1 = 全部
        std::string search_text;
        bool search_focused = false;
        double search_edit_time = 0;
    };

    // ---- 窗口消息（由 PanelWindow 转发进来）----
    friend class PanelWindow;
    [[nodiscard]] bool handle_message(UINT message, WPARAM wparam, LPARAM lparam,
                                     LRESULT& result);

    // ---- 帧 ----
    void on_frame();
    void sync_state();
    void build_regions();

    // ---- 输入 ----
    [[nodiscard]] ui::Point to_ui_point(POINT client) const noexcept;
    // 按光标下面的东西给指针形状（拖动中 / 可拖的卡 / 可点的控件 / 其余）
    [[nodiscard]] HCURSOR cursor_for_client_point(POINT client) const;
    // 鼠标消息追踪（MUSICBOARD_TRACE_INPUT=1 时写日志）。
    // "点下去没反应"这类问题没有它就只能靠猜，而日志本来就是给这种情况准备的。
    void trace_input(const char* what, POINT client) const;
    [[nodiscard]] static bool input_trace_enabled();
    void on_mouse_move(POINT client);
    // 拖动音量条：按命中 id 决定改哪一个（全局 / 音效台某一路）
    void apply_slider_drag(POINT client);
    // 把某一段滚进视野（上一首/下一首点完之后要把当前曲目滚出来）
    void scroll_visible_into_view(std::size_t index);
    void on_left_down(POINT client);
    void on_left_up(POINT client);
    void on_right_up(POINT client);
    void on_wheel(POINT client, int delta);
    void on_char(wchar_t character);
    void on_key_down(UINT virtual_key);
    void on_drop_files(HDROP drop);
    void show_link_import();
    [[nodiscard]] bool start_link_import(const LinkImportInput& input, Failure& failure);
    void pump_link_import();

    // ---- 播放 ----
    void ensure_envelope(std::size_t index);
    // 收后台算好的包络（每帧一次，绝不阻塞）
    void collect_envelopes();
    [[nodiscard]] bool play_bgm(std::size_t index);
    // 后台解码的收集与起播（界面线程从不解码，见 audio/envelope_worker.h）
    void collect_pending_playback();
    void cancel_pending_playback(int deck, std::size_t keep_index = static_cast<std::size_t>(-1));
    void queue_playback(std::size_t index, int deck);
    void clear_playback(bool is_sfx, bool fade = false);
    void cancel_pointer_gesture();

    // 真正把（已就绪的）音频交给引擎
    [[nodiscard]] bool start_playback(std::size_t index, const DecodedAudio& audio,
                                      std::size_t sfx_deck, bool is_sfx);
    void toggle_bgm_pause();
    void step_bgm(int direction);
    [[nodiscard]] bool trigger_sfx(std::size_t deck, std::size_t index);
    void seek_bgm(double seconds);
    void commit_pending_seek();
    void apply_volume_changes();

    void begin_library_load(const std::filesystem::path& root, bool startup = false);
    void collect_library_load();
    void choose_new_library();
    [[nodiscard]] bool trigger_pad(std::size_t index);
    void set_dj_mode(bool enabled);
    void set_sfx_collapsed(bool collapsed);
    void set_show_all_categories(bool enabled);
    void set_ui_language(Language language);
    struct RecentBgm final { std::filesystem::path path; std::size_t count=0, order=0; };
    std::vector<RecentBgm> recent_bgm_;
    std::size_t recent_order_ = 0;
    std::vector<std::size_t> recent_menu_indices_;
    [[nodiscard]] std::vector<std::size_t> ranked_recent_bgm() const;
    [[nodiscard]] bool create_tray_menu(HMENU& menu, Failure& failure);

    // ---- 曲库 ----
    // 把一个本地文件导入曲库（复制到暂存区 → 校验 → 原子发布 → 记进内存模型）。
    // 拖放与自检都走它 —— 一份逻辑一个入口。
    // 失败时 problem 里是一句可显示的原因。
    [[nodiscard]] bool import_local_file(const std::filesystem::path& source,
                                        const std::string& category, bool as_sfx,
                                        std::string& problem);

    // 重算某一列里可见的条目
    void rebuild_visible(Column& column, bool section_is_sfx);
    // 按实测的芯片宽度算出行数；行数变了就重算布局（网格起点会跟着下移）
    void update_chip_rows();
    // 按当前布局重算两列的滚动几何（视口高 / 内容高）。
    //
    // 为什么必须单独有这一步：滚动几何是在 rebuild_visible 里设的，而它只在
    // "曲库变了 / 换了分类 / 搜了字"时被调用 —— 首次启动时布局还没算出来
    // （create_windows 才建布局），于是视口高和内容高都是 0，滚动范围是 0。
    // 表现就是用户说的"只有切换一次标签之后才能滚"。
    void refresh_scroll_geometry();
    // 光标所在的那一列（按几何判断，不按命中 id）
    [[nodiscard]] Column& column_at_point(ui::Point point) noexcept;


    // ---- 命中编号 → 归属 ----
    //
    // 两列的编号段**必须错开**，否则命中与悬停会串到隔壁列。每个 id 都要能
    // 反解出"哪一列 + 段内第几个"，而不是处处硬写 "target - 1000" ——
    // 硬写正是之前右列显示音乐、悬停跟着左列走的原因。
    static constexpr int kChipBase = 400;
    static constexpr int kSfxChipBase = 500;
    static constexpr int kCardBase = 1000;
    static constexpr int kSfxCardBase = 2000;
    static constexpr int kSearchIdBgm = 300;
    static constexpr int kSearchIdSfx = 301;

    [[nodiscard]] static bool section_of_id(int id) noexcept;   // true = 属于音效列
    // id 是芯片 → 填 is_sfx/index，返回 true
    [[nodiscard]] static bool chip_of_id(int id, bool& is_sfx, int& index) noexcept;
    // id 是曲目卡 → 填 is_sfx/index，返回 true
    [[nodiscard]] static bool card_of_id(int id, bool& is_sfx, int& index) noexcept;
    [[nodiscard]] Column& column_of_id(int id) noexcept;        // 该 id 属于哪一列
    [[nodiscard]] Column& active_search_column() noexcept;      // 现在有焦点的那一列
    void set_status(std::string text);



    // ---- 动效 ----
    //
    // 悬停过渡：每张卡 / 每枚芯片一个 0..1 的量，按**真实流逝时间**推进
    // （dt 由两个绝对时刻相减得到，所以掉帧不会让过渡变慢或变快）。
    // 原型是 120ms 缓出；这里推进的是线性量，缓出曲线在绘制层做。
    void update_hover_animations(float dt_seconds);
    // 面板显隐：0..1 的进度 + "正在收起时窗口还不能真的藏起来"。
    [[nodiscard]] float reveal_progress() const;
    void begin_reveal(bool showing);
    void update_reveal();
    // 屏幕上还有没有东西在动（悬停过渡 / 显隐 / 唱针）。有就继续重画 ——
    // 少了这一步，动效会停在半路，看起来就像"卡住了"。
    [[nodiscard]] bool animations_active() const noexcept;

    void set_search(std::string text);

    // ---- 快捷键改键 ----
    //
    // 冲突是这个功能的常态（默认组合可能被别的程序占用），所以必须能在界面里改，
    // 而不是让用户去手编 settings.conf。捕获方式很直接：进捕获态时**先把自己的
    // 全局快捷键全撤掉**，然后把下一次按下的"修饰键 + 普通键"当新绑定 ——
    // 不撤的话，用户按下的那一组可能正好是已注册的，于是同时触发了音效。
    void begin_hotkey_capture(int slot);
    void cancel_hotkey_capture();
    [[nodiscard]] bool commit_hotkey_capture(UINT virtual_key);
    void re_register_hotkeys();
    [[nodiscard]] std::string hotkey_menu_label(int slot) const;

    // ---- 通知区域 ----
    [[nodiscard]] bool create_tray_icon(Failure& failure);
    void destroy_tray_icon();
    void show_tray_menu(POINT screen_point);

    // ---- 设置 ----
    void save_settings();
    [[nodiscard]] std::vector<HotkeyBinding> current_hotkey_bindings() const;

    // ---- 数据 ----
    std::vector<LibraryEntry> entries_;    // 两栏共用的条目数组
    Column bgm_;                           // 音乐列
    Column sfx_;                           // 音效列
    std::filesystem::path library_root_;
    LinkImportWindow link_import_window_;
    std::unique_ptr<ImportJob> link_import_job_;
    std::unique_ptr<OnlineJob> online_job_;
    LinkImportInput link_import_input_;
    bool link_import_active_ = false;
    std::string link_import_status_;
    std::filesystem::path settings_path_;

    LibraryLoader loader_;
    bool loading_ = false;
    bool startup_loading_ = true;
    std::string load_details_;
    ProductSettings product_;
    AudioEngine engine_;
    bool audio_ready_ = false;
    // 包络在后台算（悬停时**绝不**解码，见 audio/envelope_worker.h）
    EnvelopeWorker envelopes_;
    // 排过队的下标：后台算完之后由界面线程来取（回调进界面线程太容易踩生命周期）
    std::vector<std::size_t> envelope_watch_;
    // 等着后台解码好之后起播的任务。
    //
    // 用**队列**而不是单个槽位：连着请求两次（快捷键连按、先放 A 再放 B）时，
    // 单槽位会把前一个悄悄丢掉 —— 表现就是"按了没反应"，而且日志里什么都看不到。
    struct PendingPlay final {
        std::size_t index = 0;
        int sfx_deck = -1;   // -1 = BGM
    };
    std::deque<PendingPlay> pending_plays_;


    // ---- 界面 ----
    ui::PanelMetrics metrics_{};
    ui::PanelLayout layout_{};
    ui::PanelState state_{};
    ui::PanelView view_;
    // 悬浮球的绘制（原来那段回调是空的 —— 球窗口存在但什么都不画，
    // 于是用户看到的是"我的悬浮球呢？"）
    ui::OrbView orb_view_;
    bool orb_view_ready_ = false;
    ui::OrbViewState orb_state_{};
    std::array<ui::DiscMotion, 1 + sfx_slot_count> disc_motion_{};
    std::array<ui::FadeMotion, 1 + sfx_slot_count> disc_hover_{};
    ui::FadeMotion orb_hover_;
    int orb_hover_zone_ = 0;
    // 上一次画球用过的状态，用来判断"还要不要再画一帧"
    float orb_last_progress_ = -1.0f;
    float orb_last_amplitude_ = -1.0f;
    bool orb_last_playing_ = false;
    // 球的第一帧必须**主动**画一次。
    // 面板是靠 dirty_ 驱动的，球没有自己的帧循环 —— 不主动画的话，
    // 那块合成表面一直是空的（窗口存在、命中有效、但完全透明），
    // 这正是"我的悬浮球呢"的成因。
    // 头一秒内每帧都画：窗口的第一次呈现可能发生在我们第一次 redraw 之后，
    // 只画一次的话内容会被丢掉（实测空闲时球面全黑，播起来才有画面）。
    int orb_warmup_frames_ = 90;
    ui::HitMap hit_;
    ui::ScrollState bgm_scroll_{};
    bool view_ready_ = false;

    CompositionWindow panel_;
    OrbWindow orb_;
    GlobalHotkeySet hotkeys_;
    std::array<std::string, sfx_slot_count> hotkey_labels_{};

    // ---- 交互状态 ----
    int active_chip_unused_ = -1;
    std::string search_text_unused_;
    bool search_focused_unused_ = false;
    bool panel_visible_ = false;
    bool tray_ready_ = false;
    HICON tray_icon_ = nullptr;

    bool dragging_ = false;
    // 拖动的是哪一条（entries_ 的全局下标）
    bool drag_from_sfx_ = false;
    // 按下时光标在卡片内的偏移（逻辑坐标）——幽灵按它对齐抓取点
    ui::Point drag_grab_{};
    // 被拖那张卡片的实际尺寸（两列宽度不同，幽灵必须按它自己的那一列来）
    ui::Point drag_size_{};
    std::size_t drag_index_ = 0;
    POINT drag_press_{};
    POINT cursor_client_{};        // 当前光标（客户区坐标）
    bool drag_moved_ = false;

    int capturing_slot_ = -1;              // >=0 表示正在等用户按下新快捷键
    int seek_slot_ = -1;                   // -1 = BGM，>=0 = 音效台
    bool seeking_ = false;
    double pending_seek_seconds_ = -1.0;   // < 0 表示没有待提交的 seek

    std::size_t selected_sfx_index_ = static_cast<std::size_t>(-1);
    std::size_t bgm_index_ = 0;
    std::size_t bgm_slot_ = 0;
    bool bgm_paused_ = false;
    double bgm_offset_ = 0.0;

    std::size_t sfx_index_[sfx_slot_count] = {};
    bool sfx_has_track_[sfx_slot_count] = {};

    std::string status_;
    bool dirty_ = true;
    unsigned frame_counter_ = 0;
    unsigned redraw_failure_count_ = 0;
    // 上一次画出去的进度（用来判断"进度变了没"，避免每帧都重画整块）
    double last_drawn_progress_ = -1.0;
    std::chrono::steady_clock::time_point started_at_{};
    struct UpdateChecks;
    std::unique_ptr<UpdateChecks> update_checks_;

    // 动效状态
    std::vector<float> bgm_hover_amounts_;
    std::vector<float> sfx_hover_amounts_;
    std::vector<float> bgm_chip_amounts_;
    std::vector<float> sfx_chip_amounts_;
    std::chrono::steady_clock::time_point last_frame_stamp_{};
    bool have_last_frame_stamp_ = false;
    std::chrono::steady_clock::time_point last_layer_repair_{};
    // 显隐动效：起点时刻 + 方向。收起到 0 之后才真的把窗口藏起来 ——
    // 否则"收进通知区域"会变成瞬间消失（原型里是淡出 + 缩小）。
    std::chrono::steady_clock::time_point reveal_stamp_{};
    bool reveal_showing_ = true;
    bool panel_hide_pending_ = false;
    // 真的退出（托盘菜单 / 球的右键菜单里那一项）。
    // 少了这个开关，"退出"发出的 WM_CLOSE 会被当成"收进通知区域" ——
    // 菜单里点了退出却关不掉，用起来就像关不掉的流氓软件。
    bool quitting_ = false;
    // 唱针过渡量（0 = 抬起，1 = 落下）。按**状态变化时刻**推进，
    // 原来用的是"从启动算起的秒数"，于是启动 0.25 秒之后它就已经到位并且
    // 再也不动了 —— 唱针动画实际上从来没演过。
    float bgm_arm_amount_ = 0.0f;
    std::array<float, sfx_slot_count> sfx_arm_amounts_{};
    // 每列的滚动目标（平滑滚动用；绘制用的是 ScrollState 的当前偏移）
    float bgm_scroll_target_ = 0.0f;
    float sfx_scroll_target_ = 0.0f;
    // 正在拖的音量条（命中 id；-1 = 没在拖）
    int slider_drag_target_ = -1;
    // 播放历史（上一首/下一首）。把 entry 下标按播放顺序记下来，
    // 与原型里的 hist 一致 —— "上一首"是"我前面放过的那首"，不是"列表里的前一首"。
    std::vector<std::size_t> history_;
    std::ptrdiff_t history_index_ = -1;



};

}  // namespace musicboard::app
