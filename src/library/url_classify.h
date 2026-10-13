#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace musicboard {

// ============================================================================
// URL 判定与输出解析（全是纯函数）
//
// 单独拆出来的理由：URL 导入这一块真正值得测的是**判定与解析**，
// 而不是"能不能连上 YouTube"。拆成纯函数之后，那些逻辑可以完全离线穷举，
// 剩下的进程管理部分用一个假下载器对着跑（见 tests/fake_child.cpp），
// 全程不碰网络。
//
// 原版（url_importer.gd）在这一层没有边界：判定、下载、进度、错误提示
// 全在一个 Node 里，于是"进度条永远不会动"这种问题没有任何一处可以单独钉住。
// ============================================================================

enum class ImportSourceKind : std::uint8_t {
    // 明确不支持（给用户一句人话，而不是让 yt-dlp 抛一堆栈）
    unsupported,
    // 交给 yt-dlp（YouTube / 网易云 / SoundCloud / 其他通用站点）
    yt_dlp,
    // 直接是音频文件链接，自己下就行，不必要求用户装 yt-dlp
    direct_http,
};

struct UrlClassification final {
    ImportSourceKind kind = ImportSourceKind::unsupported;
    // "youtube" / "netease" / "soundcloud" / "bilibili" / "direct_audio" / "generic"
    std::string platform;
    // 界面用的中文名
    std::string display_name;
    // kind == unsupported 时给出原因（直接可显示）
    std::string rejection_reason;
};

// 判定一个 URL 该走哪条路。
[[nodiscard]] UrlClassification classify_url(std::string_view url);

// 形状检查：http(s) 开头、有主机名、没有控制字符。
// 比原版的 "以 http 开头且含一个点" 严格一点 —— 后者会把 "http://." 放过。
[[nodiscard]] bool url_is_well_formed(std::string_view url);
[[nodiscard]] std::string normalize_import_url(std::string_view url);

// 从 URL 推导一个期望的文件名主干（已净化，可直接当文件名）。
// 推不出来时返回空串，调用方应改用兜底名。
[[nodiscard]] std::string stem_from_url(std::string_view url);

// 解析 yt-dlp 的一行输出里的下载百分比。
// 命中时 fraction ∈ [0,1]；不是进度行、或数值不合理时返回 false。
//
// 解析的是人类可读的那一行而不是 --progress-template：模板参数在旧版
// yt-dlp 上是未知参数，会让整个下载直接失败。--newline --progress 到处都有。
[[nodiscard]] bool parse_yt_dlp_progress_line(std::string_view line, double& fraction);

// 从累积输出里挑出最值得显示的一行错误原因。挑不出来就返回空串。
[[nodiscard]] std::string extract_yt_dlp_error(std::string_view output);

// 针对常见失败给一句可执行的建议。没有对应建议时返回空串。
[[nodiscard]] std::string suggest_yt_dlp_fix(std::string_view output);

[[nodiscard]] std::string summarize_yt_dlp_failure(std::string_view output);
[[nodiscard]] bool parse_yt_dlp_duration_line(std::string_view line, double& seconds);
[[nodiscard]] bool downloaded_audio_is_incomplete(double source_seconds, double audio_seconds) noexcept;

// URL 里的百分号解码（%E4%B8%AD → 中）。非法序列原样保留。
[[nodiscard]] std::string percent_decode(std::string_view text);

}  // namespace musicboard
