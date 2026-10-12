#include "library/library_ops.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/path_utf8.h"

#pragma comment(lib, "shell32.lib")

namespace musicboard {
namespace {

// Windows 文件名里不允许出现的字符（只可能是单字节 ASCII）
constexpr std::string_view kIllegalCharacters = "<>:\"/\\|?*";

// 保留设备名。注意 "CON.txt" 同样被拒 —— Windows 认的是第一个点之前那一段。
constexpr std::array<std::string_view, 22> kReservedNames = {
    "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
    "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
};

[[nodiscard]] std::string uppercase_ascii(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return out;
}

// ---------------------------------------------------------------------------
// 逐码点看字符串
//
// 为什么不能只看字节：一个全角空格 U+3000 在 UTF-8 里是 E3 80 80，
// 看字节完全看不出"这是空白"。用户用中文输入法敲出来的就是一串 U+3000，
// 而 Windows 在存文件名时会把尾部的空格（含 U+3000）吃掉 ——
// 于是"文件名叫'战斗 '、界面上显示的却是'战斗'"这种不一致就出现了。
// 判空白必须按码点来。
// ---------------------------------------------------------------------------

struct DecodedCodePoint final {
    std::uint32_t value = 0;
    std::size_t length = 1;   // 字节数；遇到残缺序列按 1 字节跳过，不在这里报错
};

[[nodiscard]] DecodedCodePoint decode_code_point(std::string_view text, std::size_t offset) {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) {
        return {first, 1};
    }

    std::size_t length = 0;
    std::uint32_t value = 0;
    if ((first & 0xE0) == 0xC0) {
        length = 2;
        value = first & 0x1Fu;
    } else if ((first & 0xF0) == 0xE0) {
        length = 3;
        value = first & 0x0Fu;
    } else if ((first & 0xF8) == 0xF0) {
        length = 4;
        value = first & 0x07u;
    } else {
        return {first, 1};   // 非法起始字节
    }

