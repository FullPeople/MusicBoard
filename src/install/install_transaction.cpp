#include "install/install_transaction.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#include "core/path_utf8.h"
#include "core/sha256.h"

namespace musicboard {
namespace {

constexpr std::string_view kManifestName = "SHA256SUMS.txt";
constexpr std::string_view kVersionName = "VERSION.txt";
constexpr std::string_view kJournalName = "install-journal.txt";
constexpr std::string_view kCurrentDir = "current";
constexpr std::string_view kPreviousDir = "previous";
constexpr wchar_t kCandidatePrefix[] = L"candidate-";

[[nodiscard]] std::string trim(std::string_view text) {
    const auto not_space = [](unsigned char ch) { return std::isspace(ch) == 0; };
    const auto begin = std::find_if(text.begin(), text.end(), not_space);
    const auto end = std::find_if(text.rbegin(), text.rend(), not_space).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

[[nodiscard]] std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

// 一个路径组件是否能把我们带出包根
[[nodiscard]] bool has_escape_component(const std::filesystem::path& relative) {
    for (const auto& part : relative) {
        const std::string name = path_to_utf8(part);
        if (name == "..") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_reparse_point(const std::filesystem::path& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// 把清单里的一行解析成 (sha256, 相对路径)。格式："<64 hex>  <path>"。
[[nodiscard]] bool parse_manifest_line(std::string_view line, InstallPackageEntry& entry,
                                       std::string& problem) {
    // 摘要与路径之间是两个空格（sha256sum 的格式），但接受任意空白
    const std::size_t split = line.find_first_of(" \t");
    if (split == std::string_view::npos) {
        problem = "清单行里没有分隔空白: " + std::string(line);
        return false;
    }
    const std::string digest = lowercase(trim(line.substr(0, split)));
    const std::string path = trim(line.substr(split));
    if (!is_hex_digest(digest)) {
        problem = "清单里的摘要不是 64 位十六进制: " + digest;
        return false;
    }
    if (path.empty()) {
        problem = "清单行里没有路径";
        return false;
    }
    entry.sha256 = digest;
    entry.relative_path = path;
    return true;
}

// ---------------------------------------------------------------------------
// VERSION.txt：与设置文件同一套 key=value 写法（项目里已经有这个约定）
// ---------------------------------------------------------------------------
using KeyValue = std::map<std::string, std::string>;

[[nodiscard]] bool parse_version_file(const std::filesystem::path& path, KeyValue& values,
                                      Failure& failure) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        failure.set(fail_code::file_not_found,
                    "安装包里没有 " + std::string(kVersionName) + "（版本契约）: " +
                        path_to_utf8(path));
        return false;
    }
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string text = trim(line);
        if (text.empty() || text.front() == '#') {
            continue;
        }
        const std::size_t equals = text.find('=');
        if (equals == std::string::npos) {
            failure.set(fail_code::invalid_argument,
                        std::string(kVersionName) + " 里有不合法的行（缺 '='）: " + text);
            return false;
        }
        values[trim(text.substr(0, equals))] = trim(text.substr(equals + 1));
    }
    return true;
}

// ---------------------------------------------------------------------------
// 安装日志
//
// 只记当前处于哪个阶段，外加版本号。够用来判断"上次跑到哪儿了"，
// 而不用把整个包的状态都序列化一遍。
// ---------------------------------------------------------------------------
struct Journal final {
    InstallPhase phase = InstallPhase::none;
    std::string version;
};

[[nodiscard]] std::filesystem::path journal_path(const std::filesystem::path& install_root) {
    return install_root / std::filesystem::path(std::wstring(kJournalName.begin(),
                                                             kJournalName.end()));
}

[[nodiscard]] bool write_journal(const std::filesystem::path& install_root,
                                 const Journal& journal, Failure& failure) {
    const std::filesystem::path path = journal_path(install_root);
    const std::filesystem::path temporary = path.wstring() + L".tmp";

    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        failure.set(fail_code::file_open_failed, "无法写安装日志: " + path_to_utf8(temporary));
        return false;
    }
    stream << "phase=" << static_cast<std::uint32_t>(journal.phase) << "\n";
    stream << "version=" << journal.version << "\n";
    stream << "complete=1\n";
    stream.close();
    if (!stream) {
        failure.set(fail_code::file_read_failed, "写安装日志失败: " + path_to_utf8(temporary));
        return false;
    }

    // 原子替换：中途断电最多丢掉这一次写入，不会留下半个日志
    if (::MoveFileExW(temporary.c_str(), path.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        failure.set(fail_code::platform_api_failed,
                    "替换安装日志失败（Win32 错误 " + std::to_string(::GetLastError()) + "）");
        return false;
    }
    return true;
}

[[nodiscard]] bool read_journal(const std::filesystem::path& install_root, Journal& journal) {
    journal = Journal{};
    const std::filesystem::path path = journal_path(install_root);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    bool saw_complete = false;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.starts_with("phase=")) {
            try {
                journal.phase = static_cast<InstallPhase>(std::stoul(line.substr(6)));
            } catch (...) {
                return false;
            }
        } else if (line.starts_with("version=")) {
            journal.version = line.substr(8);
        } else if (line == "complete=1") {
            saw_complete = true;
        }
    }
    // 没有 complete=1 说明日志是截断的 —— 当作"没有日志"，
    // 让上层的恢复逻辑走最保守的那条路
    return saw_complete;
}

// ---------------------------------------------------------------------------
// 目录搬运
// ---------------------------------------------------------------------------

[[nodiscard]] bool copy_tree(const std::filesystem::path& source,
                             const std::filesystem::path& target, Failure& failure,
                             const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                             std::uint64_t total_bytes) {
    std::error_code ec;
    std::filesystem::create_directories(target, ec);
    if (ec) {
        failure.set(fail_code::file_open_failed,
                    "创建目录失败 " + path_to_utf8(target) + ": " + ec.message());
        return false;
    }
    std::uint64_t copied = 0;
    if (progress) progress(0, total_bytes);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source, ec)) {
        const std::filesystem::path relative = std::filesystem::relative(entry.path(), source, ec);
        if (ec) {
            failure.set(fail_code::file_open_failed, "算相对路径失败: " + ec.message());
            return false;
        }
        const std::filesystem::path destination = target / relative;
        if (entry.is_directory(ec)) {
            std::filesystem::create_directories(destination, ec);
            if (ec) {
                failure.set(fail_code::file_open_failed,
                            "创建目录失败 " + path_to_utf8(destination) + ": " + ec.message());
                return false;
            }
            continue;
        }
        std::filesystem::copy_file(entry.path(), destination,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            failure.set(fail_code::file_open_failed,
                        "复制失败 " + path_to_utf8(entry.path()) + " → " +
                            path_to_utf8(destination) + ": " + ec.message());
            return false;
        }
        const auto bytes = entry.file_size(ec);
        if (ec) {
            failure.set(fail_code::file_read_failed, "读取复制文件大小失败: " + ec.message());
            return false;
        }
        copied += bytes;
        if (progress) progress(copied, total_bytes);
    }
    return true;
}

