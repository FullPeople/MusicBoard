#pragma once

#include "core/localization.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 产品设置
//
// 三条纪律写在这个文件里，因为它们决定了实现的形状：
//
//   1. **文件坏了不清空。** 读到损坏的内容时把原文件挪到一边（隔离）并说明原因，
//      然后从默认值开始。绝不"悄悄清空"——那会连同用户的配置一起丢掉。
//   2. **版本要能迁移。** 每个文件都带 schema 号；读到旧版本按迁移链升级。
//      读到比本程序更新的版本时，同样是隔离而不是覆盖（那是新版本写的文件）。
//   3. **保存是原子的。** 先写同目录下的临时文件，再替换目标。
//      中途掉电最多丢掉这次改动，不会留下半个文件。
//
// 存储格式刻意用行式 key=value 而不是 JSON：
//   · 两端都由本项目控制，JSON 换不来任何东西
//   · 行式格式的解析错误容易精确定位（第几行）
//   · 末尾写一行 complete=1 就能检测"被截断但恰好断在行边界"这种最阴的情况
// ============================================================================

// 每加一个字段或改一次语义就 +1，并在 apply_migration 里补一段。
//   schema 2：加了全局快捷键（音效台 1..4 的绑定 + 总开关）
//   schema 3：加了"面板总在最前"（默认开 —— 跑团时它要浮在浏览器与 PDF 之上）
inline constexpr std::uint32_t product_settings_current_schema = 7;

inline constexpr std::uint16_t default_volume_percent = 100;
inline constexpr std::uint16_t minimum_volume_percent = 0;
inline constexpr std::uint16_t maximum_volume_percent = 100;

// 四条音效台的默认值来自设计稿：循环开、淡入淡出关
inline constexpr bool default_sfx_loop = true;
inline constexpr bool default_sfx_fade = false;
inline constexpr std::size_t sfx_slot_count = 4;

struct SfxSlotSettings final {
    std::uint16_t volume_percent = default_volume_percent;
    bool loop = default_sfx_loop;
    bool fade = default_sfx_fade;

    [[nodiscard]] bool operator==(const SfxSlotSettings& other) const noexcept {
        return volume_percent == other.volume_percent && loop == other.loop && fade == other.fade;
    }
};

struct PanelGeometry final {
    std::int32_t x = -1;          // -1 表示"没记录过，交给系统摆"
    std::int32_t y = -1;
    std::int32_t width = 0;       // 0 表示用设计稿的逻辑尺寸
    std::int32_t height = 0;
    bool visible = true;
    // 总在最前。**默认开**：这个程序就是挂在别的窗口之上的，
    // 不置顶等于每次切到浏览器回来都要重新点一次。
    bool topmost = true;

    [[nodiscard]] bool has_position() const noexcept { return x >= 0 && y >= 0; }
    [[nodiscard]] bool has_size() const noexcept { return width > 0 && height > 0; }

    [[nodiscard]] bool operator==(const PanelGeometry& other) const noexcept {
        return x == other.x && y == other.y && width == other.width && height == other.height &&
               visible == other.visible && topmost == other.topmost;
    }
};

// 一条全局快捷键的**持久化形式**。存的是原始值而不是文本：文本是给人看的，
// 换个说法（"Ctrl" / "控制"）不该改变绑定，也不该让设置文件变得不好解析。
// virtual_key = 0 表示"没绑定"。
//
// 名字叫 HotkeyChord 而不是 HotkeyBinding：后者是 platform/global_hotkeys.h 里
// 那个"要交给 RegisterHotKey 的绑定"。两个东西长得像但职责不同
// （一个管存，一个管注册），同名会让同一个文件里 include 两者时直接编译不过。
struct HotkeyChord final {
    std::uint32_t modifiers = 0;
    std::uint32_t virtual_key = 0;

    [[nodiscard]] bool is_bound() const noexcept { return virtual_key != 0; }
    [[nodiscard]] bool operator==(const HotkeyChord& other) const noexcept {
        return modifiers == other.modifiers && virtual_key == other.virtual_key;
    }
};

struct HotkeySettings final {
    // 总开关。默认开 —— 跑团现场按数字键触发音效是这个功能存在的理由，
    // 但要能关掉：全局快捷键会从别的程序手里拿走按键，有些人不想要。
    bool enabled = true;
    // 四条音效台的绑定，顺序与 sfx_slots 一致（0..3 号台）。
    //
    // 默认值放在**这里**（而不是只写在迁移函数里）：首次安装没有设置文件，
    // 那条路根本不走迁移。一开始只写在迁移里，测试立刻抓到了 ——
    // 全新安装的用户拿到的是"快捷键全都没绑定"，而升级的用户反而有。
    std::array<HotkeyChord, sfx_slot_count> decks = [] {
        std::array<HotkeyChord, sfx_slot_count> values{};
        for (std::size_t i = 0; i < sfx_slot_count; ++i) {
            // MOD_CONTROL | MOD_ALT | MOD_NOREPEAT，数值与 Win32 一致。
            // 设置这一层不 include windows.h（它要能在任何地方被测）。
            // MOD_NOREPEAT 很重要：按住不放时音效被连点几十次是灾难。
            values[i].modifiers = 0x0002u | 0x0001u | 0x4000u;
            values[i].virtual_key = static_cast<std::uint32_t>('1' + i);
        }
        return values;
    }();