    if (offset + length > text.size()) {
        return {first, 1};   // 被截断
    }
    for (std::size_t i = 1; i < length; ++i) {
        const auto next = static_cast<unsigned char>(text[offset + i]);
        if ((next & 0xC0) != 0x80) {
            return {first, 1};
        }
        value = (value << 6) | (next & 0x3Fu);
    }
    return {value, length};
}

[[nodiscard]] bool is_space_code_point(std::uint32_t code_point) noexcept {
    switch (code_point) {
        case 0x20:   // 空格
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D:
        case 0xA0:   // 不换行空格
        case 0x1680:
        case 0x2028:
        case 0x2029:
        case 0x202F:
        case 0x205F:
        case 0x3000:   // 全角空格 —— 中文输入法下最常见的那个
            return true;
        default:
            return code_point >= 0x2000 && code_point <= 0x200A;
    }
}

[[nodiscard]] bool is_control_code_point(std::uint32_t code_point) noexcept {
    return code_point < 0x20 || code_point == 0x7F;
}

// ---------------------------------------------------------------------------
// 两条把 UTF-8 字符串变成路径的纪律
//
// **绝对不要写** `directory / std::string(...)` 或 `path / std::string_view`。
// 那个重载把窄字符串按**活动代码页**解释（本机是 936/GBK），
// 于是一个 UTF-8 的中文名字会变成另一个目录名：
//
//     "战斗"（UTF-8，6 字节）→ 按 GBK 读 → 磁盘上多出一个乱码目录
//
// 后果特别隐蔽：create_category 返回成功、目录确实建出来了，
// 但扫描器走宽字符 API，找不到那个名字 —— 用户看到的是"建了分类却不显示"。
// 这个 bug 是 library_ops_tests 抓出来的，不是读代码看出来的。
// ---------------------------------------------------------------------------

// UTF-8 名字 → 路径。不是合法 UTF-8 就返回空路径（调用方必须检查空值）。
[[nodiscard]] std::filesystem::path utf8_path(std::string_view utf8) {
    std::filesystem::path path;
    Failure ignored;
    if (!path_from_utf8(utf8, path, ignored)) {
        return {};
    }
    return path;
}

// 名字必须先是合法 UTF-8，才谈得上"能不能当文件名"。
// 顺序很重要：不先挡这一条，后面构造路径时会失败 ——
// 那个失败要么被忽略（静默回退），要么变成一个用户看不懂的错误。
[[nodiscard]] bool is_valid_utf8(std::string_view text) {
    std::wstring wide;
    Failure ignored;
    return utf8_to_wide(text, wide, ignored);
}

[[nodiscard]] bool common_name_check(std::string_view name, std::size_t max_length,
                                     std::string_view what, Failure& failure) {
    if (name.empty()) {
        failure.set(fail_code::invalid_argument, std::string(what) + "不能为空");
        return false;
    }
    if (name.size() > max_length) {
        failure.set(fail_code::invalid_argument,
                    std::string(what) + "过长（" + std::to_string(name.size()) + " 字节，上限 " +
                        std::to_string(max_length) + "）");
        return false;
    }
    if (!is_valid_utf8(name)) {
        failure.set(fail_code::invalid_argument,
                    std::string(what) + "不是合法的 UTF-8 字节序列（本项目内部一律 UTF-8）");
        return false;
    }

    bool has_visible = false;
    bool starts_with_space = false;
    std::uint32_t last_code_point = 0;
    std::size_t character_index = 0;

    std::size_t offset = 0;
    while (offset < name.size()) {
        const DecodedCodePoint code_point = decode_code_point(name, offset);
        ++character_index;

        if (is_control_code_point(code_point.value)) {
            failure.set(fail_code::invalid_argument,
                        std::string(what) + "含控制字符（第 " + std::to_string(character_index) +
                            " 个字符）");
            return false;
        }
        // 非法字符只能是单字节 ASCII，多字节码点不参与这个判断
        if (code_point.length == 1 &&
            kIllegalCharacters.find(static_cast<char>(code_point.value)) != std::string_view::npos) {
            failure.set(fail_code::invalid_argument,
                        std::string(what) + "含 Windows 不允许的字符 '" +
                            static_cast<char>(code_point.value) + "'（不允许的有 < > : \" / \\ | ? *）");
            return false;
        }

        if (character_index == 1) {
            starts_with_space = is_space_code_point(code_point.value);
        }
        if (!is_space_code_point(code_point.value)) {
            has_visible = true;
        }
        last_code_point = code_point.value;
        offset += code_point.length;
    }

    if (!has_visible) {
        failure.set(fail_code::invalid_argument, std::string(what) + "全是空白字符");
        return false;
    }
    if (starts_with_space) {
        failure.set(fail_code::invalid_argument,
                    std::string(what) + "不能以空白字符开头（全角空格也算）");
        return false;
    }
    if (is_space_code_point(last_code_point) || last_code_point == '.') {
        failure.set(fail_code::invalid_argument,
                    std::string(what) + "不能以空白或点结尾（Windows 存盘时会把它们吃掉，"
                                        "名字就和界面上显示的不一致了）");
        return false;
    }

    // Windows 认的是第一个点之前的那一段：CON.txt 和 CON 一样是设备。
    // 所以要拿主干去比，而不是拿整个名字。
    const std::string upper = uppercase_ascii(name);
    const std::size_t dot = upper.find('.');
    const std::string_view device_part =
        dot == std::string::npos ? std::string_view(upper) : std::string_view(upper).substr(0, dot);
    for (const auto& reserved : kReservedNames) {
        if (device_part == reserved) {
            failure.set(fail_code::invalid_argument,
                        std::string(what) + "是 Windows 保留设备名（" + std::string(reserved) +
                            "，带扩展名也算）");
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string hresult_hex(DWORD error) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(error));
    return buffer;
}

// 把路径规范化后比较，避免 "a/../b" 这类绕过
[[nodiscard]] std::filesystem::path normalized(const std::filesystem::path& path) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical;
}

[[nodiscard]] bool path_is_symlink(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec));
}