[[nodiscard]] bool remove_tree(const std::filesystem::path& path, std::uintmax_t& removed,
                               Failure& failure) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return true;
    }
    removed += std::filesystem::remove_all(path, ec);
    if (ec) {
        failure.set(fail_code::file_open_failed,
                    "删除失败 " + path_to_utf8(path) + ": " + ec.message());
        return false;
    }
    return true;
}

[[nodiscard]] std::vector<std::filesystem::path> candidate_directories(
    const std::filesystem::path& install_root) {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    if (!std::filesystem::is_directory(install_root, ec)) {
        return found;
    }
    for (const auto& entry : std::filesystem::directory_iterator(install_root, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        if (path_to_utf8(entry.path().filename()).starts_with("candidate-")) {
            found.push_back(entry.path());
        }
    }
    return found;
}

}  // namespace

const char* install_phase_name(InstallPhase phase) noexcept {
    switch (phase) {
        case InstallPhase::none:
            return "none";
        case InstallPhase::verified:
            return "verified";
        case InstallPhase::staged:
            return "staged";
        case InstallPhase::previous_moved:
            return "previous_moved";
        case InstallPhase::candidate_published:
            return "candidate_published";
        case InstallPhase::committed:
            return "committed";
    }
    return "unknown";
}

std::filesystem::path default_install_root() {
    wchar_t buffer[32768]{};
    const DWORD written = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                                   static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(buffer) / L"Programs" / L"MusicBoard";
}

// ---------------------------------------------------------------------------
// 校验
// ---------------------------------------------------------------------------

