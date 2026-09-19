// 交互自检
//
// 这一层唯一没有覆盖的测试是"交互"：布局有几何断言、绘制有像素证据，
// 而"点了到底有没有反应"只能靠人点。这个自检用**真实的事件处理函数**
// （on_left_down → 命中 → 动作）驱动一遍主要交互，逐条断言结果 ——
// 走的是与真人操作完全相同的代码路径。
//
// 需要音频设备的条目在设备不可用时 SKIP 而不是 FAIL：
// 测试的成败不该取决于跑测试的机器有没有声卡。

#include "app/application.h"

#include <cmath>
#include <cstdio>

#include "audio/waveform.h"
#include "core/path_utf8.h"
#include "core/single_instance.h"
#include "library/import_transaction.h"
#include "library/library_ops.h"

namespace musicboard::app {

// 面板窗口的实例由 application.cpp 持有；自检需要问它"窗口真的可见吗"
extern bool panel_window_visible();
extern bool panel_window_has_handle();
extern bool orb_window_handle_exists();
extern void* orb_window_handle();

namespace {

// 写一个最小的合法 WAV：8kHz 单声道 16bit。
// 自带一份而不是引入测试脚手架 —— 正式程序不该依赖测试头文件。
bool write_selftest_wav(const std::filesystem::path& path, std::uint32_t duration_ms) {
    constexpr std::uint32_t sample_rate = 8000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits = 16;
    constexpr std::uint16_t block_align = channels * bits / 8;
    const std::uint32_t frames = sample_rate * duration_ms / 1000;
    const std::uint32_t data_size = frames * block_align;

    std::vector<std::uint8_t> bytes;
    const auto push32 = [&bytes](std::uint32_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    };
    const auto push16 = [&bytes](std::uint16_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    };
    const auto tag = [&bytes](const char* text) {
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<std::uint8_t>(text[i]));
        }
    };
    tag("RIFF");
    push32(36 + data_size);
    tag("WAVE");
    tag("fmt ");
    push32(16);
    push16(1);
    push16(channels);
    push32(sample_rate);
    push32(sample_rate * block_align);
    push16(block_align);
    push16(bits);
    tag("data");
    push32(data_size);
    for (std::uint32_t i = 0; i < frames; ++i) {
        push16(((i / 4) % 2 == 0) ? 8000 : 1000);
    }

    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                nullptr);
    ::CloseHandle(file);
    return ok != FALSE && written == bytes.size();
}

// 递归删一个目录（自检收尾用）
void remove_tree(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

}  // namespace