// 从末尾切掉若干码点。切的是"最后一个完整码点"，绝不切进多字节序列中间。
void trim_back(std::string& text, bool also_dots) {
    while (!text.empty()) {
        std::size_t start = text.size() - 1;
        while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
            --start;
        }
        const DecodedCodePoint code_point = decode_code_point(text, start);
        if (code_point.length != text.size() - start) {
            break;   // 尾部本身就是残缺的，别再往下切了
        }
        if (!is_space_code_point(code_point.value) &&
            !(also_dots && code_point.value == '.')) {
            break;
        }
        text.resize(start);
    }
}

// 用 SHFileOperationW 送进回收站。
// 这条路是同步的，且需要双 null 结尾的路径列表。
[[nodiscard]] bool move_to_recycle_bin(const std::filesystem::path& path, Failure& failure) {
    std::wstring from = path.wstring();
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;

    const int result = ::SHFileOperationW(&operation);
    if (result != 0) {
        failure.set(fail_code::platform_api_failed,
                    "移入回收站失败（SHFileOperation 返回 " + std::to_string(result) + "）: " +
                        path_to_utf8(path));
        return false;
    }
    if (operation.fAnyOperationsAborted) {
        failure.set(fail_code::platform_api_failed, "移入回收站被中止: " + path_to_utf8(path));
        return false;
    }
    return true;
}

[[nodiscard]] bool is_empty_directory(const std::filesystem::path& path, bool& empty,
                                      Failure& failure) {
    std::error_code ec;
    std::filesystem::directory_iterator it(path, ec);
    if (ec) {
        failure.set(fail_code::file_open_failed,
                    "无法读取目录 " + path_to_utf8(path) + ": " + ec.message());
        return false;
    }
    empty = true;
    for (const auto& entry : it) {
        const std::string name = path_to_utf8(entry.path().filename());
        if (name.empty() || name.front() == '.') {
            continue;   // 隐藏项不算内容（.DS_Store 之类不该阻止删空目录）
        }
        empty = false;
        break;
    }
    return true;
}

}  // namespace

const char* delete_mode_name(DeleteMode mode) noexcept {
    return mode == DeleteMode::recycle_bin ? "recycle_bin" : "permanent";
}

std::filesystem::path track_path_for(const std::filesystem::path& directory, std::string_view stem,
                                     std::string_view extension) {
    const std::filesystem::path name = utf8_path(stem);
    if (name.empty()) {
        return {};
    }
    std::filesystem::path path = directory / name;
    if (extension.empty()) {
        return path;
    }
    std::wstring wide_extension;
    Failure ignored;
    if (!utf8_to_wide(extension, wide_extension, ignored)) {
        return {};
    }
    path += L".";   // path 的 += 是拼字符串（不做分隔符处理），正是这里要的
    path += wide_extension;
    return path;
}

bool validate_file_stem(std::string_view stem, Failure& failure) {
    return common_name_check(stem, maximum_file_stem_length, "文件名", failure);
}

bool validate_category_name(std::string_view name, Failure& failure) {
    if (name == "." || name == "..") {
        failure.set(fail_code::invalid_argument, "分类名不能是 \".\" 或 \"..\"");
        return false;
    }
    return common_name_check(name, maximum_category_name_length, "分类名", failure);
}