bool verify_install_package(const VerifyOptions& options, VerifiedPackage& package,
                            Failure& failure) {
    package = VerifiedPackage{};
    std::error_code ec;

    if (options.package_root.empty()) {
        failure.set(fail_code::invalid_argument, "没有指定安装包目录");
        return false;
    }
    if (!std::filesystem::is_directory(options.package_root, ec)) {
        failure.set(fail_code::file_not_found,
                    "安装包目录不存在: " + path_to_utf8(options.package_root));
        return false;
    }
    // 包根本身是符号链接的话，"校验过的"与"装下去的"可能是两个地方
    if (is_reparse_point(options.package_root)) {
        failure.set(fail_code::invalid_argument,
                    "安装包目录是重解析点（符号链接或联结点），拒绝: " +
                        path_to_utf8(options.package_root));
        return false;
    }
    package.root = options.package_root;

    // ---- VERSION.txt ----
    const std::filesystem::path version_path =
        options.package_root / std::filesystem::path(std::wstring(kVersionName.begin(),
                                                                 kVersionName.end()));
    KeyValue version_values;
    if (!parse_version_file(version_path, version_values, failure)) {
        return false;
    }
    const auto require = [&version_values, &failure](std::string_view key) -> std::string {
        const auto found = version_values.find(std::string(key));
        if (found == version_values.end() || found->second.empty()) {
            failure.set(fail_code::invalid_argument,
                        std::string(kVersionName) + " 缺少字段 " + std::string(key));
            return {};
        }
        return found->second;
    };
    package.product = require("product");
    package.version = require("version");
    package.package_version = require("package_version");
    package.architecture = require("architecture");
    package.executable_name = require("executable");
    if (!failure.empty()) {
        return false;
    }
    if (package.product != "MusicBoard") {
        failure.set(fail_code::invalid_argument,
                    "安装包的产品名不对（期望 MusicBoard，实际 " + package.product + "）");
        return false;
    }
    if (package.architecture != options.expected_architecture) {
        failure.set(fail_code::invalid_argument,
                    "安装包的架构不对（期望 " + options.expected_architecture + "，实际 " +
                        package.architecture + "）");
        return false;
    }

    // ---- SHA256SUMS.txt ----
    const std::filesystem::path manifest_path =
        options.package_root / std::filesystem::path(std::wstring(kManifestName.begin(),
                                                                 kManifestName.end()));
    if (!sha256_file_hex(manifest_path, package.manifest_sha256, failure)) {
        return false;
    }
    if (!options.expected_manifest_sha256.empty() &&
        (!is_hex_digest(options.expected_manifest_sha256) ||
         !digest_equals(package.manifest_sha256, options.expected_manifest_sha256))) {
        failure.set(fail_code::file_read_failed, "安装文件清单与安装向导验证的清单不一致");
        return false;
    }
    std::ifstream manifest(manifest_path, std::ios::binary);
    if (!manifest) {
        failure.set(fail_code::file_not_found,
                    "安装包里没有 " + std::string(kManifestName) + ": " +
                        path_to_utf8(manifest_path));
        return false;
    }

    std::vector<InstallPackageEntry> entries;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(manifest, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string text = trim(line);
        if (text.empty() || text.front() == '#') {
            continue;
        }
        InstallPackageEntry entry;
        std::string problem;
        if (!parse_manifest_line(text, entry, problem)) {
            failure.set(fail_code::invalid_argument,
                        std::string(kManifestName) + " 第 " + std::to_string(line_number) +
                            " 行: " + problem);
            return false;
        }
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) {
        failure.set(fail_code::invalid_argument,
                    std::string(kManifestName) + " 里一条记录都没有");
        return false;
    }

    // ---- 逐条校验 ----
    std::map<std::string, bool> listed;
    std::uint64_t verified_files = 0;
    if (options.progress) options.progress(0, entries.size());
    for (const InstallPackageEntry& entry : entries) {
        std::string relative_text = entry.relative_path;
        // 统一分隔符，免得清单里写 "a\b" 而实际是 "a/b"（或者反过来）
        std::replace(relative_text.begin(), relative_text.end(), '\\', '/');

        // 路径必须是相对的：绝对路径与盘符直接拒
        std::filesystem::path relative;
        Failure convert_failure;
        if (!path_from_utf8(relative_text, relative, convert_failure)) {
            failure.set(fail_code::invalid_argument,
                        "清单里的路径不是合法 UTF-8: " + relative_text);
            return false;
        }
        if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
            failure.set(fail_code::invalid_argument,
                        "清单里出现绝对路径（拒绝）: " + relative_text);
            return false;
        }
        if (has_escape_component(relative)) {
            failure.set(fail_code::invalid_argument,
                        "清单里的路径含 \"..\"（路径逃逸，拒绝）: " + relative_text);
            return false;
        }

        const std::filesystem::path full = options.package_root / relative;
        if (!std::filesystem::is_regular_file(full, ec)) {
            failure.set(fail_code::file_not_found,
                        "清单里的文件不存在: " + relative_text);
            return false;
        }
        if (is_reparse_point(full)) {
            failure.set(fail_code::invalid_argument,
                        "包内出现重解析点（拒绝）: " + relative_text);
            return false;
        }

        std::string actual;
        Failure hash_failure;
        if (!sha256_file_hex(full, actual, hash_failure)) {
            failure = hash_failure;
            return false;
        }
        if (!digest_equals(actual, entry.sha256)) {
            // 这一条是完整性边界的核心：文件被动过就装不进去
            failure.set(fail_code::file_read_failed,
                        "文件校验失败（内容与清单不符）: " + relative_text + "，清单 " +
                            entry.sha256.substr(0, 16) + "…，实际 " + actual.substr(0, 16) + "…");
            return false;
        }
        listed[lowercase(relative_text)] = true;
        package.total_bytes += std::filesystem::file_size(full, ec);
        if (ec) {
            failure.set(fail_code::file_read_failed, "读取校验文件大小失败: " + relative_text);
            return false;
        }
        ++verified_files;
        if (options.progress) options.progress(verified_files, entries.size());
    }

    // ---- 包内不许有清单之外的文件 ----
    //
    // 少了这一条，攻击者可以在一个"校验通过"的包里加一个 dll —— 每个被列出的
    // 文件都对得上，但多出来的那个会被加载。
    for (const auto& entry : std::filesystem::recursive_directory_iterator(options.package_root,
                                                                          ec)) {
        const std::filesystem::path relative =
            std::filesystem::relative(entry.path(), options.package_root, ec);
        if (ec) {
            failure.set(fail_code::file_open_failed, "遍历包目录失败: " + ec.message());
            return false;
        }
        std::string relative_text = path_to_utf8(relative);
        std::replace(relative_text.begin(), relative_text.end(), '\\', '/');
        if (entry.is_directory(ec)) {
            continue;
        }
        if (is_reparse_point(entry.path())) {
            failure.set(fail_code::invalid_argument,
                        "包内出现重解析点（拒绝）: " + relative_text);
            return false;
        }
        // **清单自己不在清单里**：它没法列出自己的哈希（列了就对不上了）。
        // 所以它是这个包的信任根 —— 包内没有任何东西能验证它自己，
        // 那要靠包外的渠道（发布时公布的哈希、以及将来的代码签名）。
        // 这里的规则是"除清单之外，包内不许有清单之外的文件"。
        if (lowercase(relative_text) == lowercase(std::string(kManifestName))) {
            continue;
        }
        if (listed.find(lowercase(relative_text)) == listed.end()) {
            failure.set(fail_code::invalid_argument,
                        "安装包里有一个没写进 " + std::string(kManifestName) +
                            " 的文件（拒绝）: " + relative_text);
            return false;
        }
    }

    // 主程序必须在清单里，否则装完没有可执行的东西
    const std::string executable = lowercase(package.executable_name);
    if (listed.find(executable) == listed.end()) {
        failure.set(fail_code::invalid_argument,
                    "主程序 " + package.executable_name + " 不在 " + std::string(kManifestName) +
                        " 里");
        return false;
    }

    package.entries = std::move(entries);
    return true;
}

