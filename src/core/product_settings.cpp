#include "core/product_settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <map>
#include <sstream>
#include <string_view>

#include "core/file_bytes.h"
#include "core/path_utf8.h"

namespace musicboard {
namespace {

constexpr std::string_view kHeader = "# MusicBoard settings";
// 最后一行必须是它。少了就说明文件被截断 —— 哪怕恰好断在行边界上也要能发现。
constexpr std::string_view kCompleteKey = "complete";


[[nodiscard]] std::string severity_name(ProductSettingsLoadState state) {
    return std::string(product_settings_load_state_name(state));
}

// ---------------------------------------------------------------------------
// 值解析。失败一律返回 false 并让调用方决定怎么处理 ——
// 这里不做"解析不出来就当默认值"的事，那正是要避免的静默回退。
// ---------------------------------------------------------------------------

[[nodiscard]] bool parse_u32(std::string_view text, std::uint32_t& out) {
    if (text.empty()) {
        return false;
    }
    std::uint32_t value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] bool parse_i32(std::string_view text, std::int32_t& out) {
    if (text.empty()) {
        return false;
    }
    std::int32_t value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] bool parse_bool(std::string_view text, bool& out) {
    if (text == "1" || text == "true") {
        out = true;
        return true;
    }
    if (text == "0" || text == "false") {
        out = false;
        return true;
    }
    return false;
}

// 转义：反斜杠必须先处理，否则会把后续转义再转一次。
[[nodiscard]] std::string escape_value(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (const char ch : raw) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '=':  out += "\\e"; break;
            default:   out.push_back(ch);
        }
    }
    return out;
}