std::string sanitize_to_valid_name(std::string_view raw, std::size_t max_length) {
    std::string out;
    out.reserve(std::min(raw.size(), max_length));

    std::size_t offset = 0;
    while (offset < raw.size()) {
        const DecodedCodePoint code_point = decode_code_point(raw, offset);
        offset += code_point.length;

        if (is_control_code_point(code_point.value)) {
            continue;   // 控制字符直接丢掉
        }
        if (code_point.length == 1 &&
            kIllegalCharacters.find(static_cast<char>(code_point.value)) != std::string_view::npos) {
            out.push_back('_');   // 换成下划线，保留长度感
            continue;
        }
        out.append(raw, offset - code_point.length, code_point.length);
    }

    // 去掉首尾空白（按码点，全角空格也算）与结尾的点
    {
        std::size_t leading = 0;
        while (leading < out.size()) {
            const DecodedCodePoint code_point = decode_code_point(out, leading);
            if (!is_space_code_point(code_point.value)) {
                break;
            }
            leading += code_point.length;
        }
        out.erase(0, leading);
    }
    trim_back(out, true);

    // 截断到上限，但不能切在多字节序列中间
    if (out.size() > max_length) {
        std::size_t cut = max_length;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;   // 回退到字符边界
        }
        out.resize(cut);
        trim_back(out, true);
    }

    if (out.empty()) {
        out = "未命名";
    }

    // 净化之后仍可能是保留名，补一个后缀（同样只看第一个点之前那段）
    const std::string upper = uppercase_ascii(out);
    const std::size_t dot = upper.find('.');
    const std::string_view device_part =
        dot == std::string::npos ? std::string_view(upper) : std::string_view(upper).substr(0, dot);
    for (const auto& reserved : kReservedNames) {
        if (device_part == reserved) {
            out += "_";
            break;
        }
    }
    return out;
}

bool path_is_inside(const std::filesystem::path& parent, const std::filesystem::path& child) {
    const std::filesystem::path base = normalized(parent);
    const std::filesystem::path target = normalized(child);

    // 逐组件比较，避免 "C:\abc" 与 "C:\abcd" 被前缀匹配误判为包含关系
    auto base_it = base.begin();
    auto target_it = target.begin();
    for (; base_it != base.end(); ++base_it, ++target_it) {
        if (target_it == target.end()) {
            return false;   // 目标比父目录还短
        }
        if (*base_it != *target_it) {
            return false;
        }
    }
    return target_it != target.end();   // 必须严格在内部
}

std::filesystem::path unique_path_in(const std::filesystem::path& directory, std::string_view stem,
                                     std::string_view extension) {
    std::error_code ec;

    std::filesystem::path candidate = track_path_for(directory, stem, extension);
    if (candidate.empty()) {
        return {};
    }
    if (!std::filesystem::exists(candidate, ec)) {
        return candidate;
    }

    for (int index = 2; index < 10000; ++index) {
        candidate = track_path_for(directory, std::string(stem) + "(" + std::to_string(index) + ")",
                                   extension);
        if (candidate.empty()) {
            return {};
        }
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

bool rename_track(const std::filesystem::path& track_path, std::string_view new_stem,
                  LibraryOpResult& result, Failure& failure) {
    result = LibraryOpResult{};

    if (!validate_file_stem(new_stem, failure)) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(track_path, ec)) {
        failure.set(fail_code::file_not_found, "要重命名的文件不存在: " + path_to_utf8(track_path));
        return false;
    }
    if (path_is_symlink(track_path)) {
        failure.set(fail_code::invalid_argument, "拒绝重命名符号链接: " + path_to_utf8(track_path));
        return false;
    }

    const std::string extension = path_to_utf8(track_path.extension());
    const std::string_view ext_without_dot =
        extension.size() > 1 ? std::string_view(extension).substr(1) : std::string_view{};
    const std::filesystem::path target =
        unique_path_in(track_path.parent_path(), new_stem, ext_without_dot);
    if (target.empty()) {
        failure.set(fail_code::file_open_failed, "在同一目录里找不到可用的新名字");
        return false;
    }

    const std::filesystem::path wanted =
        track_path_for(track_path.parent_path(), new_stem, ext_without_dot);
    result.renamed_to_avoid_collision = wanted.empty() || target != wanted;

    std::filesystem::rename(track_path, target, ec);
    if (ec) {
        failure.set(fail_code::file_open_failed,
                    "重命名失败 " + path_to_utf8(track_path) + " → " + path_to_utf8(target) + ": " +
                        ec.message());
        return false;
    }
    result.resulting_path = target;
    return true;
}

bool delete_track(const std::filesystem::path& track_path, DeleteMode mode, LibraryOpResult& result,
                  Failure& failure) {
    result = LibraryOpResult{};
    result.delete_mode = mode;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(track_path, ec)) {
        failure.set(fail_code::file_not_found, "要删除的文件不存在: " + path_to_utf8(track_path));
        return false;
    }
    if (path_is_symlink(track_path)) {
        failure.set(fail_code::invalid_argument, "拒绝删除符号链接: " + path_to_utf8(track_path));
        return false;
    }

    if (mode == DeleteMode::recycle_bin) {
        if (!move_to_recycle_bin(track_path, failure)) {
            return false;
        }
    } else {
        if (!std::filesystem::remove(track_path, ec) || ec) {
            failure.set(fail_code::file_open_failed,
                        "删除失败 " + path_to_utf8(track_path) + ": " + ec.message());
            return false;
        }
    }
    return true;
}