// ---------------------------------------------------------------------------
// 安装
// ---------------------------------------------------------------------------

bool install_product(const InstallOptions& options, InstallResult& result, Failure& failure) {
    result = InstallResult{};

    if (options.install_root.empty()) {
        failure.set(fail_code::invalid_argument, "没有指定安装位置");
        return false;
    }
    if (options.package.entries.empty() || options.package.root.empty()) {
        failure.set(fail_code::invalid_argument,
                    "安装包没有经过校验（verify_install_package 必须先跑）");
        return false;
    }

    std::error_code ec;
    if (!options.install_root.is_absolute() || options.install_root.lexically_normal() == options.install_root.root_path()) {
        failure.set(fail_code::invalid_argument, "安装位置必须是单独的完整目录，不能使用磁盘根目录");
        return false;
    }
    std::filesystem::path checked;
    for (const auto& component : options.install_root) {
        checked /= component;
        if (is_reparse_point(checked)) {
            failure.set(fail_code::invalid_argument, "安装路径包含重解析点，已拒绝");
            return false;
        }
    }
    if (std::filesystem::exists(options.install_root, ec) &&
        !std::filesystem::is_empty(options.install_root, ec) &&
        !std::filesystem::is_regular_file(journal_path(options.install_root), ec)) {
        failure.set(fail_code::invalid_argument, "所选目录已有其他文件，请为音乐板选择单独的目录");
        return false;
    }
    if (ec) {
        failure.set(fail_code::file_open_failed, "无法检查安装目录: " + ec.message());
        return false;
    }
    std::filesystem::create_directories(options.install_root, ec);
    if (ec) {
        failure.set(fail_code::file_open_failed,
                    "创建安装目录失败 " + path_to_utf8(options.install_root) + ": " + ec.message());
        return false;
    }

    const std::filesystem::path current = options.install_root /
                                          std::filesystem::path(std::wstring(kCurrentDir.begin(),
                                                                            kCurrentDir.end()));
    const std::filesystem::path previous =
        options.install_root / std::filesystem::path(std::wstring(kPreviousDir.begin(),
                                                                 kPreviousDir.end()));

    // ---- 恢复上次中断的事务 ----
    Journal journal;
    if (read_journal(options.install_root, journal)) {
        result.recovered_interrupted_transaction = true;
        switch (journal.phase) {
            case InstallPhase::staged:
                // 候选写满了但还没换上去 → 旧的还在，把候选当垃圾清掉
                for (const auto& candidate : candidate_directories(options.install_root)) {
                    std::uintmax_t removed = 0;
                    if (!remove_tree(candidate, removed, failure)) {
                        return false;
                    }
                }
                break;
            case InstallPhase::previous_moved:
                // 旧版本已经让位、候选还没就位 → 把旧的放回去
                if (std::filesystem::exists(previous, ec) && !std::filesystem::exists(current, ec)) {
                    std::filesystem::rename(previous, current, ec);
                    if (ec) {
                        failure.set(fail_code::file_open_failed,
                                    "恢复上次中断的安装失败（无法把 previous 移回 current）: " +
                                        ec.message());
                        return false;
                    }
                }
                for (const auto& candidate : candidate_directories(options.install_root)) {
                    std::uintmax_t removed = 0;
                    if (!remove_tree(candidate, removed, failure)) {
                        return false;
                    }
                }
                break;
            case InstallPhase::candidate_published:
            case InstallPhase::committed: {
                // 候选已经就位 → 收尾：删掉 previous
                std::uintmax_t removed = 0;
                if (!remove_tree(previous, removed, failure)) {
                    return false;
                }
                break;
            }
            default:
                break;
        }
    }

    // ---- 幂等：已经装的是同一个版本就不重复劳动 ----
    const std::string existing = installed_version(options.install_root);
    std::string existing_manifest;
    Failure manifest_failure;
    const bool same_manifest = sha256_file_hex(current / L"SHA256SUMS.txt", existing_manifest,
                                               manifest_failure) &&
                               digest_equals(existing_manifest, options.package.manifest_sha256);
    if (existing == options.package.version && same_manifest &&
        std::filesystem::is_regular_file(current / std::filesystem::path(
                                                       std::wstring(
                                                           options.package.executable_name.begin(),
                                                           options.package.executable_name.end())),
                                        ec)) {
        result.installed_version = existing;
        result.installed_executable =
            current / std::filesystem::path(std::wstring(options.package.executable_name.begin(),
                                                        options.package.executable_name.end()));
        return true;
    }

    Journal next;
    next.version = options.package.version;
    next.phase = InstallPhase::verified;
    if (!write_journal(options.install_root, next, failure)) {
        return false;
    }

    // ---- 1. 把包抄进候选目录 ----
    //
    // 先抄完再换：这样"换上去"那一步只是两次改名，中间没有半成品窗口。
    const std::filesystem::path candidate =
        options.install_root /
        (std::wstring(kCandidatePrefix) +
         std::wstring(options.package.version.begin(), options.package.version.end()));
    {
        std::uintmax_t removed = 0;
        if (!remove_tree(candidate, removed, failure)) {
            return false;
        }
    }
    const auto manifest_bytes = std::filesystem::file_size(options.package.root / L"SHA256SUMS.txt", ec);
    if (ec) {
        failure.set(fail_code::file_read_failed, "读取安装文件清单大小失败: " + ec.message());
        return false;
    }
    if (!copy_tree(options.package.root, candidate, failure, options.progress,
                   options.package.total_bytes + manifest_bytes)) {
        // 抄失败就把候选删掉，不留下一个不完整的目录 ——
        // 它下一次会被误判成"上次的候选"
        std::uintmax_t removed = 0;
        Failure cleanup;
        static_cast<void>(remove_tree(candidate, removed, cleanup));
        return false;
    }
    next.phase = InstallPhase::staged;
    if (!write_journal(options.install_root, next, failure)) {
        return false;
    }

    // ---- 2. 旧版本让位 ----
    result.upgraded = std::filesystem::exists(current, ec);
    if (result.upgraded) {
        result.previous_version = existing;
        std::uintmax_t removed = 0;
        if (!remove_tree(previous, removed, failure)) {
            return false;
        }
        std::filesystem::rename(current, previous, ec);
        if (ec) {
            failure.set(fail_code::file_open_failed,
                        "把现有版本移到 previous 失败（可能有文件正在被占用，"
                        "请先退出音乐板）: " + ec.message());
            return false;
        }
    }
    next.phase = InstallPhase::previous_moved;
    if (!write_journal(options.install_root, next, failure)) {
        return false;
    }

    // ---- 3. 候选就位 ----
    std::filesystem::rename(candidate, current, ec);
    if (ec) {
        failure.set(fail_code::file_open_failed,
                    "把新版本放到 current 失败: " + ec.message());
        return false;
    }
    next.phase = InstallPhase::candidate_published;
    if (!write_journal(options.install_root, next, failure)) {
        return false;
    }

    // ---- 4. 收尾 ----
    if (result.upgraded) {
        std::uintmax_t removed = 0;
        if (!remove_tree(previous, removed, failure)) {
            // 旧版本删不掉不算安装失败：新版本已经就位了。
            // 但要留痕 —— 不然用户磁盘上会一直躺着一份旧版本而没人知道。
            ::OutputDebugStringA(
                ("旧版本目录删不掉（不影响使用）: " + path_to_utf8(previous) + "\n").c_str());
        }
    }
    next.phase = InstallPhase::committed;
    if (!write_journal(options.install_root, next, failure)) return false;

    result.installed_version = options.package.version;
    result.installed_executable =
        current / std::filesystem::path(std::wstring(options.package.executable_name.begin(),
                                                    options.package.executable_name.end()));
    return true;
}

