// 设置存储测试
//
// 冲着的核心风险：**文件坏了会不会把用户的配置一起丢掉**。
// 所以大部分用例是在构造各种损坏形态，验证原文件被隔离而不是被清空。
//
// 另一个重点是原子保存：中途失败不能留下半个目标文件。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/file_bytes.h"
#include "core/path_utf8.h"
#include "core/product_settings.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;

namespace {

std::filesystem::path temp_dir() {
    static const std::filesystem::path dir = [] {
        std::filesystem::path d = std::filesystem::temp_directory_path() / L"musicboard_settings_tests";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return dir;
}

// 每次用例用一个独立子目录，避免互相干扰
std::filesystem::path case_dir(const char* name) {
    const std::filesystem::path dir = temp_dir() / std::wstring(name, name + std::strlen(name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// 用宽字符 API 写文件，绕开窄字符路径问题
bool write_text(const std::filesystem::path& path, const std::string& text) {
    const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = ::WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written,
                                nullptr) != 0 &&
                    written == text.size();
    ::CloseHandle(handle);
    return ok;
}

std::string read_text(const std::filesystem::path& path) {
    std::vector<std::uint8_t> bytes;
    Failure failure;
    if (!read_file_bytes(path, bytes, failure)) {
        return {};
    }
    return std::string(bytes.begin(), bytes.end());
}

std::size_t count_files_in(const std::filesystem::path& dir) {
    std::size_t count = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) {
            ++count;
        }
    }
    return count;
}

void test_missing_file_uses_defaults(Registry& r) {
    const auto dir = case_dir("missing");
    const auto path = dir / L"settings.conf";

    ProductSettingsStore store;
    ProductSettingsLoadResult result;
    Failure failure;
    r.check(store.load_or_initialize(path, result, failure), "文件不存在时读取成功");
    r.check(result.state == ProductSettingsLoadState::initialized_defaults,
            "文件不存在 → initialized_defaults");
    r.check(result.settings == ProductSettings{}, "文件不存在 → 全是默认值");
    r.check(result.isolated_path.empty(), "文件不存在 → 没有隔离");
    r.check(failure.empty(), "文件不存在不算错误");
}

void test_round_trip(Registry& r) {
    const auto dir = case_dir("roundtrip");
    const auto path = dir / L"settings.conf";

    ProductSettings original;
    // 中文路径 + 全角符号，直接取自真实曲库的目录风格
    original.library_folder = "D:/音乐板/audio/BGM/01_休息与安全";
    original.bgm_volume_percent = 73;
    original.sfx_volume_percent = 41;
    original.bgm_loop = false;
    original.bgm_fade = true;
    original.bgm_mix = true;
    original.show_all_categories = true;
    original.ui_language = Language::english;
    original.sfx_slots[0] = SfxSlotSettings{55, false, true};
    original.sfx_slots[2] = SfxSlotSettings{12, true, true};
    original.panel = PanelGeometry{120, 80, 1884, 1182, false};
    original.orb = OrbGeometry{2600, 900};

    ProductSettingsStore store;
    Failure failure;
    r.check_invocation([&] { return store.save_atomic(path, original, failure); }, "保存成功", failure);

    ProductSettingsLoadResult result;
    Failure load_failure;
    r.check_invocation([&] { return store.load_or_initialize(path, result, load_failure); }, "读取成功", load_failure);
    r.check(result.state == ProductSettingsLoadState::loaded, "往返后状态是 loaded");
    r.check(result.settings == original, "往返后设置完全一致");
    r.check(result.settings.library_folder == original.library_folder,
            "中文路径往返后一字不差");
    r.check(result.settings.sfx_slots[0] == original.sfx_slots[0], "第 1 台音效设置往返一致");
    r.check(result.settings.sfx_slots[1] == SfxSlotSettings{},
            "没改过的第 2 台保持设计默认值（循环开、淡入关）");
    r.check(result.notes.empty(), "正常读取不留纠正记录");
    r.check(failure.empty() && load_failure.empty(), "正常往返没有错误");
}

void test_no_temp_left_behind(Registry& r) {
    const auto dir = case_dir("notemp");
    const auto path = dir / L"settings.conf";

    ProductSettingsStore store;
    Failure failure;
    r.check(store.save_atomic(path, ProductSettings{}, failure), "首次保存成功");
    r.check(count_files_in(dir) == 1, "保存后目录里只有一个文件（临时文件已被替换掉）");

    // 再存一次，覆盖已有文件
    ProductSettings changed;
    changed.bgm_volume_percent = 42;
    r.check(store.save_atomic(path, changed, failure), "覆盖保存成功");
    r.check(count_files_in(dir) == 1, "覆盖保存后仍然只有一个文件");

    ProductSettingsLoadResult result;
    r.check(store.load_or_initialize(path, result, failure), "覆盖后能读回");
    r.check(result.settings.bgm_volume_percent == 42, "读到的是覆盖后的值");
}

// 用**当前** schema 号拼一个设置文件头。
//
// 不要在这些"与迁移无关"的用例里钉死 schema 号：每次升版，它们都会静默变成
// 迁移用例 —— 状态从 loaded 变成 migrated、notes 里多出一条迁移说明，
// 于是断言莫名其妙地红了，而红的原因跟它们要测的东西毫无关系。
// 实测在 schema 1→2 时就这么红了两条。
std::string current_schema_header() {
    return "# MusicBoard settings\nschema=" +
           std::to_string(product_settings_current_schema) + "\n";
}

void test_corrupt_forms_are_isolated(Registry& r) {
    struct Case {
        const char* name;
        const char* content;
        const char* expect_reason_fragment;
    };
    const Case cases[] = {
        {"bad_header", "这不是设置文件\nschema=1\ncomplete=1\n", "文件头"},
        {"no_equals", "# MusicBoard settings\nschema=1\n这行没有等号\ncomplete=1\n", "缺少"},
        {"truncated", "# MusicBoard settings\nschema=1\nbgm_volume=50\n", "截断"},
        {"bad_integer", "# MusicBoard settings\nschema=1\nbgm_volume=abc\ncomplete=1\n", "不是合法整数"},
        {"future_schema", "# MusicBoard settings\nschema=999\ncomplete=1\n", "高于本程序支持"},
    };

    for (const Case& item : cases) {
        const auto dir = case_dir(item.name);
        const auto path = dir / L"settings.conf";
        const std::string original = item.content;
        r.check(write_text(path, original), std::string("写好损坏样本: ") + item.name);

        ProductSettingsStore store;
        ProductSettingsLoadResult result;
        Failure failure;
        const bool ok = store.load_or_initialize(path, result, failure);
        r.check_eq(ok, std::string("损坏样本仍能返回结果: ") + item.name, failure.to_string());
        if (!ok) {
            continue;
        }
        r.check_eq(result.state == ProductSettingsLoadState::corrupt_isolated,
                   std::string("判定为 corrupt_isolated: ") + item.name,
                   product_settings_load_state_name(result.state));
        r.check(result.settings == ProductSettings{},
                std::string("从默认值开始: ") + item.name);
        r.check(!result.isolated_path.empty(), std::string("给出了隔离后的路径: ") + item.name);
        r.check(result.isolation_reason.find(item.expect_reason_fragment) != std::string::npos,
                std::string("隔离原因里点明了问题: ") + item.name + " → " + result.isolation_reason);
        r.check(!std::filesystem::exists(path),
                std::string("原路径上的坏文件已被挪走: ") + item.name);
        r.check_eq(read_text(result.isolated_path) == original,
                   std::string("隔离文件内容原样保留（没有清空）: ") + item.name,
                   "隔离文件=" + path_to_utf8(result.isolated_path));
    }
}

void test_out_of_range_is_clamped_with_note(Registry& r) {
    const auto dir = case_dir("clamp");
    const auto path = dir / L"settings.conf";
    // 音量越界不该让整份设置作废，但也不能静默纠正
    r.check(write_text(path, current_schema_header() +
                       "bgm_volume=250\n"
                       "sfx_volume=100\n"
                       "complete=1\n"),
            "写好越界样本");

    ProductSettingsStore store;
    ProductSettingsLoadResult result;
    Failure failure;
    r.check(store.load_or_initialize(path, result, failure), "越界样本仍可读");
    r.check(result.state == ProductSettingsLoadState::loaded, "越界不算损坏");
    r.check(result.settings.bgm_volume_percent == maximum_volume_percent, "越界音量被夹到上限");
    r.check(result.settings.sfx_volume_percent == 100, "同文件里正常的字段不受影响");
    r.check(result.notes.size() == 1, "记录了 1 条纠正说明");
    if (!result.notes.empty()) {
        r.check(result.notes.front().find("bgm_volume") != std::string::npos,
                "纠正说明里点明了字段名");
    }
}

void test_unknown_keys_are_ignored(Registry& r) {
    const auto dir = case_dir("unknown");
    const auto path = dir / L"settings.conf";
    // 未来版本可能加字段。读到不认识的键要跳过而不是判损坏 ——
    // 否则升级回退时旧版本会直接拒绝新版本写的文件。
    r.check(write_text(path, current_schema_header() +
                       "some_future_field=hello\n"
                       "bgm_volume=66\n"
                       "another_future=1\n"
                       "complete=1\n"),
            "写好含未知键的样本");

    ProductSettingsStore store;
    ProductSettingsLoadResult result;
    Failure failure;
    r.check(store.load_or_initialize(path, result, failure), "含未知键的文件仍可读");
    r.check(result.state == ProductSettingsLoadState::loaded, "含未知键不算损坏");
    r.check(result.settings.bgm_volume_percent == 66, "已知字段正常读到");
}

void test_value_escaping(Registry& r) {
    const auto dir = case_dir("escape");
    const auto path = dir / L"settings.conf";

    // 路径里带 '='、反斜杠、换行 —— 转义必须扛得住，否则会写出一份自己读不回来的文件
    ProductSettings original;
    original.library_folder = "D:/mu=sic\\BGM\n第二行";

    ProductSettingsStore store;
    Failure failure;
    r.check(store.save_atomic(path, original, failure), "含特殊字符的设置保存成功");

    ProductSettingsLoadResult result;
    r.check(store.load_or_initialize(path, result, failure), "含特殊字符的设置能读回");
    r.check(result.settings.library_folder == original.library_folder,
            "含 '='、反斜杠、换行的值转义往返无损");
}

void test_schema_migration(Registry& r) {
    const auto dir = case_dir("migrate");
    const auto path = dir / L"settings.conf";
    // schema=0：早期格式。应当被识别为旧版本并迁移，而不是判损坏。
    r.check(write_text(path,
                       "# MusicBoard settings\n"
                       "schema=0\n"
                       "bgm_volume=80\n"
                       "complete=1\n"),
            "写好旧版本样本");

    ProductSettingsStore store;
    ProductSettingsLoadResult result;
    Failure failure;
    r.check(store.load_or_initialize(path, result, failure), "旧版本文件可读");
    r.check(result.state == ProductSettingsLoadState::migrated, "旧版本 → migrated");
    r.check(result.source_schema == 0, "记录了源版本号");
    r.check(result.settings.bgm_volume_percent == 80, "迁移时保留能读到的字段");
    r.check(!result.notes.empty(), "迁移留下了说明");
    r.check(!std::filesystem::exists(std::filesystem::path(path_to_utf8(path) + ".corrupt")),
            "迁移不会产生隔离文件");
}

void test_hotkey_settings(Registry& r) {
    const auto dir = case_dir("hotkeys");
    const auto path = dir / L"settings.conf";

    ProductSettings defaults;
    r.check(defaults.hotkeys.enabled, "默认开启全局快捷键（这是这个功能存在的理由）");
    r.check(defaults.hotkeys.decks[0].is_bound(), "台位 0 默认有绑定");

    ProductSettingsStore store;
    Failure failure;
    ProductSettings settings;
    settings.hotkeys.enabled = false;
    settings.hotkeys.decks[2].modifiers = 0x0002u | 0x0001u | 0x4000u;
    settings.hotkeys.decks[2].virtual_key = 'K';
    r.check_invocation([&] { return store.save_atomic(path, settings, failure); },
                       "保存带快捷键的设置", failure);

    ProductSettingsLoadResult loaded;
    r.check_invocation([&] { return store.load_or_initialize(path, loaded, failure); },
                       "读回设置", failure);
    r.check(loaded.state == ProductSettingsLoadState::loaded,
            "是 loaded（版本一致，没有触发迁移）");
    r.check(!loaded.settings.hotkeys.enabled, "总开关往返无损");
    r.check(loaded.settings.hotkeys.decks[2].virtual_key == 'K', "台位 2 的按键往返无损");
    r.check(loaded.settings.hotkeys.decks[2].modifiers == (0x0002u | 0x0001u | 0x4000u),
            "台位 2 的修饰键往返无损");
    r.check(loaded.settings.hotkeys == settings.hotkeys, "整块快捷键设置相等");
    r.check(loaded.settings == settings, "整个设置对象相等");

    // "我不要这个台位的快捷键"是合法状态，也要能往返
    ProductSettings unbound;
    unbound.hotkeys.decks[1].modifiers = 0;
    unbound.hotkeys.decks[1].virtual_key = 0;
    r.check_invocation([&] { return store.save_atomic(path, unbound, failure); },
                       "保存含未绑定台位的设置", failure);
    ProductSettingsLoadResult reloaded;
    r.check_invocation([&] { return store.load_or_initialize(path, reloaded, failure); },
                       "再读", failure);
    r.check(!reloaded.settings.hotkeys.decks[1].is_bound(), "未绑定状态往返无损");
}

void test_hotkey_migration_from_old_schema(Registry& r) {
    const auto dir = case_dir("hotkey-migrate");
    const auto path = dir / L"settings.conf";
    // schema=1 的文件里没有快捷键配置
    r.check(write_text(path,
                       "# MusicBoard settings\n"
                       "schema=1\n"
                       "bgm_volume=70\n"
                       "complete=1\n"),
            "写一份 schema 1 的设置");

    ProductSettingsStore store;
    ProductSettingsLoadResult result;
    Failure failure;
    r.check_invocation([&] { return store.load_or_initialize(path, result, failure); },
                       "schema 1 的文件可读", failure);
    r.check(result.state == ProductSettingsLoadState::migrated, "状态是 migrated");
    r.check(result.settings.bgm_volume_percent == 70, "迁移时保留原有字段");
    r.check(result.settings.hotkeys.enabled, "迁移补上了默认快捷键（默认是开的）");
    r.check(result.settings.hotkeys.decks[0].virtual_key == '1' &&
                result.settings.hotkeys.decks[3].virtual_key == '4',
            "默认绑定是 1..4");
    r.check(result.settings.hotkeys.decks[0].modifiers != 0, "默认带修饰键");

    // 必须留痕：升级之后全局快捷键会突然开始生效（会从别的程序手里拿走按键），
    // 用户该有个机会知道这件事。静默启用一个全局快捷键是不能接受的。
    bool mentions_hotkey = false;
    for (const std::string& note : result.notes) {
        if (note.find("快捷键") != std::string::npos) {
            mentions_hotkey = true;
        }
    }
    r.check(mentions_hotkey, "迁移说明里提到了快捷键已启用（不静默启用全局快捷键）");
}

void test_migration_then_save_upgrades(Registry& r) {
    const auto dir = case_dir("upgrade");
    const auto path = dir / L"settings.conf";
    r.check(write_text(path, "# MusicBoard settings\nschema=0\nbgm_volume=80\ncomplete=1\n"),
            "写好旧版本样本");

    ProductSettingsStore store;
    ProductSettingsLoadResult result;
    Failure failure;
    r.check(store.load_or_initialize(path, result, failure), "读到旧版本");
    r.check(result.state == ProductSettingsLoadState::migrated, "状态是 migrated");

    // 读出来再存一次，文件里应当已经写成当前版本
    r.check(store.save_atomic(path, result.settings, failure), "保存迁移后的设置");
    ProductSettingsLoadResult second;
    r.check(store.load_or_initialize(path, second, failure), "再读");
    r.check(second.state == ProductSettingsLoadState::loaded, "再读时已是 loaded（版本已升到当前）");
    r.check(second.settings.bgm_volume_percent == 80, "迁移过程中字段没有丢");
}

}  // namespace

int main() {
    Registry r;
    test_missing_file_uses_defaults(r);
    test_round_trip(r);
    test_no_temp_left_behind(r);
    test_corrupt_forms_are_isolated(r);
    test_out_of_range_is_clamped_with_note(r);
    test_unknown_keys_are_ignored(r);
    test_value_escaping(r);
    test_schema_migration(r);
    test_hotkey_settings(r);
    test_hotkey_migration_from_old_schema(r);
    test_migration_then_save_upgrades(r);
    return r.finish("settings_tests");
}