bool move_track(const std::filesystem::path& track_path,
                const std::filesystem::path& target_category_dir, LibraryOpResult& result,
                Failure& failure) {
    result = LibraryOpResult{};

    std::error_code ec;
    if (!std::filesystem::is_regular_file(track_path, ec)) {
        failure.set(fail_code::file_not_found, "要移动的文件不存在: " + path_to_utf8(track_path));
        return false;
    }
    if (path_is_symlink(track_path)) {
        failure.set(fail_code::invalid_argument, "拒绝移动符号链接: " + path_to_utf8(track_path));
        return false;
    }
    if (!std::filesystem::is_directory(target_category_dir, ec)) {
        failure.set(fail_code::file_not_found,
                    "目标分类目录不存在: " + path_to_utf8(target_category_dir));
        return false;
    }

    const std::string stem = path_to_utf8(track_path.stem());
    const std::string extension = path_to_utf8(track_path.extension());
    const std::string_view ext_without_dot =
        extension.size() > 1 ? std::string_view(extension).substr(1) : std::string_view{};

    const std::filesystem::path target = unique_path_in(target_category_dir, stem, ext_without_dot);
    if (target.empty()) {
        failure.set(fail_code::file_open_failed, "目标目录里找不到可用的文件名");
        return false;
    }
    const std::filesystem::path wanted = track_path_for(target_category_dir, stem, ext_without_dot);
    result.renamed_to_avoid_collision = wanted.empty() || target != wanted;

    if (normalized(track_path.parent_path()) == normalized(target_category_dir)) {
        failure.set(fail_code::invalid_argument, "源与目标在同一个目录，不需要移动");
        return false;
    }

    // 先试同盘重命名（原子且不复制数据）；跨盘失败再退回复制 + 校验 + 删除。
    std::filesystem::rename(track_path, target, ec);
    if (!ec) {
        result.resulting_path = target;
        return true;
    }

    std::filesystem::copy_file(track_path, target, std::filesystem::copy_options::none, ec);
    if (ec) {
        failure.set(fail_code::file_open_failed,
                    "移动失败（跨盘复制也失败）" + path_to_utf8(track_path) + " → " +
                        path_to_utf8(target) + ": " + ec.message());
        return false;
    }

    // 复制完校验大小一致再删源 —— 直接删会在复制不完整时丢文件
    const auto source_size = std::filesystem::file_size(track_path, ec);
    const auto target_size = ec ? 0 : std::filesystem::file_size(target, ec);
    if (ec || source_size != target_size) {
        std::error_code cleanup_ec;
        std::filesystem::remove(target, cleanup_ec);
        failure.set(fail_code::file_read_failed,
                    "移动失败：复制后大小不一致（源 " + std::to_string(source_size) + " 目标 " +
                        std::to_string(target_size) + "），已删除半成品");
        return false;
    }

    std::filesystem::remove(track_path, ec);
    if (ec) {
        failure.set(fail_code::file_open_failed,
                    "移动已完成但删除源文件失败: " + path_to_utf8(track_path) + ": " + ec.message());
        return false;
    }
    result.resulting_path = target;
    return true;
}