// 反转义。遇到不认识的转义序列时原样保留，不报错 ——
// 这样以后加新的转义不会让旧文件直接读不了。
[[nodiscard]] std::string unescape_value(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\' || i + 1 >= raw.size()) {
            out.push_back(raw[i]);
            continue;
        }
        const char next = raw[++i];
        switch (next) {
            case '\\': out.push_back('\\'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 'e':  out.push_back('='); break;
            default:
                out.push_back('\\');
                out.push_back(next);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 键名的生成与解析
//
// 用统一的前后缀而不是散落的字符串字面量，改键名时只有一处要动。
// ---------------------------------------------------------------------------

std::string slot_key(std::size_t index, std::string_view field) {
    return "sfx" + std::to_string(index) + "_" + std::string(field);
}

std::string hotkey_key(std::size_t index, std::string_view field) {
    return "hotkey" + std::to_string(index) + "_" + std::string(field);
}

// 把越界取值夹回合法区间，并记一条 note —— 不做静默纠正
[[nodiscard]] std::uint16_t clamp_volume(std::uint32_t raw, std::string_view field,
                                         std::vector<std::string>& notes) {
    if (raw > maximum_volume_percent) {
        notes.push_back(std::string(field) + "=" + std::to_string(raw) + " 超出 0..100，已夹到 " +
                        std::to_string(maximum_volume_percent));
        return maximum_volume_percent;
    }
    return static_cast<std::uint16_t>(raw);
}

// ---------------------------------------------------------------------------
// 迁移
//
// 从 source_schema 升到当前版本。每加一版就在这里补一段，
// 并且每一步都要能被单独测到。
// ---------------------------------------------------------------------------
void apply_migration(ProductSettings& settings, std::uint32_t source_schema,
                     std::vector<std::string>& notes) {
    // schema 1 是首版，没有需要迁移的历史版本。
    // 这个函数现在只在 source_schema < 1 时被调用（即文件里的版本号是 0，
    // 意味着早期手写格式），保持默认值即可，但要留痕。
    if (source_schema == 0) {
        notes.push_back("文件版本号为 0，按首版处理，字段取默认值");
        (void)settings;
    }

    // schema 2 加了全局快捷键。旧文件里没有这几个键，读的时候会保持
    // HotkeySettings 的默认值（默认值定义在结构体上，见头文件 ——
    // 那样首次安装与升级两条路拿到的是同一份默认值）。
    // 这里**只需要留一条 note**：用户升级之后快捷键会突然开始生效
    // （Ctrl+Alt+1..4 会从别的程序手里拿走按键）。这不是静默纠正，
    // 得让界面有机会告诉他"快捷键已启用，可以在设置里改或关掉"。
    if (source_schema < 4) notes.push_back("新增 DJ 按钮模式，保留原唱盘形态，可在音效列切换");
    if (source_schema < 7) notes.push_back("移除从头播放设置，新增默认开启的混音与中英语言设置");
    if (source_schema < 6) notes.push_back("新增显示全部开关，默认隐藏两列的全部分类选项");
    if (source_schema < 5) notes.push_back("新增音效栏折叠设置，默认完整双栏显示");
    if (source_schema < 3) {
        // 旧文件没有"总在最前"，取默认值（开）。这里不需要额外 note：
        // 之前的行为本来就是不置顶，改成置顶是修 bug，不是用户的设置被改动。
        settings.panel.topmost = true;
    }

    if (source_schema < 2) {
        notes.push_back("旧设置文件没有快捷键配置，已启用默认的 Ctrl+Alt+1..4；"
                        "如不需要可在设置里关掉");
    }
}

struct ParsedFile final {
    std::uint32_t schema = 0;
    bool saw_complete = false;
    std::map<std::string, std::string> values;
};

[[nodiscard]] bool parse_file(const std::vector<std::uint8_t>& bytes, ParsedFile& out,
                              std::string& error) {
    const std::string text(bytes.begin(), bytes.end());
    std::istringstream stream(text);
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(stream, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();   // 容忍 CRLF
        }
        if (line_number == 1) {
            if (line != kHeader) {
                error = "第 1 行不是预期的文件头";
                return false;
            }
            continue;
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            error = "第 " + std::to_string(line_number) + " 行缺少 '='";
            return false;
        }
        const std::string key = line.substr(0, separator);
        const std::string raw_value = line.substr(separator + 1);
        if (key.empty()) {
            error = "第 " + std::to_string(line_number) + " 行键名为空";
            return false;
        }
        out.values[key] = unescape_value(raw_value);
        if (key == kCompleteKey) {
            out.saw_complete = true;
        }
        if (key == "schema") {
            // 这一行必须有 —— 少了它 schema 恒为 0，所有文件都会被当成"旧版本待迁移"。
            // 实测漏掉过：11 条测试同时失败，根因就这一处。
            if (!parse_u32(out.values[key], out.schema)) {
                error = "第 " + std::to_string(line_number) + " 行的 schema 不是合法整数: " +
                        out.values[key];
                return false;
            }
        }
    }
    return true;
}

// 把解析出来的键值对填进设置对象。任何一处解析失败都算文件不可用。
[[nodiscard]] bool build_settings(const ParsedFile& parsed, ProductSettings& settings,
                                  std::vector<std::string>& notes, std::string& error) {
    const auto find = [&](std::string_view key) -> const std::string* {
        const auto it = parsed.values.find(std::string(key));
        return it == parsed.values.end() ? nullptr : &it->second;
    };

    const auto read_u32 = [&](std::string_view key, std::uint32_t fallback) -> std::uint32_t {
        const std::string* text = find(key);
        if (text == nullptr) {
            return fallback;
        }
        std::uint32_t value = 0;
        if (!parse_u32(*text, value)) {
            error = "键 " + std::string(key) + " 的值不是合法整数: " + *text;
        }
        return value;
    };
    const auto read_i32 = [&](std::string_view key, std::int32_t fallback) -> std::int32_t {
        const std::string* text = find(key);
        if (text == nullptr) {
            return fallback;
        }
        std::int32_t value = 0;
        if (!parse_i32(*text, value)) {
            error = "键 " + std::string(key) + " 的值不是合法整数: " + *text;
        }
        return value;
    };
    const auto read_bool = [&](std::string_view key, bool fallback) -> bool {
        const std::string* text = find(key);
        if (text == nullptr) {
            return fallback;
        }
        bool value = false;
        if (!parse_bool(*text, value)) {
            error = "键 " + std::string(key) + " 的值不是 0/1: " + *text;
        }
        return value;
    };

    if (const std::string* folder = find("library_folder")) {
        settings.library_folder = *folder;
    }
    if (const std::string* locale = find("language")) {
        if (*locale == "en") settings.ui_language = Language::english;
        else if (*locale == "cn") settings.ui_language = Language::chinese;
        else error = "Unsupported language value: " + *locale;
    }
    settings.bgm_volume_percent = clamp_volume(read_u32("bgm_volume", settings.bgm_volume_percent),
                                               "bgm_volume", notes);
    settings.sfx_volume_percent = clamp_volume(read_u32("sfx_volume", settings.sfx_volume_percent),
                                               "sfx_volume", notes);
    settings.bgm_loop = read_bool("bgm_loop", settings.bgm_loop);
    settings.bgm_fade = read_bool("bgm_fade", settings.bgm_fade);
    settings.sfx_dj_mode = read_bool("sfx_dj_mode", settings.sfx_dj_mode);
    settings.sfx_collapsed = read_bool("sfx_collapsed", settings.sfx_collapsed);
    settings.show_all_categories = read_bool("show_all_categories", settings.show_all_categories);
    settings.bgm_mix = read_bool("bgm_mix", settings.bgm_mix);

    for (std::size_t i = 0; i < sfx_slot_count; ++i) {
        SfxSlotSettings& slot = settings.sfx_slots[i];
        slot.volume_percent = clamp_volume(read_u32(slot_key(i, "volume"), slot.volume_percent),
                                           slot_key(i, "volume"), notes);
        slot.loop = read_bool(slot_key(i, "loop"), slot.loop);
        slot.fade = read_bool(slot_key(i, "fade"), slot.fade);
    }

    settings.panel.x = read_i32("panel_x", settings.panel.x);
    settings.panel.y = read_i32("panel_y", settings.panel.y);
    settings.panel.width = read_i32("panel_width", settings.panel.width);
    settings.panel.height = read_i32("panel_height", settings.panel.height);
    settings.panel.visible = read_bool("panel_visible", settings.panel.visible);
    settings.panel.topmost = read_bool("panel_topmost", settings.panel.topmost);

    settings.orb.x = read_i32("orb_x", settings.orb.x);
    settings.orb.y = read_i32("orb_y", settings.orb.y);

    settings.hotkeys.enabled = read_bool("hotkeys_enabled", settings.hotkeys.enabled);
    for (std::size_t i = 0; i < sfx_slot_count; ++i) {
        HotkeyChord& binding = settings.hotkeys.decks[i];
        binding.modifiers = read_u32(hotkey_key(i, "modifiers"), binding.modifiers);
        binding.virtual_key = read_u32(hotkey_key(i, "key"), binding.virtual_key);
    }

    return error.empty();
}

[[nodiscard]] std::string format_settings(const ProductSettings& settings) {
    std::ostringstream out;
    out << kHeader << "\n";
    out << "# schema 变了就要在这里加迁移，见 product_settings.cpp 的 apply_migration\n";
    out << "schema=" << product_settings_current_schema << "\n";
    out << "language=" << (settings.ui_language == Language::english ? "en" : "cn") << "\n";
    out << "library_folder=" << escape_value(settings.library_folder) << "\n";
    out << "bgm_volume=" << settings.bgm_volume_percent << "\n";
    out << "sfx_volume=" << settings.sfx_volume_percent << "\n";
    out << "bgm_loop=" << (settings.bgm_loop ? 1 : 0) << "\n";
    out << "bgm_fade=" << (settings.bgm_fade ? 1 : 0) << "\n";
    out << "sfx_dj_mode=" << (settings.sfx_dj_mode ? 1 : 0) << "\n";
    out << "sfx_collapsed=" << (settings.sfx_collapsed ? 1 : 0) << "\n";
    out << "show_all_categories=" << (settings.show_all_categories ? 1 : 0) << "\n";
    out << "bgm_mix=" << (settings.bgm_mix ? 1 : 0) << "\n";
    for (std::size_t i = 0; i < sfx_slot_count; ++i) {
        const SfxSlotSettings& slot = settings.sfx_slots[i];
        out << slot_key(i, "volume") << "=" << slot.volume_percent << "\n";
        out << slot_key(i, "loop") << "=" << (slot.loop ? 1 : 0) << "\n";
        out << slot_key(i, "fade") << "=" << (slot.fade ? 1 : 0) << "\n";
    }
    out << "panel_x=" << settings.panel.x << "\n";
    out << "panel_y=" << settings.panel.y << "\n";
    out << "panel_width=" << settings.panel.width << "\n";
    out << "panel_height=" << settings.panel.height << "\n";
    out << "panel_visible=" << (settings.panel.visible ? 1 : 0) << "\n";
    out << "panel_topmost=" << (settings.panel.topmost ? 1 : 0) << "\n";
    out << "orb_x=" << settings.orb.x << "\n";
    out << "orb_y=" << settings.orb.y << "\n";
    out << "hotkeys_enabled=" << (settings.hotkeys.enabled ? 1 : 0) << "\n";
    for (std::size_t i = 0; i < sfx_slot_count; ++i) {
        const HotkeyChord& binding = settings.hotkeys.decks[i];
        out << hotkey_key(i, "modifiers") << "=" << binding.modifiers << "\n";
        out << hotkey_key(i, "key") << "=" << binding.virtual_key << "\n";
    }
    // 必须最后一行。缺了它，读的时候就知道文件是截断的。
    out << kCompleteKey << "=1\n";
    return out.str();
}

// 往文件名里插一段后缀，保留扩展名：a.json -> a.corrupt-20260916-131500.json
[[nodiscard]] std::filesystem::path with_suffix(const std::filesystem::path& path,
                                               const std::string& suffix) {
    std::filesystem::path result = path.parent_path();
    result /= path.stem().wstring() + std::wstring(suffix.begin(), suffix.end()) + path.extension().wstring();
    return result;
}

[[nodiscard]] bool write_bytes(const std::filesystem::path& path, std::string_view text,
                               Failure& failure) {
    const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        failure.set(fail_code::file_open_failed,
                    "无法写入 " + path_to_utf8(path) + " (Win32 错误 " +
                        std::to_string(::GetLastError()) + ")");
        return false;
    }
    DWORD written = 0;
    const bool ok = ::WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written,
                                nullptr) != 0 &&
                    written == text.size();
    if (!ok) {
        failure.set(fail_code::file_read_failed,
                    "写入长度不符 " + path_to_utf8(path) + " (Win32 错误 " +
                        std::to_string(::GetLastError()) + ")");
    }
    // 先 FlushFileBuffers 再关，保证内容真的落盘；否则"原子替换"换上去的可能是空文件
    if (ok) {
        ::FlushFileBuffers(handle);
    }
    ::CloseHandle(handle);
    return ok;
}

}  // namespace