    [[nodiscard]] bool operator==(const HotkeySettings& other) const noexcept {
        return enabled == other.enabled && decks == other.decks;
    }
};

struct OrbGeometry final {
    std::int32_t x = -1;
    std::int32_t y = -1;

    [[nodiscard]] bool has_position() const noexcept { return x >= 0 && y >= 0; }

    [[nodiscard]] bool operator==(const OrbGeometry& other) const noexcept {
        return x == other.x && y == other.y;
    }
};

struct ProductSettings final {
    // 曲库根目录。UTF-8；空表示还没选过。
    std::string library_folder;

    // 两条总线各自的音量
    std::uint16_t bgm_volume_percent = default_volume_percent;
    std::uint16_t sfx_volume_percent = default_volume_percent;

    // BGM 台开关。默认：循环开、淡入淡出开、从头播放关（与设计稿一致）
    bool bgm_loop = true;
    bool bgm_fade = true;
    bool bgm_mix = true;

    // 四台音效各自的音量与开关 —— 它们互相独立，所以要逐台存
    std::array<SfxSlotSettings, sfx_slot_count> sfx_slots{};

    bool sfx_dj_mode = false;
    bool sfx_collapsed = false;
    bool show_all_categories = false;
    Language ui_language = Language::chinese;
    PanelGeometry panel;
    OrbGeometry orb;
    HotkeySettings hotkeys;

    [[nodiscard]] bool operator==(const ProductSettings& other) const noexcept {
        return library_folder == other.library_folder &&
               bgm_volume_percent == other.bgm_volume_percent &&
               sfx_volume_percent == other.sfx_volume_percent && bgm_loop == other.bgm_loop &&
               bgm_fade == other.bgm_fade && bgm_mix == other.bgm_mix &&
               sfx_slots == other.sfx_slots && panel == other.panel && orb == other.orb &&
               hotkeys == other.hotkeys && sfx_dj_mode == other.sfx_dj_mode && sfx_collapsed == other.sfx_collapsed && show_all_categories == other.show_all_categories && ui_language == other.ui_language;
    }
};

// 默认设置文件位置：%LOCALAPPDATA%\MusicBoard\settings.conf
//
// 放 %LOCALAPPDATA% 而不是程序目录：程序可能装在 Program Files（不可写），
// 而且同一台机器上不同用户该有各自的设置。取不到环境变量时返回空路径，
// 由调用方明确失败 —— 不静默落到当前目录（那会变成一个"设置怎么丢了"的谜）。
[[nodiscard]] std::filesystem::path default_settings_path();

// 应用数据根目录（%LOCALAPPDATA%\MusicBoard）。日志与暂存区都在它下面。
[[nodiscard]] std::filesystem::path product_data_root();

enum class ProductSettingsLoadState : std::uint8_t {
    initialized_defaults,   // 文件不存在，用默认值
    loaded,                 // 正常读到当前版本
    migrated,               // 读到旧版本并已升级
    corrupt_isolated,       // 内容不可用，原文件已隔离，从默认值开始
};

[[nodiscard]] const char* product_settings_load_state_name(ProductSettingsLoadState state) noexcept;

struct ProductSettingsLoadResult final {
    ProductSettings settings;
    ProductSettingsLoadState state = ProductSettingsLoadState::initialized_defaults;
    std::uint32_t source_schema = product_settings_current_schema;
    // 隔离时原文件被挪到了哪里（为空表示没有发生隔离）
    std::filesystem::path isolated_path;
    // 隔离原因，给人看的一句话
    std::string isolation_reason;
    // 被纠正过的字段（越界取值被夹到合法区间之类）。调用方应当把它写进日志 ——
    // 静默纠正也是一种静默回退。
    std::vector<std::string> notes;
};

class ProductSettingsStore final {
public:
    // 读设置。文件不存在 → initialized_defaults；损坏 → 隔离后从默认值开始。
    // 只有真正的系统级失败（比如隔离时挪不动文件）才返回 false。
    [[nodiscard]] bool load_or_initialize(const std::filesystem::path& path,
                                         ProductSettingsLoadResult& result,
                                         Failure& failure) const;

    // 原子保存：写临时文件 → 替换目标。失败时不留半个目标文件。
    [[nodiscard]] bool save_atomic(const std::filesystem::path& path,
                                   const ProductSettings& settings, Failure& failure) const;
};

}  // namespace musicboard