int Application::self_test() {
    int total = 0;
    int failures = 0;
    int skipped = 0;
    const auto check = [&](bool ok, const std::string& label) {
        ++total;
        if (ok) {
            std::printf("PASS %s\n", label.c_str());
        } else {
            ++failures;
            std::printf("FAIL %s\n", label.c_str());
        }
    };
    const auto check_detail = [&](bool ok, const std::string& label, const std::string& detail) {
        ++total;
        if (ok) {
            std::printf("PASS %s\n", label.c_str());
        } else {
            ++failures;
            std::printf("FAIL %s  [%s]\n", label.c_str(), detail.c_str());
        }
    };
    const auto skip = [&](const std::string& label) {
        ++skipped;
        std::printf("SKIP %s\n", label.c_str());
    };

    // 命中区域是在 on_frame 里登记的，所以每次模拟输入之前都要先跑一帧
    // （真人操作时也是这个顺序：输入到了用上一帧的区域判定）。
    const auto frame = [this] { on_frame(); };

    // 等某一台真的开始出声。
    //
    // 播放现在是**异步**的：界面线程只排任务，解码在后台（不然放置音乐时界面会
    // 冻结一下）。所以"拖上去之后立刻断言在播"已经不对了 —— 要等它起播。
    // 等的上限给足（每帧 10ms，最多 ~3 秒），超时才算失败。
    const auto wait_playing = [this, &frame](AudioBus bus, std::size_t slot) {
        for (int i = 0; i < 300; ++i) {
            if (audio_ready_ && engine_.slot_state(bus, slot).in_use) {
                return true;
            }
            ::Sleep(10);
            frame();
        }
        return false;
    };
    // 等**指定的那一条**真的在放。
    // 只看"这一台在响"是不够的：上一首还在响的时候它会立刻通过，
    // 于是"第二首没起来"这种问题被掩盖掉（自检里就是这么误判过一次）。
    const auto wait_playing_index = [this, &frame](AudioBus bus, std::size_t slot,
                                                   std::size_t index) {
        for (int i = 0; i < 300; ++i) {
            const bool is_current = bus == AudioBus::bgm
                                        ? bgm_index_ == index
                                        : (slot < sfx_slot_count && sfx_index_[slot] == index);
            if (audio_ready_ && is_current && engine_.slot_state(bus, slot).in_use) {
                return true;
            }
            ::Sleep(10);
            frame();
        }
        return false;
    };

    // 等某一台变成"暂停"（点唱片的暂停也是同一条异步路径上的后续动作）
    const auto wait_paused = [this, &frame](AudioBus bus, std::size_t slot) {
        for (int i = 0; i < 300; ++i) {
            if (engine_.slot_state(bus, slot).paused) {
                return true;
            }
            ::Sleep(10);
            frame();
        }
        return false;
    };

    const auto preload_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (loading_ && std::chrono::steady_clock::now() < preload_deadline) {
        frame(); ::Sleep(10);
    }
    check(!loading_, "集中预加载完成后进入交互验收");
    // Legacy playback scenarios intentionally use the complete library.
    // Default-off and index remapping are covered by --selftest-display.
    set_show_all_categories(true);
    bgm_.active_chip = -1; sfx_.active_chip = -1;
    rebuild_visible(bgm_, false); rebuild_visible(sfx_, true); frame();
    check(panel_visible_, "启动加载完成后面板默认保持开启");
    set_panel_visible(true);
    set_sfx_collapsed(false);
    set_dj_mode(false);
    frame();
    check(!bgm_.visible.empty(), "曲库里有可见曲目（否则下面的交互都无从谈起）");
    if (bgm_.visible.empty()) {
    std::printf("---- 交互自检: %d/%d 通过，%d 跳过 ----\n", total - failures, total, skipped);
        return 1;
    }

    // ---- 1. 搜索框：点它获得焦点；输入汉字；退格按字符删；Esc 清空 ----
    {
        frame();
        const POINT point{static_cast<LONG>(layout_.bgm_search_input.x + 4.0f),
                          static_cast<LONG>(layout_.bgm_search_input.center().y)};
        on_left_down(point);
        on_left_up(point);
        frame();
        check(bgm_.search_focused, "点击搜索框获得焦点");

        const std::size_t before = bgm_.visible.size();

        on_char(L'主');
        frame();
        check(!bgm_.search_text.empty(), "输入字符进入搜索框");
        check(bgm_.visible.size() <= before, "搜索之后可见曲目不多于之前");

        // 真机上按退格会**同时**产生 WM_KEYDOWN(VK_BACK) 与 WM_CHAR('')，
        // 而程序只在 WM_CHAR 里处理（两边都处理会一次删掉两个字符）。
        // 所以这里必须走 WM_CHAR 那条路 —— 用 VK_BACK 测的是另一条不存在的入口。
        on_char(L'');
        frame();
        check(bgm_.search_text.empty(), "退格删掉整个汉字（按字符删，不是按字节删）");

        on_char(L'测');
        on_char(L'试');
        frame();
        check(bgm_.search_text == "测试", "连输两个汉字得到「测试」");

        on_key_down(VK_ESCAPE);
        frame();
        check(bgm_.search_text.empty() && !bgm_.search_focused, "Esc 清空搜索并取消焦点");
        check(bgm_.visible.size() == before, "清空之后可见曲目恢复");
    }

    // ---- 2. 单击曲目卡：只选中，不播放 ----
    {
        frame();
        const ui::Rect card = layout_.card_rect(layout_.bgm_grid, 0, bgm_.scroll.offset());
        const POINT point{static_cast<LONG>(card.center().x), static_cast<LONG>(card.center().y)};

        const bool playing_before = audio_ready_ && engine_.slot_state(AudioBus::bgm, bgm_slot_).playing;
        on_left_down(point);
        on_left_up(point);   // 没有移动 → 算点击
        frame();
        const bool playing_after = audio_ready_ && engine_.slot_state(AudioBus::bgm, bgm_slot_).playing;
        check(playing_after == playing_before, "单击曲目卡**不**播放（跑团现场最怕误触发）");
        check(status_.find("已选中") != std::string::npos, "状态栏说明只是选中了");
    }

    // ---- 3. 拖动曲目卡到 BGM 唱片 ----
    if (!audio_ready_) {
        skip("拖动曲目卡到唱片上播放（本机没有可用音频设备）");
        skip("点击唱片暂停并保持位置（同上）");
        skip("快捷键触发音效台（同上）");
        skip("音效台的循环开关（同上）");
    } else {
        frame();
        const ui::Rect card = layout_.card_rect(layout_.bgm_grid, 0, bgm_.scroll.offset());
        const POINT from{static_cast<LONG>(card.center().x), static_cast<LONG>(card.center().y)};
        const POINT to{static_cast<LONG>(layout_.bgm_disc.center().x),
                       static_cast<LONG>(layout_.bgm_disc.center().y)};

        on_left_down(from);
        on_mouse_move(POINT{from.x + 40, from.y + 20});   // 超过阈值才算拖动
        on_mouse_move(to);
        frame();
        check(dragging_, "按下并移动之后进入拖动状态");
        on_left_up(to);
        frame();

        check(wait_playing_index(AudioBus::bgm, bgm_slot_, drag_index_),
              "拖到唱片上之后 BGM 台上开始播放了");
        const VoiceSlotState slot = engine_.slot_state(AudioBus::bgm, bgm_slot_);
        // 注意断言的是**界面说的那段话**，而不是引擎的 slot.playing：
        // 后者要求 current_gain > 0，而淡入从 0 开始，所以刚拖上去那一瞬间它是假的。
        // 界面按"在台上且没暂停"来显示播放中 —— 断言要跟着界面的口径走，
        // 否则测的是另一个问题（而且会得出"界面错了"的错误结论）。
        frame();
        check(!slot.paused && state_.bgm.playing, "并且界面显示为正在播放（淡入期间也是）");
        check(!dragging_, "松手之后拖动状态结束");

        // 淡入要真的在爬升：等一小会儿再看增益
        ::Sleep(200);
        frame();
        const float gain_after = engine_.slot_state(AudioBus::bgm, bgm_slot_).current_gain;
        check_detail(gain_after > 0.0f, "淡入真的把增益从 0 抬起来了",
                     std::to_string(gain_after));

        // ---- 4. 点击唱片暂停/继续，位置要保住 ----
        frame();
        const POINT disc{static_cast<LONG>(layout_.bgm_disc.center().x),
                         static_cast<LONG>(layout_.bgm_disc.center().y)};
        ::Sleep(300);   // 先让它播一会儿，这样"位置"不是 0
        frame();
        const double position_before = engine_.slot_state(AudioBus::bgm, bgm_slot_).position_seconds;

        on_left_down(disc);
        on_left_up(disc);
        frame();
        check(wait_paused(AudioBus::bgm, bgm_slot_), "点击唱片 → 暂停");
        const VoiceSlotState paused = engine_.slot_state(AudioBus::bgm, bgm_slot_);
        check(paused.in_use, "暂停之后曲目还在台上（不是被清掉了）");

        on_left_down(disc);
        on_left_up(disc);
        frame();
        for (int i = 0; i < 50 && engine_.slot_state(AudioBus::bgm, bgm_slot_).paused; ++i) {
            ::Sleep(10);
            frame();
        }
        const VoiceSlotState resumed = engine_.slot_state(AudioBus::bgm, bgm_slot_);
        check(!resumed.paused, "再点一下 → 继续");
        check_detail(std::fabs(resumed.position_seconds - position_before) < 0.5,
                     "继续时位置没有被重置（从暂停处接着播）",
                     "暂停前 " + std::to_string(position_before) + "，继续后 " +
                         std::to_string(resumed.position_seconds));

        // ---- 5. 快捷键触发音效台（走 WM_HOTKEY 同一条入口）----
        frame();
        const std::vector<HotkeyBinding> bindings = current_hotkey_bindings();
        if (bindings.empty()) {
            skip("快捷键触发音效台（没有配置快捷键）");
        } else {
            const std::size_t deck = static_cast<std::size_t>(bindings.front().id);
            LRESULT result = 0;
            static_cast<void>(
                handle_message(WM_HOTKEY, static_cast<WPARAM>(bindings.front().id), 0, result));
            frame();
            check(deck < sfx_slot_count && wait_playing(AudioBus::sfx, deck),
                  "快捷键让音效台 " + std::to_string(deck + 1) + " 开始发声");
        }

        // ---- 6. 音效台的循环开关 ----
        frame();
        {
            const bool before = product_.sfx_slots[0].loop;
            const ui::Rect loop_button = layout_.sfx_loop[0];
            const POINT point{static_cast<LONG>(loop_button.center().x),
                              static_cast<LONG>(loop_button.center().y)};
            on_left_down(point);
            on_left_up(point);
            frame();
            check(product_.sfx_slots[0].loop != before, "点音效台的「循」按钮切换循环开关");
            on_left_down(point);
            on_left_up(point);
            frame();
            check(product_.sfx_slots[0].loop == before, "再点一次切回来");
        }
    }

    // ---- 6b. 右列（音效库）：悬停要点得到、卡片要拖得动 ----
    //
    // 这一段是针对一个真实故障补的：右列以前画的是**音乐**曲目（音效库压根
    // 没被载入），而且音效卡的命中区根本没登记 —— 于是右列"悬停无反应、
    // 拖不动"。断言必须落在"这一列的目标对象存在且能拖"上，而不是
    // "函数被调用了"，否则同样的问题还会漏过去。
    {
        frame();
        if (sfx_.visible.empty()) {
            skip("右列悬停命中音效卡（这台的曲库里没有音效）");
            skip("右列音效卡拖动到音效台（同上）");
        } else {
            // a) 悬停：光标落在右列第一张卡上，命中 id 必须落在音效卡段
            const ui::Rect sfx_card = layout_.card_rect(layout_.sfx_grid, 0, sfx_.scroll.offset());
            const POINT sfx_point{static_cast<LONG>(sfx_card.center().x),
                                  static_cast<LONG>(sfx_card.center().y)};
            on_mouse_move(sfx_point);
            frame();
            check_detail(state_.hovered_id == kSfxCardBase,
                         "悬停右列第一张卡 → 命中音效卡段（不再是\"没反应\"）",
                         std::to_string(state_.hovered_id));
            check(state_.sfx_library.hovered_track == 0,
                  "界面把悬停落在了音效列的 0 号条目上");

            // b) 两列的卡段不重叠：右列的 id 不能被左列的解释吃掉
            {
                bool is_sfx = false;
                int index = -1;
                const bool sfx_card_id = card_of_id(kSfxCardBase, is_sfx, index);
                check(sfx_card_id && is_sfx && index == 0, "音效卡 id 反解成音效列的 0 号");
                const bool bgm_card_id = card_of_id(kCardBase, is_sfx, index);
                check(bgm_card_id && !is_sfx && index == 0, "音乐卡 id 反解成音乐列的 0 号");
                check(section_of_id(kSfxCardBase) && !section_of_id(kCardBase),
                      "两列的归属判断互相排斥");
            }

            // c) 拖动：右列的卡拖到音效台上必须真的开始发声。
            //    音效台没有"点一下播放"（设计稿：音效卡靠拖或快捷键触发），
            //    所以拖动是右列唯一的直接播放方式。
            if (!audio_ready_) {
                skip("右列音效卡拖动到音效台（本机没有可用音频设备）");
            } else {
                const ui::Rect target_card =
                    layout_.card_rect(layout_.sfx_grid, 0, sfx_.scroll.offset());
                const POINT from{static_cast<LONG>(target_card.center().x),
                                 static_cast<LONG>(target_card.center().y)};
                const POINT to{static_cast<LONG>(layout_.sfx_discs[0].center().x),
                               static_cast<LONG>(layout_.sfx_discs[0].center().y)};

                // 先停干净，否则"在响"可能来自上一条断言留下的状态
                Failure ignored;
                static_cast<void>(engine_.stop(AudioBus::sfx, 0, 0.0, ignored));
                frame();
                on_left_down(from);
                on_mouse_move(POINT{from.x + 40, from.y});
                frame();
                check(dragging_ && drag_from_sfx_, "从右列开始拖动，并且记着它来自音效列");
                on_mouse_move(to);
                frame();
                const std::size_t dragged = drag_index_;
                on_left_up(to);
                frame();
                check_detail(wait_playing_index(AudioBus::sfx, 0, dragged),
                             "右列的音效卡拖到音效台 1 上之后开始发声",
                             std::to_string(dragged));
                const VoiceSlotState slot = engine_.slot_state(AudioBus::sfx, 0);
                check_detail(sfx_index_[0] == dragged,
                             "音效台上记的正是拖上去的那一条（不是隔壁列的同号条目）",
                             "台上 " + std::to_string(sfx_index_[0]) + "，拖的是 " +
                                 std::to_string(dragged));

                // 收尾：停掉，免得后面的断言听到残留的声音
                static_cast<void>(engine_.stop(AudioBus::sfx, 0, 0.0, ignored));
                frame();
            }
        }
    }

    // ---- 6b2. 标题栏按钮必须**点得到** ----
    //
    // 这条也是真故障的回归守卫：整条标题栏原本一律返回 HTCAPTION（好让拖动
    // 交给系统），于是那三个按钮画得出来、鼠标却永远碰不到 —— 实测扫过一遍，
    // 那片区域从来没收到过鼠标消息。窗口层现在问 is_titlebar_button_point，
    // 所以这里断言的是"按钮位置要认出是按钮、别处别乱认"。
    {
        const ui::Rect bar = layout_.titlebar;
        check(bar.height > 0.0f, "标题栏有高度（本机 DPI 下量出来的）");
        int recognized = 0;
        for (int index = 0; index < 3; ++index) {
            const ui::Rect button{bar.right() - 40.0f - static_cast<float>(index) * 34.0f,
                                  bar.y + 6.0f, 28.0f, bar.height - 12.0f};
            POINT center{static_cast<LONG>(button.center().x),
                         static_cast<LONG>(button.center().y)};
            if (is_titlebar_button_point(center)) {
                ++recognized;
            }
        }
        check(recognized == 3, "三个标题栏按钮的位置都被认成按钮（否则点不到）");

        // 按钮之间的缝不能算按钮，否则拖动窗口会莫名其妙点掉一个按钮
        {
            const ui::Rect button0{bar.right() - 40.0f, bar.y + 6.0f, 28.0f, bar.height - 12.0f};
            POINT gap{static_cast<LONG>(button0.x - 3.0f),
                      static_cast<LONG>(button0.center().y)};
            check(!is_titlebar_button_point(gap), "按钮之间的缝隙不算按钮（还能拖动窗口）");
        }
        // 标题栏之外的客户区当然更不算
        {
            POINT below{static_cast<LONG>(bar.center().x),
                        static_cast<LONG>(bar.bottom() + 40.0f)};
            check(!is_titlebar_button_point(below), "标题栏下方不算按钮");
        }
    }

    // ---- 6c. 悬停是渐变的，不是一步跳过去的 ----
    //
    // 这条断言盯的是手感：原型里悬停是 120ms 缓动，而之前这里是一步到位 ——
    // "鼠标效果差得远"主要就差在这。所以断言"刚移上去的那一帧还没走到 1"。
    {
        frame();
        if (bgm_.visible.empty()) {
            skip("悬停过渡是渐变的（曲库是空的）");
        } else {
            // 先把光标移开，让所有过渡量归零
            on_mouse_move(POINT{2, 2});
            for (int i = 0; i < 20; ++i) {
                ::Sleep(16);
                frame();
            }
            const ui::Rect card = layout_.card_rect(layout_.bgm_grid, 0, bgm_.scroll.offset());
            on_mouse_move(POINT{static_cast<LONG>(card.center().x),
                                static_cast<LONG>(card.center().y)});
            frame();
            const float first = bgm_hover_amounts_.empty() ? -1.0f : bgm_hover_amounts_[0];
            check_detail(first >= 0.0f && first < 1.0f,
                         "刚悬停上去时过渡量还没到 1（不是一步跳过去）",
                         std::to_string(first));

            for (int i = 0; i < 20; ++i) {
                ::Sleep(16);
                frame();
            }
            const float settled = bgm_hover_amounts_.empty() ? -1.0f : bgm_hover_amounts_[0];
            check_detail(settled > 0.99f, "过一会儿过渡量走到 1（悬停态真的到了）",
                         std::to_string(settled));

            // 移开之后要退回去
            on_mouse_move(POINT{2, 2});
            for (int i = 0; i < 20; ++i) {
                ::Sleep(16);
                frame();
            }
            const float away = bgm_hover_amounts_.empty() ? -1.0f : bgm_hover_amounts_[0];
            check_detail(away < 0.01f, "移开之后过渡量退回 0", std::to_string(away));
        }
    }

    // ---- 6d. 第二个实例把面板叫出来（走的是注册消息那条路）----
    {
        frame();
        const unsigned int message = show_panel_message();
        check(message != 0, "注册消息拿到了一个有效的 id（跨进程要靠它）");
        if (message != 0) {
            set_panel_visible(false);
            for (int i = 0; i < 40 && panel_window_visible(); ++i) {
                ::Sleep(20);
                frame();
            }
            check(!panel_window_visible(), "先收起来，好验证呼出这一步真的起作用了");

            LRESULT result = 0;
            static_cast<void>(handle_message(message, 0, 0, result));
            frame();
            check(panel_visible_ && panel_window_visible(),
                  "收到呼出消息之后面板显出来了");
            check(status_.find("呼出") != std::string::npos, "状态栏说了是呼出来的");
        }
    }

    // ---- 7. 滚轮滚动曲库 ----
    {
        frame();
        const ui::Rect grid = layout_.bgm_grid;
        const POINT point{static_cast<LONG>(grid.center().x), static_cast<LONG>(grid.center().y)};
        const float before = bgm_.scroll.offset();
        on_wheel(point, -WHEEL_DELTA * 3);
        frame();
        if (bgm_.scroll.scrollable()) {
            check(bgm_.scroll.offset() > before, "滚轮向下滚动了曲库");

            // **平滑**：滚轮给的是目标，位移由动画追过去。
            // 所以"一帧之后就到了"反而是错的 —— 那说明它在跳。
            // 注意方向：滚轮 delta 为负 = 向下滚 = 偏移变大（与真人一致）。
            const float target_before = bgm_scroll_target_;
            on_wheel(point, -WHEEL_DELTA * 6);
            const float target = bgm_scroll_target_;
            check_detail(target > target_before,
                         "一次滚轮把目标推远了（先记目标，不瞬移）",
                         "目标 " + std::to_string(target) + "，当前 " +
                             std::to_string(bgm_.scroll.offset()));
            frame();
            check_detail(std::fabs(bgm_.scroll.offset() - target) > 0.5f,
                         "一帧之后还没到目标（是真的在滑动，不是跳过去）",
                         std::to_string(bgm_.scroll.offset()));

            // 等它滑到位
            for (int i = 0; i < 90 && std::fabs(bgm_.scroll.offset() - target) > 0.5f; ++i) {
                ::Sleep(8);
                frame();
            }
            check_detail(std::fabs(bgm_.scroll.offset() - target) < 0.6f,
                         "滑动最终停在目标上", std::to_string(bgm_.scroll.offset()));

            on_wheel(point, WHEEL_DELTA * 1000);   // 一路滚回顶
            for (int i = 0; i < 200 && std::fabs(bgm_.scroll.offset()) > 1.0f; ++i) {
                ::Sleep(8);
                frame();
            }
            // 收口那一帧才把它按到目标上（阈值内直接对齐），所以要多跑一帧再断言
            frame();
            check_detail(bgm_.scroll.offset() == 0.0f, "滚到顶之后停在 0（不会变成负数）",
                         std::to_string(bgm_.scroll.offset()));
        } else {
            skip("滚轮滚动曲库（曲目还没多到需要滚动）");
        }
    }

    // ---- 8. 分类筛选 ----
    {
        frame();
        if (state_.bgm_library.chip_widths.size() > 1 && state_.bgm_library.chip_widths[1] > 0.0f) {
            const std::vector<ui::Rect> chips =
                layout_.chip_rects(layout_.bgm_chips, state_.bgm_library.chip_widths);
            if (chips.size() > 1) {
                const std::size_t before = bgm_.visible.size();
                const POINT second{static_cast<LONG>(chips[1].center().x),
                                   static_cast<LONG>(chips[1].center().y)};
                on_left_down(second);
                on_left_up(second);
                frame();
                check(bgm_.active_chip == 0, "点第一个分类芯片之后进入该分类");
                check(bgm_.visible.size() <= before, "筛选之后可见曲目不多于之前");
                check_detail(state_.bgm_library.active_chip == 1, "界面状态里的选中芯片同步了",
                             std::to_string(state_.bgm_library.active_chip));

                const POINT first{static_cast<LONG>(chips[0].center().x),
                                  static_cast<LONG>(chips[0].center().y)};
                on_left_down(first);
                on_left_up(first);
                frame();
                check(bgm_.active_chip == -1, "点「全部」回到不筛选");
            } else {
                skip("分类筛选（芯片还没量出宽度）");
            }
        } else {
            skip("分类筛选（没有分类）");
        }
    }

    // ---- 9. 音量：点音量条 → 设置改变 → 落到引擎 ----
    {
        frame();
        // 点的是**可拖的那一段**（右侧百分比文字不算）——
        // 与绘制、拖动用的是同一个矩形。
        const ui::Rect slider = ui::PanelLayout::volume_track(layout_.bgm_volume_slider);
        const POINT point{static_cast<LONG>(slider.x + slider.width * 0.5f),
                          static_cast<LONG>(slider.center().y)};
        on_left_down(point);
        on_left_up(point);
        frame();
        check_detail(product_.bgm_volume_percent >= 45 && product_.bgm_volume_percent <= 55,
                     "点音量条中间 → 音量落在 50% 附近",
                     std::to_string(product_.bgm_volume_percent));
        if (audio_ready_) {
            check(std::fabs(engine_.bus_volume(AudioBus::bgm) -
                            static_cast<float>(product_.bgm_volume_percent) / 100.0f) < 0.01f,
                  "音量改动落到了引擎上");
        }
    }

    // ---- 10. 拖入文件导入（在临时曲库里做，**绝不碰用户真实的曲库**）----
    {
        // 用一个临时曲库：改 library_root_ 之前先存下来，做完就还回去。
        // 自检往用户的曲库里写文件是不可接受的 —— 那是他的资料。
        const std::filesystem::path saved_root = library_root_;
        const std::vector<std::string> saved_categories = bgm_.categories;
        const std::size_t saved_library_size = entries_.size();

        const std::filesystem::path sandbox =
            product_data_root() / L"selftest-import";
        remove_tree(sandbox);
        std::error_code ec;
        // 分类名故意用中文：分类名到路径这一段要经 path_from_utf8，
        // 直接拼窄字符串会走活动代码页（本项目最怕的一类错），这里正好钉住它。
        const std::string category = "自检分类";
        std::filesystem::path category_dir;
        Failure convert_failure;
        const bool converted = path_from_utf8(category, category_dir, convert_failure);
        check(converted, "中文分类名能转成路径");
        if (converted) {
            std::filesystem::create_directories(sandbox / L"BGM" / category_dir, ec);
        }

        const std::filesystem::path source = sandbox / L"待导入的曲子.wav";
        check(write_selftest_wav(source, 200), "准备一个待导入的音频文件");

        library_root_ = sandbox;
        std::string problem;
        const bool imported = import_local_file(source, category, false, problem);
        check_detail(imported, "导入本地文件成功", problem);

        if (imported) {
            // 文件真的落到了 BGM/<中文分类>/ 下面
            const std::filesystem::path expected =
                sandbox / L"BGM" / category_dir / L"待导入的曲子.wav";
            check(std::filesystem::is_regular_file(expected, ec),
                  "文件落在 BGM/中文分类/ 下（分类名没有被编码打乱）");
            check(entries_.size() == saved_library_size + 1, "内存里的曲库多了一条");
            check(!entries_.empty() && entries_.back().category == category,
                  "新曲目记在了刚才那个分类下");
            check(!entries_.empty() && entries_.back().track.name == "待导入的曲子",
                  "曲名用的是文件自己的主干（没有按 URL 或别的东西改）");
            check(std::find(bgm_.categories.begin(), bgm_.categories.end(), category) !=
                      bgm_.categories.end(),
                  "新分类进了分类列表");

            // 源文件必须还在 —— 导入是复制，不是搬走用户的原件
            check(std::filesystem::is_regular_file(source, ec),
                  "导入之后用户的原文件还在（是复制，不是搬走）");

            // 再导入一次同一个文件：文件已经在曲库里了，不该产生第二份副本
            check(path_is_inside(library_root_, expected),
                  "导入后的文件被判定为\"在曲库内\"（所以再次拖入会被跳过）");

            // 拖进来的如果不是音频，要明确失败并说明原因
            const std::filesystem::path junk = sandbox / L"不是音频.mp3";
            {
                const std::string text = "这不是音频";
                const HANDLE file = ::CreateFileW(junk.c_str(), GENERIC_WRITE, 0, nullptr,
                                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    static_cast<void>(::WriteFile(file, text.data(),
                                                 static_cast<DWORD>(text.size()), &written,
                                                 nullptr));
                    ::CloseHandle(file);
                }
            }
            std::string junk_problem;
            const bool junk_imported = import_local_file(junk, category, false, junk_problem);
            check(!junk_imported && !junk_problem.empty(),
                  "拖进来的不是音频 → 明确失败并给出原因");
        }

        // 同一个入口、as_sfx=true：必须落到 SFX/ 下，并被标成音效。
        // 这两件事以前是写死的（永远 BGM、永远 is_sfx=false），
        // 于是"拖到音效列里导入"这个手势根本无处可去。
        {
            // 沙盒里**故意不建** SFX/<分类> 目录：散放在库根目录下的文件会被
            // 扫描器归成「默认」分类，而那个目录从没真被建过 —— 拖进「默认」
            // 以前会报"目标分类目录不存在"。导入要自己把它建出来。
            const std::filesystem::path expected_dir = sandbox / L"SFX" / category_dir;
            check(!std::filesystem::exists(expected_dir, ec),
                  "沙盒里刻意没有 SFX 分类目录（要考的是导入自己会建）");
            const std::filesystem::path sfx_source = sandbox / L"待导入的音效.wav";
            const bool prepared = write_selftest_wav(sfx_source, 200);
            const bool sfx_imported =
                prepared && import_local_file(sfx_source, category, true, problem);
            check_detail(sfx_imported, "把文件作为音效导入成功", problem);
            if (sfx_imported) {
                const std::filesystem::path expected =
                    sandbox / L"SFX" / category_dir / L"待导入的音效.wav";
                check_detail(std::filesystem::is_regular_file(expected, ec),
                             "音效落在 SFX/中文分类/ 下（没有跑进 BGM/）", problem);
                check(!entries_.empty() && entries_.back().is_sfx,
                      "新条目被标成音效（右列才会列出它）");
                check(trigger_pad(entries_.size() - 1), "新导入的音效已预载，DJ 按钮立即可播");
                clear_playback(true);
                check(engine_.active_count(AudioBus::sfx) == 0, "导入扩容后清空所有音效成功");
                check(std::find(sfx_.visible.begin(), sfx_.visible.end(),
                                entries_.size() - 1) != sfx_.visible.end(),
                      "而且它真的出现在音效列的可见列表里");
            }
        }

        // 收尾：还原曲库根与内存模型，删掉沙盒
        library_root_ = saved_root;
        bgm_.categories = saved_categories;
        entries_.resize(saved_library_size);
        rebuild_visible(bgm_, false);
        rebuild_visible(sfx_, true);
        remove_tree(sandbox);
    }

    // ---- 11. 通知区域与悬浮球（这两样是"常驻"形态的全部）----
    {
        frame();
        // 托盘图标是程序自己画出来的；建不起来时程序会照常跑但"关闭"会变成退出。
        // 这里只断言"建起来了"，因为它的可视效果没法自动验。
        check(tray_ready_, "通知区域图标建起来了");
        check(tray_icon_ != nullptr, "图标句柄有效（不是空句柄）");

        // 悬浮球 ↔ 主面板两态切换：这是产品的核心交互。
        // 直接给球窗口发鼠标消息，走的是与真人点击完全相同的窗口过程。
        if (orb_window_handle() == nullptr) {
            skip("悬浮球左键展开面板（本机没建出悬浮球窗口）");
        } else {
            set_panel_visible(false);
            frame();
            check(!panel_visible_, "先收起面板");

            const HWND orb = static_cast<HWND>(orb_window_handle());
            // 按下与松开在同一个点 → 算点击（不是拖动），于是应当展开面板
            ::SendMessageW(orb, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(90, 90));
            ::SendMessageW(orb, WM_LBUTTONUP, 0, MAKELPARAM(90, 90));
            frame();
            check(panel_visible_, "点击悬浮球展开主面板");

            // 再点一次：**收起**。球是这套东西的开关（用户明确要求），
            // 原来只做"展开"，面板开着时点球没反应。
            ::SendMessageW(orb, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(90, 90));
            ::SendMessageW(orb, WM_LBUTTONUP, 0, MAKELPARAM(90, 90));
            frame();
            check(!panel_visible_, "球上部在面板打开时点击收起");
            // 收起的动效走完再把面板显回来，免得影响后面的断言
            for (int i = 0; i < 40 && panel_window_visible(); ++i) {
                ::Sleep(20);
                frame();
            }
            ::SendMessageW(orb, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(90, 90));
            ::SendMessageW(orb, WM_LBUTTONUP, 0, MAKELPARAM(90, 90));
            frame();
            check(panel_visible_, "再点一次 → 又展开");
        }
    }

    // ---- 12. 快捷键改键 ----
    {
        frame();
        if (!audio_ready_) {
            skip("快捷键改键（本机没有可用音频设备）");
        } else {
            // 进捕获态：必须先把已注册的全局快捷键全撤掉，
            // 否则用户按下的那一组可能正好已注册，会一边被捕获一边打响音效。
            const std::size_t registered_before = hotkeys_.registered_count();
            begin_hotkey_capture(0);
            frame();
            check(capturing_slot_ == 0, "进入捕获态（正在等音效台 1 的新快捷键）");
            check(hotkeys_.registered_count() == 0,
                  "捕获期间把自己的全局快捷键全撤掉了（否则会边捕获边触发）");
            check(status_.find("请按下") != std::string::npos, "状态栏提示了要按新快捷键");
            static_cast<void>(registered_before);

            // 只按修饰键不算：用户还在按组合键的过程中
            on_key_down(VK_CONTROL);
            frame();
            check(capturing_slot_ == 0, "只按修饰键不结束捕获（还在等普通键）");

            // Esc 取消：注册要恢复回去
            on_key_down(VK_ESCAPE);
            frame();
            check(capturing_slot_ == -1, "Esc 取消捕获");
            check(hotkeys_.registered_count() > 0, "取消之后原来的快捷键被重新注册回来了");

            // 真的改一个键。修饰键状态是从 GetKeyState 取的，自检里按不出
            // 真实的 Ctrl，所以这里把"按下"这一步拆开验：
            //   a) 没有修饰键的组合必须被拒（裸字母会抢掉别的程序里的按键）
            //   b) 带修饰键的组合能提交，并且落到设置里、重新注册上
            const UINT saved_modifiers = static_cast<UINT>(product_.hotkeys.decks[0].modifiers);
            const UINT saved_key = static_cast<UINT>(product_.hotkeys.decks[0].virtual_key);

            // **必须重新进捕获态**：上面刚用 Esc 退出了。
            // 少了这一步，commit 会因为"不在捕获态"直接返回 false ——
            // 下一条断言于是"因为错误的原因通过"，而后面两条会莫名其妙地红。
            // （一个因为错误原因通过的断言比一条红的更危险。）
            begin_hotkey_capture(0);
            frame();
            check(capturing_slot_ == 0, "重新进入捕获态");

            // a) 不带修饰键：commit 必须拒绝并说明原因，且不改动设置
            check(!commit_hotkey_capture('Q'),
                  "没有修饰键的组合被拒（会抢掉别处的按键）");
            check(product_.hotkeys.decks[0].virtual_key == saved_key,
                  "被拒之后设置没有被改动");
            check(capturing_slot_ == 0, "被拒之后仍在捕获态（让用户重按）");
            check(status_.find("没有修饰键") != std::string::npos,
                  "状态栏说清了为什么被拒");

            // b) 带修饰键：直接写一组，然后走 re_register_hotkeys 的注册路径
            product_.hotkeys.decks[0].modifiers = MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT;
            product_.hotkeys.decks[0].virtual_key = VK_F9;
            capturing_slot_ = -1;
            re_register_hotkeys();
            frame();
            check_detail(hotkey_labels_[0].find("Ctrl") != std::string::npos &&
                             hotkey_labels_[0].find("F9") != std::string::npos,
                         "界面角标跟着新组合变了", hotkey_labels_[0]);
            check_detail(hotkey_menu_label(0).find("Ctrl+Shift+F9") != std::string::npos,
                         "菜单标签也跟着变了", hotkey_menu_label(0));
            check(hotkeys_.is_registered(0), "新组合注册上了");

            // 恢复原样
            product_.hotkeys.decks[0].modifiers = saved_modifiers;
            product_.hotkeys.decks[0].virtual_key = saved_key;
            re_register_hotkeys();
            frame();
            check(hotkeys_.registered_count() > 0, "回到原组合之后快捷键都在");

            // 关掉全局快捷键：一个都不该留着注册
            product_.hotkeys.enabled = false;
            re_register_hotkeys();
            frame();
            check(hotkeys_.registered_count() == 0, "关掉之后一个全局快捷键都没注册");
            product_.hotkeys.enabled = true;
            re_register_hotkeys();
            frame();
            check(hotkeys_.registered_count() > 0, "重新打开之后又注册上了");
        }
    }

    // ---- 13. 面板显隐与设置往返 ----
    {
        // 收起现在是**带动效**的（原型 420ms）：调用之后窗口还会留着，
        // 等动效走完才真的藏起来。所以这里分两步断言 ——
        // 只断言"立刻不可见"会把一个有意的动效当成 bug。
        set_panel_visible(false);
        frame();
        check(!panel_visible_, "收起面板：状态立刻变成不可见");
        check(panel_window_visible(), "收起动效期间窗口还在（不是瞬间消失）");

        // 把动效走完（420ms + 余量）
        for (int i = 0; i < 40 && panel_window_visible(); ++i) {
            ::Sleep(20);
            frame();
        }
        check(!panel_window_visible(), "动效走完之后窗口真的藏起来了");

        set_panel_visible(true);
        frame();
        check(panel_visible_ && panel_window_visible(), "再显示回来");
        for (int i = 0; i < 40; ++i) {
            ::Sleep(20);
            frame();
        }
        check_detail(state_.reveal > 0.99f, "展开动效走完之后进度到 1",
                     std::to_string(state_.reveal));

        check(!settings_path_.empty(), "设置文件路径已确定");
        save_settings();
        ProductSettingsStore store;
        ProductSettingsLoadResult loaded;
        Failure failure;
        const bool loaded_ok = store.load_or_initialize(settings_path_, loaded, failure);
        check(loaded_ok, "保存后能读回设置");
        if (loaded_ok) {
            check(loaded.settings.bgm_volume_percent == product_.bgm_volume_percent,
                  "音量往返无损");
            check(loaded.settings.sfx_slots[0].loop == product_.sfx_slots[0].loop,
                  "音效台开关往返无损");
            check(loaded.settings.library_folder == product_.library_folder,
                  "曲库目录记进了设置");
        }
    }

    // ---- 14. 这一轮补的交互：拖拽幽灵 / 音量拖动 / 唱片按钮 / 历史 / 异步包络 ----
    {
        frame();

        // a) 拖拽幽灵要有内容。drag_label 曾经从来没被赋值过，
        //    于是"拖动时没有任何东西跟着鼠标" —— 只有断言到这个字段才防得住。
        if (!bgm_.visible.empty() && audio_ready_) {
            const ui::Rect card = layout_.card_rect(layout_.bgm_grid, 0, bgm_.scroll.offset());
            const POINT from{static_cast<LONG>(card.center().x), static_cast<LONG>(card.center().y)};
            on_left_down(from);
            on_mouse_move(POINT{from.x + 60, from.y + 30});
            frame();
            check_detail(state_.dragging && !state_.drag_label.empty(),
                         "拖动时有一块跟手的标签（内容是曲名）", state_.drag_label);
            check(state_.drag_cursor.x > card.x, "幽灵跟着光标走，不是停在按下点");
            on_left_up(POINT{from.x + 60, from.y + 30});
            frame();
        } else {
            skip("拖动时跟手的标签（没有可拖的曲目或没有音频设备）");
        }

        // b) 全局音量条：按下就跳到位，然后跟着鼠标走，松手落盘
        {
            const ui::Rect slider = ui::PanelLayout::volume_track(layout_.bgm_volume_slider);
            check(slider.width > 130.0f, "全局音量条加长到接近两倍（方便微调）");
            const POINT quarter{static_cast<LONG>(slider.x + slider.width * 0.25f),
                                static_cast<LONG>(slider.center().y)};
            on_left_down(quarter);
            frame();
            const int after_press = product_.bgm_volume_percent;
            check_detail(after_press >= 20 && after_press <= 30,
                         "点音量条 1/4 处 → 音量跳到 25% 附近", std::to_string(after_press));
            const POINT three_quarter{static_cast<LONG>(slider.x + slider.width * 0.75f),
                                      static_cast<LONG>(slider.center().y)};
            on_mouse_move(three_quarter);
            frame();
            check_detail(product_.bgm_volume_percent >= 70 &&
                             product_.bgm_volume_percent <= 80,
                         "**按住不放拖动**也会跟着走（不只是点一下）",
                         std::to_string(product_.bgm_volume_percent));
            on_left_up(three_quarter);
            frame();

            // SFX 全局音量条：原来连命中区都没有
            const ui::Rect sfx_slider = ui::PanelLayout::volume_track(layout_.sfx_volume_slider);
            const POINT half{static_cast<LONG>(sfx_slider.x + sfx_slider.width * 0.5f),
                             static_cast<LONG>(sfx_slider.center().y)};
            on_left_down(half);
            frame();
            check_detail(product_.sfx_volume_percent >= 45 &&
                             product_.sfx_volume_percent <= 55,
                         "SFX 全局音量条也能拖动（原来完全没反应）",
                         std::to_string(product_.sfx_volume_percent));
            on_left_up(half);
            frame();

            // 右侧百分比数字那一块不该能拖（用户明确指出的）
            const ui::Rect full = layout_.bgm_volume_slider;
            const ui::Rect track = ui::PanelLayout::volume_track(full);
            const int before_label = product_.bgm_volume_percent;
            const POINT label_point{static_cast<LONG>(track.right() + 12.0f),
                                    static_cast<LONG>(full.center().y)};
            on_left_down(label_point);
            on_left_up(label_point);
            frame();
            check_detail(product_.bgm_volume_percent == before_label,
                         "百分比数字那一块拖不动（那里只是数字）",
                         std::to_string(product_.bgm_volume_percent));
        }

        // c) BGM 台的三个开关：命中区 + 真的改设置
        {
            const std::vector<std::string> labels = {
                state_.bgm.category.empty() ? std::string("未选分类") : state_.bgm.category,
                state_.bgm.format.empty() ? std::string("—") : state_.bgm.format,
                "循环", "淡入淡出", "混音"};
            const std::vector<ui::Rect> items = layout_.option_rects(layout_.bgm_deck_line, labels);
            check(items.size() >= 3, "BGM 台那一行排出了开关（循环 / 淡入淡出 / 混音）");
            if (items.size() >= 3) {
                const auto click_item = [&](std::size_t from_end) {
                    const ui::Rect box = items[items.size() - 1 - from_end];
                    const POINT point{static_cast<LONG>(box.center().x),
                                      static_cast<LONG>(box.center().y)};
                    on_left_down(point);
                    on_left_up(point);
                    frame();
                };
                const bool loop_before = product_.bgm_loop;
                click_item(2);
                check(product_.bgm_loop != loop_before, "点「循环」真的改了设置（原来点了没反应）");
                click_item(2);
                check(product_.bgm_loop == loop_before, "再点一次切回来");

                const bool fade_before = product_.bgm_fade;
                click_item(1);
                check(product_.bgm_fade != fade_before, "点「淡入淡出」真的改了设置");
                click_item(1);

                const bool restart_before = product_.bgm_mix;
                click_item(0);
                check(product_.bgm_mix != restart_before, "点「混音」真的改了设置");
                click_item(0);
            }
        }

        // d) 音效台：点唱片是**暂停/继续**，不是把台清掉
        if (!audio_ready_) {
            skip("音效台点唱片 = 暂停（没有音频设备）");
        } else {
            const std::vector<HotkeyBinding> bindings = current_hotkey_bindings();
            const std::size_t deck = bindings.empty()
                                         ? 0
                                         : static_cast<std::size_t>(bindings.front().id);
            if (!sfx_.visible.empty()) {
                static_cast<void>(trigger_sfx(deck, sfx_.visible.front()));
                frame();
                check(wait_playing(AudioBus::sfx, deck), "音效台起播了（异步解码之后）");
                const POINT disc{static_cast<LONG>(layout_.sfx_discs[deck].center().x),
                                 static_cast<LONG>(layout_.sfx_discs[deck].center().y)};
                on_left_down(disc);
                on_left_up(disc);
                frame();
                check(wait_paused(AudioBus::sfx, deck), "点音效唱片之后它是暂停状态");
                const VoiceSlotState paused = engine_.slot_state(AudioBus::sfx, deck);
                check(paused.in_use, "点音效唱片之后**台上还有**这个音效（不是被清掉）");
                on_left_down(disc);
                on_left_up(disc);
                frame();
                check(!engine_.slot_state(AudioBus::sfx, deck).paused, "再点一下 → 继续");

                // 循环开关要真的作用到引擎上（"关掉了还在放"就是这个）
                const bool loop_before = product_.sfx_slots[deck].loop;
                const ui::Rect loop_box = layout_.sfx_loop[deck];
                const POINT loop_point{static_cast<LONG>(loop_box.center().x),
                                       static_cast<LONG>(loop_box.center().y)};
                on_left_down(loop_point);
                on_left_up(loop_point);
                frame();
                check_detail(!engine_.slot_state(AudioBus::sfx, deck).looping,
                             "关掉循环之后引擎里的循环也关了（正在放的那条也生效）",
                             std::to_string(engine_.slot_state(AudioBus::sfx, deck).looping));
                on_left_down(loop_point);
                on_left_up(loop_point);
                frame();
                check(product_.sfx_slots[deck].loop == loop_before, "再点一次切回来");
                Failure ignored;
                static_cast<void>(engine_.stop(AudioBus::sfx, deck, 0.0, ignored));
                frame();
            } else {
                skip("音效台点唱片 = 暂停（右列没有音效）");
            }
        }

        // e) 上一首 / 下一首：走播放历史
        if (!audio_ready_ || bgm_.visible.size() < 2) {
            skip("上一首 / 下一首走播放历史（曲目不够或没有音频设备）");
        } else {
            static_cast<void>(play_bgm(bgm_.visible[0]));
            frame();
            check(wait_playing_index(AudioBus::bgm, bgm_slot_, bgm_.visible[0]),
                  "第一首起来了（等的是**这一首**，不是'这一台在响'）");
            static_cast<void>(play_bgm(bgm_.visible[1]));
            frame();
            check(wait_playing_index(AudioBus::bgm, bgm_slot_, bgm_.visible[1]), "第二首也起来了");
            check(state_.can_prev, "播放两首之后「上一首」可点了");
            check(!state_.history_prev.empty(), "「上一首」上写着邻曲名（不是空白按钮）");
            check(!state_.can_next, "还没有下一首时它不可点");

            const POINT prev_point{static_cast<LONG>(layout_.bgm_prev.center().x),
                                   static_cast<LONG>(layout_.bgm_prev.center().y)};
            on_left_down(prev_point);
            on_left_up(prev_point);
            frame();
            check_detail(wait_playing_index(AudioBus::bgm, bgm_slot_, bgm_.visible[0]), "点「上一首」回到刚才放的那一首",
                         std::to_string(bgm_index_));
            check(state_.can_next, "回到上一首之后「下一首」可点了");
        }

        // f) 点音波图跳转：落点要对，而且**不能把循环区间弄坏**。
        //    "跳转之后反复只播一小段"就是这个：单缓冲 + 无限循环的循环区间是
        //    现在提交整段 PCM，起播点可改变，循环区域始终是整首。
        if (!audio_ready_ || bgm_.visible.empty()) {
            skip("点音波图跳转（没有曲目或没有音频设备）");
        } else {
            static_cast<void>(play_bgm(bgm_.visible[0]));
            frame();
            check(wait_playing_index(AudioBus::bgm, bgm_slot_, bgm_.visible[0]),
                  "点音波图那一段：曲目起来了");
            ::Sleep(150);
            frame();
            const VoiceSlotState playing = engine_.slot_state(AudioBus::bgm, bgm_slot_);
            if (!playing.in_use || playing.duration_seconds <= 1.0) {
                skip("点音波图跳转（这一首太短或没放起来）");
            } else {
                const ui::Rect wave = layout_.bgm_waveform;
                const POINT middle{static_cast<LONG>(wave.x + wave.width * 0.35f),
                                   static_cast<LONG>(wave.center().y)};
                on_left_down(middle);
                on_left_up(middle);
                frame();
                const VoiceSlotState seeked = engine_.slot_state(AudioBus::bgm, bgm_slot_);
                const double expected = seeked.duration_seconds * 0.35;
                check_detail(std::fabs(seeked.position_seconds - expected) <
                                 seeked.duration_seconds * 0.06,
                             "点音波图 35% 处 → 播放位置就在 35% 附近（不是别处）",
                             "期望 " + std::to_string(expected) + "，实际 " +
                                 std::to_string(seeked.position_seconds));
                check_detail(seeked.position_seconds > 1.0,
                             "确实跳到了中间，而不是跳回开头",
                             std::to_string(seeked.position_seconds));
                // 位置要**继续往前走**（不是卡在跳转点反复重来）
                ::Sleep(400);
                frame();
                const VoiceSlotState later = engine_.slot_state(AudioBus::bgm, bgm_slot_);
                check_detail(later.position_seconds > seeked.position_seconds,
                             "跳转之后位置继续前进（没有卡在一小段里反复）",
                             std::to_string(seeked.position_seconds) + " → " +
                                 std::to_string(later.position_seconds));
                Failure ignored;
                static_cast<void>(engine_.stop(AudioBus::bgm, bgm_slot_, 0.0, ignored));
                frame();
            }
        }

        // g) 包络是**后台**算的：界面线程从不解码。
        //    这里只能验"算好了会送达"，但那条路径正是悬停延迟的根因。
        if (!entries_.empty() && audio_ready_) {
            const std::size_t probe = bgm_.visible.empty() ? 0 : bgm_.visible[0];
            entries_[probe].envelope_attempted = false;
            entries_[probe].envelope.clear();
            // 先忘掉它：后台那边记着"这个 key 排过队"，不清掉的话重新请求
            // 不会再排一次（forget 就是给这种"要重来一遍"准备的）。
            envelopes_.forget(probe);
            ensure_envelope(probe);
            frame();
            bool delivered = false;
            for (int i = 0; i < 200 && !delivered; ++i) {
                ::Sleep(10);
                frame();
                delivered = entries_[probe].envelope_attempted;
            }
            check(delivered, "后台把包络算好并送了回来（悬停不再阻塞界面）");
            check(!entries_[probe].envelope.empty(), "算出来的包络不是空的");

            // 连续请求多个音频时，**任何一个都不该永远拿不到**。
            // 音频任务只保留最新一个（用户一次只能拖一首），但被顶掉的那些
            // 必须能重新请求 —— 否则它们会永远卡在"正在载入"，
            // 这正是用户说的"大量音乐没办法播放"。
            {
                EnvelopeWorker playback_probe;
                const std::size_t other = bgm_.visible.size() > 1 ? bgm_.visible[1] : probe;
                playback_probe.request_audio(probe, entries_[probe].track.path);
                playback_probe.request_audio(other, entries_[other].track.path);
                playback_probe.request_audio(probe, entries_[probe].track.path);
                DecodedAudio audio;
                bool decoded = false;
                bool got = false;
                for (int i = 0; i < 300 && !got; ++i) {
                    ::Sleep(10);
                    Failure decode_failure;
                    got = playback_probe.take_audio(probe, audio, decoded, decode_failure);
                }
                check_detail(got && decoded,
                             "被顶掉的音频请求还能重新拿到（不会永远卡在正在载入）",
                             got ? std::to_string(decoded) : "超时");
            }


        } else {
            skip("后台算包络（没有曲目或没有音频设备）");
        }
    }

        // Lifecycle regression cases: use real handlers and real asynchronous decoding.
    if (audio_ready_ && !bgm_.visible.empty()) {
        const auto point_of = [](const ui::Rect& rect) {
            return POINT{static_cast<LONG>(rect.center().x), static_cast<LONG>(rect.center().y)};
        };
        const std::size_t first = bgm_.visible.front();
        const std::size_t second = bgm_.visible.size() > 1 ? bgm_.visible[1] : first;
        clear_playback(false);
        clear_playback(true);
        product_.bgm_fade = false;
        product_.bgm_loop = true;
        check(play_bgm(first), "加载请求已接受");
        // Build state without pumping the decoder result so loading can be cancelled.
        sync_state();
        build_regions();
        check(state_.can_clear_bgm, "加载期间清空按钮可用");
        on_left_down(point_of(layout_.bgm_clear));
        on_left_up(point_of(layout_.bgm_clear));
        for (int i = 0; i < 30; ++i) { ::Sleep(10); frame(); }
        check(pending_plays_.empty() && !engine_.slot_state(AudioBus::bgm, bgm_slot_).in_use,
              "加载后立刻清空：后台结果不会让音乐重新响起");

        bool queued_all = true;
        for (std::size_t deck = 0; deck < sfx_slot_count; ++deck) {
            product_.sfx_slots[deck].loop = true;
            product_.sfx_slots[deck].fade = false;
            queued_all = trigger_sfx(deck, deck % 2 == 0 ? first : second) && queued_all;
        }
        check(queued_all, "四路音效请求均被接受（含同一首分配多个台位）");
        for (std::size_t deck = 0; deck < sfx_slot_count; ++deck) {
            check(wait_playing_index(AudioBus::sfx, deck, deck % 2 == 0 ? first : second),
                  "并发请求送达音效台 " + std::to_string(deck + 1));
        }
        LRESULT result = 0;
        static_cast<void>(handle_message(WM_HOTKEY, 1, 0, result));
        check(wait_playing_index(AudioBus::sfx, 1, second), "快捷键重复触发该台已分配曲目");
        Failure failure;
        check(engine_.pause(AudioBus::sfx, 0, failure), "暂停音效台成功");
        frame();
        on_left_down(point_of(layout_.sfx_clear));
        on_left_up(point_of(layout_.sfx_clear));
        frame();
        check(engine_.active_count(AudioBus::sfx) == 0 && pending_plays_.empty(),
              "清空音效列同时终止暂停声部与所有待播请求");
        check(!state_.sfx[0].has_track && state_.sfx[0].envelope.empty() && state_.sfx[0].path.empty(),
              "清空音效后封面、波形和曲目信息均无残留");

        check(play_bgm(first) && wait_playing_index(AudioBus::bgm, bgm_slot_, first), "准备重复投放暂停曲目");
        failure.clear();
        check(engine_.pause(AudioBus::bgm, bgm_slot_, failure), "暂停 BGM 成功");
        product_.bgm_mix = false;
        check(play_bgm(first), "接受重复投放已暂停 BGM");
        check(!engine_.slot_state(AudioBus::bgm, bgm_slot_).paused, "混音关闭时重复投放真正恢复声音");

        on_left_down(point_of(ui::PanelLayout::volume_track(layout_.bgm_volume_slider)));
        check(slider_drag_target_ == 103, "开始拖动音量");
        static_cast<void>(handle_message(WM_CAPTURECHANGED, 0, 0, result));
        check(slider_drag_target_ < 0 && !dragging_ && !seeking_, "捕获丢失不会留下粘住鼠标的拖动");
        const float volume = product_.bgm_volume_percent;
        on_mouse_move(POINT{-100, -100});
        check(product_.bgm_volume_percent == volume, "捕获丢失后鼠标移动不再改变音量");

        frame();
        on_left_down(point_of(layout_.bgm_waveform));
        check(seeking_, "开始拖动播放位置");
        on_key_down(VK_ESCAPE);
        check(!seeking_ && pending_seek_seconds_ < 0, "Esc 取消进度拖动且丢弃未提交的位置");
        clear_playback(false);
        clear_playback(true);
    } else skip("播放生命周期回归（缺少音频设备或曲目）");

    if (audio_ready_ && sfx_.visible.size() >= 2) {
        set_panel_visible(true);
        set_dj_mode(true);
        frame();
        const auto first = sfx_.visible[0], second = sfx_.visible[1];
        const auto click_pad = [&](std::size_t visible) {
            const auto card = layout_.card_rect(layout_.sfx_grid, visible, sfx_.scroll.offset());
            const POINT point{static_cast<LONG>(card.center().x), static_cast<LONG>(card.center().y)};
            on_left_down(point); on_left_up(point); frame();
        };
        click_pad(0); click_pad(1);
        check(engine_.slot_state(AudioBus::sfx, sfx_slot_count + first).in_use &&
              engine_.slot_state(AudioBus::sfx, sfx_slot_count + second).in_use,
              "DJ 两个按钮独立发声，不占用彼此台位");
        check(!engine_.slot_state(AudioBus::sfx, sfx_slot_count + first).looping,
              "DJ 点按默认单次播放，忽略唱盘的循环设置");
        check(!dragging_ && !hit_.is_captured(), "DJ 点按不会进入卡片拖动状态");
        set_dj_mode(false); frame();
        check(engine_.slot_state(AudioBus::sfx, sfx_slot_count + first).in_use,
              "切回唱盘不会截断 DJ 音效");
        clear_playback(true); frame();
        check(engine_.active_count(AudioBus::sfx) == 0, "清空同时覆盖唱盘与 DJ 声部");
        const auto root_before = library_root_;
        const auto tracks_before = entries_.size();
        const auto reload = POINT{static_cast<LONG>(layout_.busbar_reload.center().x), static_cast<LONG>(layout_.busbar_reload.center().y)};
        on_left_down(reload); on_left_up(reload);
        check(loading_, "右下角重新加载进入统一加载流程");
        loader_.cancel();
        const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (loading_ && std::chrono::steady_clock::now() < cancel_deadline) { frame(); ::Sleep(5); }
        check(!loading_ && library_root_ == root_before && entries_.size() == tracks_before,
              "取消重新加载保留原曲库与目录设置");
        set_panel_visible(false); frame();
        check(orb_.handle() && ::IsWindowVisible(orb_.handle()), "收起后悬浮球继续常驻");
    } else skip("DJ 与重新加载交互（需要至少两条音效）");

    if(audio_ready_ && !bgm_.visible.empty()) {
        const auto point=[](const ui::Rect& area){ return POINT{static_cast<LONG>(area.center().x),static_cast<LONG>(area.center().y)}; };
        set_panel_visible(true); set_sfx_collapsed(false); frame();
        check(hit_.topmost_at(layout_.busbar_library.center())==-1,"底栏地址只读，不占用点击区域");
        check(hit_.topmost_at(layout_.busbar_change.center())==6 && hit_.topmost_at(layout_.busbar_reload.center())==5,"底栏曲库按钮分别命中");
        check(hit_.topmost_at(layout_.title_pin.center())==3 && layout_.title_pin.x<layout_.title_minimize.x,"图钉位于收起与关闭左侧");
        const bool pinned_before = product_.panel.topmost;
        on_left_down(point(layout_.title_pin)); on_left_up(point(layout_.title_pin)); frame();
        check(product_.panel.topmost != pinned_before && state_.always_on_top == product_.panel.topmost,
            "点击图钉同步设置和绘制状态");
        ProductSettingsStore pin_store; ProductSettingsLoadResult pin_loaded; Failure pin_failure;
        const bool pin_saved = pin_store.load_or_initialize(settings_path_, pin_loaded, pin_failure);
        check(pin_saved && pin_loaded.settings.panel.topmost == !pinned_before,"置顶切换即时保存，重启不恢复默认高亮");
        on_left_down(point(layout_.title_pin)); on_left_up(point(layout_.title_pin)); frame();
        check(product_.panel.topmost == pinned_before && state_.always_on_top == pinned_before,"再点图钉恢复状态，双向同步");
        clear_playback(false); frame();
        check(cursor_for_client_point(point(layout_.bgm_disc))==::LoadCursorW(nullptr,IDC_ARROW),"空唱片无可执行动作，保持箭头");
        const auto index=bgm_.visible.front();
        check(play_bgm(index),"准备唱片悬停及球下部播放控制"); frame();
        const POINT corner{static_cast<LONG>(layout_.bgm_disc.x+1),static_cast<LONG>(layout_.bgm_disc.y+1)};
        check(cursor_for_client_point(corner)==::LoadCursorW(nullptr,IDC_ARROW),"唱片外接矩形角落不是可点击圆面");
        check(cursor_for_client_point(point(layout_.bgm_disc))==::LoadCursorW(nullptr,IDC_HAND),"唱片圆内使用手形鼠标");
        check(cursor_for_client_point(point(layout_.bgm_search_input))==::LoadCursorW(nullptr,IDC_IBEAM),"搜索输入区使用 I 型鼠标");
        const auto orb=static_cast<HWND>(orb_window_handle());
        ::SendMessageW(orb,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(92,127));
        ::SendMessageW(orb,WM_LBUTTONUP,0,MAKELPARAM(92,127)); frame();
        check(engine_.slot_state(AudioBus::bgm,0).paused && panel_visible_,"球下部暂停 BGM，不改变面板显隐");
        ::Sleep(320); frame();
        check(state_.bgm.tonearm==0,"暂停过渡结束唱针确实抬起");
        ::SendMessageW(orb,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(92,127));
        ::SendMessageW(orb,WM_LBUTTONUP,0,MAKELPARAM(92,127)); frame();
        check(!engine_.slot_state(AudioBus::bgm,0).paused,"球下部再次点击继续 BGM");
        if(!sfx_.visible.empty()) check(trigger_pad(sfx_.visible.front()),"折叠前准备独立音效");
        const auto sfx_before=engine_.active_count(AudioBus::sfx);
        set_sfx_collapsed(true); frame();
        check(layout_.panel.width==746 && layout_.sfx_grid.empty() && layout_.title_close.right()<746,"折叠后窗口宽度、隐藏栏和顶栏一致");
        check(engine_.active_count(AudioBus::sfx)==sfx_before,"折叠不会停止音效");
        set_sfx_collapsed(false); frame();
        check(layout_.panel.width==1256 && !layout_.sfx_grid.empty(),"展开恢复完整双栏");
        recent_bgm_.clear(); recent_order_=0;
        const auto available=std::min<std::size_t>(6,bgm_.visible.size());
        for(std::size_t i=0;i<available;++i) queue_playback(bgm_.visible[i],-1);
        queue_playback(index,-1); queue_playback(index,-1); frame();
        const auto ranking=ranked_recent_bgm();
        check(ranking.size()==std::min<std::size_t>(5,available) && ranking.front()==index,"本次 BGM 次数排序正确且最多五条");
        if(available>1) check(ranking[1]==bgm_.visible[available-1],"同次数 BGM 按最近播放先后排列");
        HMENU menu=nullptr; Failure failure;
        const bool built=create_tray_menu(menu,failure);
        check_detail(built,"球右键菜单创建成功",failure.to_string());
        if(built) {
            check(::GetMenuItemCount(menu)==6,"球菜单恰有六项，没有旧的置顶项");
            const std::array<const wchar_t*,6> labels{{L"暂停",L"中止播放",L"播放最近",L"快捷键",L"隐藏面板",L"退出程序"}};
            bool text_ok=true;
            for(int i=0;i<6;++i) {wchar_t label[128]{};::GetMenuStringW(menu,static_cast<UINT>(i),label,128,MF_BYPOSITION);text_ok=text_ok && std::wstring(label)==labels[static_cast<std::size_t>(i)];}
            check(text_ok,"菜单顺序和文案符合六项要求");
            check(::GetMenuItemCount(::GetSubMenu(menu,2))==static_cast<int>(ranking.size()),"播放最近二级菜单与排序结果一致");
            ::DestroyMenu(menu);
        }
        LRESULT result=0; static_cast<void>(handle_message(WM_COMMAND,11,0,result)); frame();
        check(!engine_.slot_state(AudioBus::bgm,0).in_use,"菜单中止结束 BGM");
        ::Sleep(320); frame(); check(state_.bgm.tonearm==0,"中止后唱针过渡回抬起状态");
        clear_playback(true);
    }

    if(audio_ready_ && sfx_.visible.size()>=2 && !bgm_.visible.empty()) {
        set_dj_mode(false); clear_playback(true); frame();
        const auto music = bgm_.visible.front(), effect = sfx_.visible.front(), replacement = sfx_.visible[1];
        check(play_bgm(music),"准备右键音效分配的 BGM 保留基线"); frame();
        const auto right_card=[&](std::size_t visible) {
            const auto area=layout_.card_rect(layout_.sfx_grid,visible,sfx_.scroll.offset());
            on_right_up(POINT{static_cast<LONG>(area.center().x),static_cast<LONG>(area.center().y)}); frame();
        };
        for(std::size_t deck=0;deck<sfx_slot_count;++deck) {
            product_.sfx_slots[deck].loop=true;
            right_card(0);
            check(wait_playing_index(AudioBus::sfx,deck,effect),"音效库右键依次送到空台 "+std::to_string(deck+1));
        }
        right_card(1);
        check(wait_playing_index(AudioBus::sfx,0,replacement),"四台占满时右键覆盖第 1 台");
        check(sfx_index_[1]==effect && sfx_index_[2]==effect && sfx_index_[3]==effect && bgm_index_==music,
            "覆盖第 1 台保留其他音效与 BGM");
        Failure slot_failure;
        check(engine_.pause(AudioBus::sfx,0,slot_failure),"准备暂停但仍占用的音效台");
        slot_failure.clear(); check(engine_.stop(AudioBus::sfx,2,0,slot_failure),"腾出第 3 台");frame();
        right_card(1);
        check(wait_playing_index(AudioBus::sfx,2,replacement) && engine_.slot_state(AudioBus::sfx,0).paused,
            "右键跳过暂停台位，优先填补中间的空位");
        clear_playback(true);clear_playback(false);frame();
    }

    {
        const auto saved_root = library_root_;
        const auto sandbox = product_data_root() / L"selftest-library-switch";
        std::error_code error;
        std::filesystem::create_directories(sandbox / L"BGM" / L"新分类", error);
        std::filesystem::create_directories(sandbox / L"SFX" / L"空分类", error);
        const bool fixture = !error && write_selftest_wav(sandbox / L"BGM" / L"新分类" / L"新曲.wav", 600);
        check(fixture, "准备独立换库素材及空音效分类");
        if (fixture) {
            begin_library_load(sandbox);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (loading_ && std::chrono::steady_clock::now() < deadline) { frame(); ::Sleep(5); }
            check(!loading_ && library_root_ == sandbox && entries_.size() == 1 && entries_[0].prepared,
                "成功换库原子替换目录、曲目及预载数据");
            check(bgm_.categories == std::vector<std::string>{"新分类"} && sfx_.categories == std::vector<std::string>{"空分类"},
                "更换曲库保留新库的空分类，旧分类不残留");
            check(product_.library_folder == path_to_utf8(sandbox), "成功换库才保存新目录");
            begin_library_load(sandbox / L"不存在");
            const auto fail_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (loading_ && std::chrono::steady_clock::now() < fail_deadline) { frame(); ::Sleep(5); }
            check(!loading_ && library_root_ == sandbox && entries_.size() == 1 && status_.find("加载失败") != std::string::npos,
                "换库失败明确提示并保留当前可用曲库");
            begin_library_load(saved_root);
            const auto restore_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (loading_ && std::chrono::steady_clock::now() < restore_deadline) { frame(); ::Sleep(5); }
            check(!loading_ && library_root_ == saved_root, "换库验收结束恢复原目录");
        }
        remove_tree(sandbox);
    }

    if (audio_ready_ && bgm_.visible.size() > 1) {
        clear_playback(false); product_.bgm_fade = false; product_.bgm_mix = true; product_.bgm_loop = true;
        const auto tick_for = [&](double seconds) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (std::chrono::steady_clock::now() < until) { frame(); ::Sleep(8); }
        };
        const auto a = bgm_.visible[0], b = bgm_.visible[1];
        check(play_bgm(a), "混音准备第一首"); tick_for(.08);
        product_.bgm_fade = true;
        check(play_bgm(b), "混音播放第二首"); tick_for(1.25);
        const auto old = engine_.slot_state(AudioBus::bgm, 1u-bgm_slot_);
        const auto current = engine_.slot_state(AudioBus::bgm, bgm_slot_);
        check_detail(old.in_use && current.in_use && old.current_gain > .4f && current.current_gain > .4f,
            "应用切歌实际保留两首并交叉淡化", "old="+std::to_string(old.current_gain)+" new="+std::to_string(current.current_gain));
        toggle_bgm_pause(); tick_for(.3);
        const auto fading = engine_.slot_state(AudioBus::bgm, bgm_slot_);
        check_detail(fading.paused && fading.current_gain > .05f && fading.current_gain < current.current_gain,
            "应用暂停有可测量的淡出", std::to_string(fading.current_gain));
        tick_for(.45);
        const auto paused = engine_.slot_state(AudioBus::bgm, bgm_slot_);
        tick_for(.12);
        check(engine_.active_count(AudioBus::bgm)==1 && paused.current_gain==0 &&
              std::abs(engine_.slot_state(AudioBus::bgm,bgm_slot_).position_seconds-paused.position_seconds)<.02,
              "暂停完成后旧曲无残留且位置冻结");
        toggle_bgm_pause(); tick_for(.3);
        const auto resumed = engine_.slot_state(AudioBus::bgm,bgm_slot_);
        check_detail(!resumed.paused && resumed.current_gain>.1f && resumed.current_gain<.5f,
            "应用继续播放有淡入", std::to_string(resumed.current_gain));
        tick_for(1);
        product_.bgm_mix=false;
        check(play_bgm(a), "关闭混音后换曲"); tick_for(.25);
        check(engine_.slot_state(AudioBus::bgm,bgm_slot_).current_gain==0,
            "普通淡化先等待旧曲淡出");
        tick_for(.7);
        check(engine_.slot_state(AudioBus::bgm,bgm_slot_).current_gain>.1f && engine_.active_count(AudioBus::bgm)==1,
            "旧曲退出后新曲开始淡入");
        clear_playback(false); product_.bgm_mix=true;
    } else skip("BGM 淡化与混音（需要两首音乐及音频设备）");

    std::printf("---- 交互自检: %d/%d 通过，%d 跳过，%d 失败 ----\n", total - failures, total,
                skipped, failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace musicboard::app