const char* product_settings_load_state_name(ProductSettingsLoadState state) noexcept {
    switch (state) {
        case ProductSettingsLoadState::initialized_defaults: return "initialized_defaults";
        case ProductSettingsLoadState::loaded:               return "loaded";
        case ProductSettingsLoadState::migrated:             return "migrated";
        case ProductSettingsLoadState::corrupt_isolated:     return "corrupt_isolated";
    }
    return "unknown";
}

std::filesystem::path product_data_root() {
    // 用宽字符 API 取：%LOCALAPPDATA% 里可能有非 ASCII 用户名
    wchar_t buffer[32768]{};
    const DWORD written = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                                   static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(buffer) / L"MusicBoard";
}

std::filesystem::path default_settings_path() {
    const std::filesystem::path root = product_data_root();
    if (root.empty()) {
        return {};
    }
    return root / L"settings.conf";
}

bool ProductSettingsStore::load_or_initialize(const std::filesystem::path& path,
                                              ProductSettingsLoadResult& result,
                                              Failure& failure) const {
    result = ProductSettingsLoadResult{};

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        result.state = ProductSettingsLoadState::initialized_defaults;
        result.settings = ProductSettings{};
        return true;
    }

    // 读文件
    std::vector<std::uint8_t> bytes;
    Failure read_failure;
    std::string problem;
    if (!read_file_bytes(path, bytes, read_failure)) {
        problem = "读取失败: " + read_failure.to_string();
    } else {
        ParsedFile parsed;
        if (!parse_file(bytes, parsed, problem)) {
            // problem 已填
        } else if (!parsed.saw_complete) {
            problem = "文件缺少结束标记 complete=1，判断为被截断";
        } else if (parsed.schema > product_settings_current_schema) {
            // 更新的版本写的文件。覆盖它会丢掉用户的设置，所以隔离。
            problem = "文件 schema=" + std::to_string(parsed.schema) + " 高于本程序支持的 " +
                      std::to_string(product_settings_current_schema) + "，可能来自更新的版本";
        } else {
            ProductSettings settings;
            std::vector<std::string> notes;
            if (!build_settings(parsed, settings, notes, problem)) {
                // problem 已填
            } else {
                if (parsed.schema < product_settings_current_schema) {
                    apply_migration(settings, parsed.schema, notes);
                    result.state = ProductSettingsLoadState::migrated;
                } else {
                    result.state = ProductSettingsLoadState::loaded;
                }
                result.settings = settings;
                result.source_schema = parsed.schema;
                result.notes = std::move(notes);
                return true;
            }
        }
    }

    // 走到这里说明内容不可用：隔离原文件，从默认值开始。
    // 关键：是"挪走"而不是"删掉"或"清空" —— 用户还能自己去捞。
    const std::string stamp = [] {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        localtime_s(&local, &t);
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), ".corrupt-%04d%02d%02d-%02d%02d%02d",
                      local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
                      local.tm_min, local.tm_sec);
        return std::string(buffer);
    }();

    const std::filesystem::path isolated = with_suffix(path, stamp);
    std::error_code rename_ec;
    std::filesystem::rename(path, isolated, rename_ec);
    if (rename_ec) {
        // 挪不动是真正的系统级失败：这时不能假装无事发生从默认值开始，
        // 那样下次保存就会覆盖掉用户那个坏文件。
        failure.set(fail_code::file_open_failed,
                    "设置内容不可用（" + problem + "），且隔离原文件失败: " + rename_ec.message());
        return false;
    }

    result.settings = ProductSettings{};
    result.state = ProductSettingsLoadState::corrupt_isolated;
    result.source_schema = 0;
    result.isolated_path = isolated;
    result.isolation_reason = problem;
    return true;
}