// ---------------------------------------------------------------------------
// 卸载
// ---------------------------------------------------------------------------

bool uninstall_product(const UninstallOptions& options, UninstallResult& result, Failure& failure) {
    result = UninstallResult{};
    if (options.install_root.empty()) {
        failure.set(fail_code::invalid_argument, "没有指定安装位置");
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(options.install_root, ec)) {
        return true;   // 已经没了就是想要的终态
    }
    if (!options.install_root.is_absolute() || options.install_root.lexically_normal() == options.install_root.root_path() ||
        !std::filesystem::is_regular_file(journal_path(options.install_root), ec) || is_reparse_point(options.install_root)) {
        failure.set(fail_code::invalid_argument, "无法确认这是音乐板的安装目录，未删除任何文件");
        return false;
    }

    result.removed_version = installed_version(options.install_root);

    Journal journal;
    if (read_journal(options.install_root, journal)) {
        result.recovered_interrupted_transaction = true;
    }

    // **只删安装根目录**。安装根之外的东西一概不碰 ——
    // 用户的曲库、设置、日志都在别处，卸载不该动它们。
    std::uintmax_t removed = 0;
    if (!remove_tree(options.install_root, removed, failure)) {
        return false;
    }
    result.removed_bytes = removed;
    return true;
}

std::string installed_version(const std::filesystem::path& install_root) {
    if (install_root.empty()) {
        return {};
    }
    Journal journal;
    if (!read_journal(install_root, journal)) {
        return {};
    }
    if (journal.phase != InstallPhase::committed) {
        // 没走到 committed 的日志不算"装好了"
        return {};
    }
    return journal.version;
}

}  // namespace musicboard