bool create_category(const std::filesystem::path& section_base, std::string_view name,
                     Failure& failure) {
    if (!validate_category_name(name, failure)) {
        return false;
    }
    // 这一条是原版那个 bug 的正解：分类名里出现路径分隔符 / ".." 一律在校验阶段拒掉，
    // 而不是让 make_dir_recursive 建出一层嵌套目录、事后谁也不知道文件去哪了。
    const std::filesystem::path target = track_path_for(section_base, name, {});
    if (target.empty()) {
        failure.set(fail_code::invalid_argument,
                    "分类名无法构成合法路径（不是合法 UTF-8）: " + std::string(name));
        return false;
    }
    if (!path_is_inside(section_base, target)) {
        failure.set(fail_code::invalid_argument, "分类名会让目录跑到栏目之外: " + std::string(name));
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(section_base, ec)) {
        failure.set(fail_code::file_not_found, "栏目目录不存在: " + path_to_utf8(section_base));
        return false;
    }
    if (std::filesystem::exists(target, ec)) {
        failure.set(fail_code::invalid_argument, "同名分类已经存在: " + std::string(name));
        return false;
    }
    if (!std::filesystem::create_directory(target, ec) || ec) {
        failure.set(fail_code::file_open_failed,
                    "创建分类目录失败 " + path_to_utf8(target) + ": " + ec.message());
        return false;
    }
    return true;
}

bool rename_category(const std::filesystem::path& section_base, std::string_view old_name,
                     std::string_view new_name, Failure& failure) {
    if (!validate_category_name(new_name, failure)) {
        return false;
    }
    if (old_name == new_name) {
        failure.set(fail_code::invalid_argument, "新旧分类名相同");
        return false;
    }

    const std::filesystem::path source = track_path_for(section_base, old_name, {});
    const std::filesystem::path target = track_path_for(section_base, new_name, {});
    if (source.empty() || target.empty()) {
        failure.set(fail_code::invalid_argument, "分类名无法构成合法路径（不是合法 UTF-8）");
        return false;
    }
    if (!path_is_inside(section_base, target)) {
        failure.set(fail_code::invalid_argument,
                    "新分类名会让目录跑到栏目之外: " + std::string(new_name));
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(source, ec)) {
        failure.set(fail_code::file_not_found, "分类目录不存在: " + path_to_utf8(source));
        return false;
    }
    if (std::filesystem::exists(target, ec)) {
        failure.set(fail_code::invalid_argument, "同名分类已经存在: " + std::string(new_name));
        return false;
    }

    std::filesystem::rename(source, target, ec);
    if (ec) {
        failure.set(fail_code::file_open_failed,
                    "重命名分类失败 " + path_to_utf8(source) + " → " + path_to_utf8(target) + ": " +
                        ec.message());
        return false;
    }
    return true;
}

bool delete_empty_category(const std::filesystem::path& section_base, std::string_view name,
                           Failure& failure) {
    const std::filesystem::path target = track_path_for(section_base, name, {});
    if (target.empty()) {
        failure.set(fail_code::invalid_argument, "分类名无法构成合法路径（不是合法 UTF-8）");
        return false;
    }
    if (!path_is_inside(section_base, target)) {
        failure.set(fail_code::invalid_argument, "分类名指向栏目之外: " + std::string(name));
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(target, ec)) {
        failure.set(fail_code::file_not_found, "分类目录不存在: " + path_to_utf8(target));
        return false;
    }

    bool empty = false;
    if (!is_empty_directory(target, empty, failure)) {
        return false;
    }
    if (!empty) {
        // 明确拒绝，绝不递归删除 —— 那是在替用户做他没想到的决定
        failure.set(fail_code::invalid_argument,
                    "分类 \"" + std::string(name) + "\" 不是空的，拒绝删除。请先移走或删除其中的文件。");
        return false;
    }

    if (!std::filesystem::remove(target, ec) || ec) {
        failure.set(fail_code::file_open_failed,
                    "删除空分类失败 " + path_to_utf8(target) + ": " + ec.message());
        return false;
    }
    return true;
}

}  // namespace musicboard