bool ProductSettingsStore::save_atomic(const std::filesystem::path& path,
                                       const ProductSettings& settings, Failure& failure) const {
    std::error_code ec;
    const std::filesystem::path directory = path.parent_path();
    if (!directory.empty()) {
        std::filesystem::create_directories(directory, ec);
        if (ec && !std::filesystem::is_directory(directory)) {
            failure.set(fail_code::file_open_failed,
                        "无法创建设置目录 " + path_to_utf8(directory) + ": " + ec.message());
            return false;
        }
    }

    // 临时文件与目标同目录 —— 跨盘的 MoveFileEx 不是原子的
    const std::filesystem::path temp = with_suffix(path, ".tmp");

    const std::string text = format_settings(settings);
    if (!write_bytes(temp, text, failure)) {
        return false;
    }

    // MOVEFILE_REPLACE_EXISTING: 覆盖目标
    // MOVEFILE_WRITE_THROUGH:   等这次移动真正落盘再返回
    if (!::MoveFileExW(temp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = ::GetLastError();
        std::error_code cleanup_ec;
        std::filesystem::remove(temp, cleanup_ec);   // 别把临时文件留在那里
        failure.set(fail_code::file_open_failed,
                    "替换设置文件失败 " + path_to_utf8(path) + " (Win32 错误 " +
                        std::to_string(error) + ")");
        return false;
    }
    return true;
}

}  // namespace musicboard
