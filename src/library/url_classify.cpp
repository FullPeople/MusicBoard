#include "library/url_classify.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include "library/library_ops.h"

namespace musicboard {
namespace {

[[nodiscard]] std::string lowercase_ascii(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

[[nodiscard]] std::string trim_ascii(std::string_view text) {
    const auto not_space = [](unsigned char ch) { return std::isspace(ch) == 0; };
    const auto begin = std::find_if(text.begin(), text.end(), not_space);
    const auto end = std::find_if(text.rbegin(), text.rend(), not_space).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

[[nodiscard]] bool contains_any(std::string_view haystack,
                                const std::array<std::string_view, 2>& needles) {
    for (const auto& needle : needles) {
        if (haystack.find(needle) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

// 带点的扩展名（含空串表示没有）
[[nodiscard]] std::string extension_of(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    const std::string_view last =
        slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = last.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {};
    }
    return lowercase_ascii(last.substr(dot));
}

// 把 URL 拆成 (路径部分, 查询/片段)。查询串不参与文件名与扩展名判断。
void split_url(std::string_view url, std::string_view& path, std::string_view& query) {
    const std::size_t cut = url.find_first_of("?#");
    if (cut == std::string_view::npos) {
        path = url;
        query = {};
        return;
    }
    path = url.substr(0, cut);
    query = url.substr(cut);
}

[[nodiscard]] std::string_view last_segment(std::string_view path) {
    while (!path.empty() && path.back() == '/') {
        path.remove_suffix(1);
    }
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

struct PlatformEntry final {
    std::string_view id;
    std::string_view display;
    std::array<std::string_view, 2> domains;
    bool supported;
};

// 顺序有意义：先匹配到的赢。把已知"不支持"的放前面，
// 免得 bilibili 链接被当成通用链接丢给 yt-dlp 再失败。
constexpr std::array<PlatformEntry, 5> kPlatforms = {{
    {"bilibili", "哔哩哔哩", {"bilibili.com", "b23.tv"}, false},
    {"youtube", "YouTube", {"youtube.com", "youtu.be"}, true},
    {"netease", "网易云音乐", {"music.163.com", "163cn.tv"}, true},
    {"soundcloud", "SoundCloud", {"soundcloud.com", "soundcloud.app.goo.gl"}, true},
    {"direct", "直接链接", {"", ""}, true},   // 占位，不会靠域名命中
}};

}  // namespace

bool url_is_well_formed(std::string_view url) {
    const std::string text = trim_ascii(url);
    if (text.empty() || text.size() > 4096) {
        return false;
    }
    // 控制字符与空格一律不接受：它们要么是粘贴时带进来的，要么是想骗解析器
    for (const char ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x21 || byte == 0x7F) {
            return false;
        }
    }
    std::string_view rest;
    if (text.starts_with("https://")) {
        rest = std::string_view(text).substr(8);
    } else if (text.starts_with("http://")) {
        rest = std::string_view(text).substr(7);
    } else if (text.starts_with("HTTP://") || text.starts_with("HTTPS://")) {
        // 大小写不敏感地接受，但主机名要重新算
        rest = std::string_view(text).substr(text.find("//") + 2);
    } else {
        return false;
    }
    // 主机名：到第一个 / ? # 为止，必须非空且含点
    const std::size_t cut = rest.find_first_of("/?#");
    const std::string_view host = cut == std::string_view::npos ? rest : rest.substr(0, cut);
    if (host.empty()) {
        return false;
    }
    const std::size_t colon = host.find(':');
    const std::string_view name = colon == std::string_view::npos ? host : host.substr(0, colon);
    return name.find('.') != std::string_view::npos && name.front() != '.' && name.back() != '.';
}

UrlClassification classify_url(std::string_view url) {
    UrlClassification result;
    const std::string text = trim_ascii(url);
    if (!url_is_well_formed(text)) {
        result.kind = ImportSourceKind::unsupported;
        result.platform = "invalid";
        result.display_name = "无效链接";
        result.rejection_reason = "链接格式不对：需要一个 http:// 或 https:// 开头、带主机名的完整地址";
        return result;
    }

    const std::string lower = lowercase_ascii(text);

    for (const PlatformEntry& entry : kPlatforms) {
        if (entry.id == "direct") {
            continue;
        }
        if (!contains_any(lower, entry.domains)) {
            continue;
        }
        result.platform = std::string(entry.id);
        result.display_name = std::string(entry.display);
        if (!entry.supported) {
            result.kind = ImportSourceKind::unsupported;
            result.rejection_reason =
                "暂不支持 " + std::string(entry.display) + " 链接导入";
            return result;
        }
        result.kind = ImportSourceKind::yt_dlp;
        return result;
    }

    // 不是已知站点：看一下路径是不是直接指向一个音频文件。
    // 是的话不必要求用户装 yt-dlp —— 原版也是这么分的。
    std::string_view path;
    std::string_view query;
    split_url(lower, path, query);
    static constexpr std::array<std::string_view, 8> kAudioExtensions = {
        ".mp3", ".ogg", ".wav", ".flac", ".m4a", ".aac", ".opus", ".wma"};
    const std::string extension = extension_of(path);
    if (std::find(kAudioExtensions.begin(), kAudioExtensions.end(), extension) !=
        kAudioExtensions.end()) {
        result.kind = ImportSourceKind::direct_http;
        result.platform = "direct_audio";
        result.display_name = "直接链接";
        return result;
    }

    // 其他站点交给 yt-dlp 试 —— 它能处理的站点远多于上面列出的
    result.kind = ImportSourceKind::yt_dlp;
    result.platform = "generic";
    result.display_name = "通用链接";
    return result;
}

std::string percent_decode(std::string_view text) {
    const auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        return -1;
    };

    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = hex_value(text[i + 1]);
            const int low = hex_value(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string stem_from_url(std::string_view url) {
    const std::string text = trim_ascii(url);
    std::string_view path;
    std::string_view query;
    split_url(text, path, query);

    // 主机名后面必须真的有路径，否则最后那一"段"其实是域名。
    // "https://example.com" 推出来的名字会是 "example" —— 那是错的，
    // 宁可不给名字，让调用方用自己的兜底名。
    const std::size_t scheme = text.find("://");
    const std::size_t path_start = text.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    if (path_start == std::string::npos) {
        return {};
    }

    std::string name = percent_decode(last_segment(path));
    if (name.empty()) {
        return {};
    }
    // 上面那条 find('/') 只保证"有个斜杠"，不保证"斜杠后面有东西"。
    // "https://example.com/" 的最后一段其实是域名，不是文件名 ——
    // 拿它当名字就会得到 "example"。
    if (path_start >= text.size() || path_start + 1 >= text.size()) {
        return {};
    }
    {
        const std::string_view segment = last_segment(path);
        const auto segment_offset =
            static_cast<std::size_t>(segment.data() - text.data());
        if (segment_offset <= path_start) {
            return {};   // 抓到的是域名，不是路径的最后一段
        }
    }
    // 去掉扩展名（"曲子.mp3" → "曲子"）
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        name.resize(dot);
    }
    // 净化成合法文件名。净化后仍不通过校验时返回空，让调用方用兜底名 ——
    // 不要在这里猜用户想要什么名字。
    name = sanitize_to_valid_name(name);
    Failure ignored;
    const bool valid = validate_file_stem(name, ignored);
    return valid ? name : std::string{};
}

bool parse_yt_dlp_progress_line(std::string_view line, double& fraction) {
    const std::string text = trim_ascii(line);
    // 只看下载阶段的行。合并/转码阶段也会打百分比，那会让人以为下载卡住了。
    if (!text.starts_with("[download]")) {
        // 直接下载模式（非 yt-dlp）也用同一个解析器，允许"[DL]"前缀
        if (!text.starts_with("[DL]")) {
            return false;
        }
    }

    const std::size_t percent = text.find('%');
    if (percent == std::string::npos) {
        return false;
    }
    // 从 % 往前扫数字与小数点
    std::size_t begin = percent;
    while (begin > 0) {
        const char ch = text[begin - 1];
        if ((ch >= '0' && ch <= '9') || ch == '.') {
            --begin;
            continue;
        }
        break;
    }
    if (begin == percent) {
        return false;   // "%" 前面没有数字
    }
    // 数字前面带符号就不认。往后退一格扫只会把 "-5" 读成 "5"，
    // 于是"负进度"被当成 5% 放过去 —— 那是在替数据编一个它没有的含义。
    if (begin > 0 && (text[begin - 1] == '-' || text[begin - 1] == '+')) {
        return false;
    }

    const std::string number = text.substr(begin, percent - begin);
    char* end = nullptr;
    const double value = std::strtod(number.c_str(), &end);
    if (end == number.c_str() || *end != '\0') {
        return false;
    }
    // 不合理就明确拒绝，不夹到 [0,1] —— 静默纠正也算静默回退
    if (!(value >= 0.0 && value <= 100.0)) {
        return false;
    }
    fraction = value / 100.0;
    return true;
}

std::string extract_yt_dlp_error(std::string_view output) {
    // 先找带 ERROR 的整行
    std::size_t offset = 0;
    std::string last_error;
    while (offset < output.size()) {
        const std::size_t end = output.find('\n', offset);
        const std::string_view line =
            output.substr(offset, end == std::string_view::npos ? std::string_view::npos
                                                                : end - offset);
        const std::string trimmed = trim_ascii(line);
        const std::string line_lower = lowercase_ascii(trimmed);
        if (line_lower.find("error") != std::string::npos) {
            last_error = trimmed;
        }
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1;
    }
    if (!last_error.empty()) {
        return last_error;
    }

    // 没有 ERROR 行就退而取最后一行非空输出
    std::size_t cursor = output.size();
    while (cursor > 0) {
        const std::size_t end = cursor;
        const std::size_t start = output.rfind('\n', cursor - 1);
        const std::string_view line =
            output.substr(start == std::string_view::npos ? 0 : start + 1,
                          end - (start == std::string_view::npos ? 0 : start + 1));
        const std::string trimmed = trim_ascii(line);
        if (!trimmed.empty()) {
            return trimmed;
        }
        if (start == std::string_view::npos) {
            break;
        }
        cursor = start;
    }
    return {};
}

std::string suggest_yt_dlp_fix(std::string_view output) {
    const std::string lower = lowercase_ascii(output);

    const auto has = [&lower](std::string_view needle) {
        return lower.find(needle) != std::string::npos;
    };

    if (has("ffmpeg") && (has("not found") || has("postprocessing") || has("postprocessor"))) {
        return "缺少 ffmpeg：转成 mp3 需要它。装上 ffmpeg 并把它放进 PATH。";
    }
    if (has("unsupported url") || has("no suitable extractor")) {
        return "yt-dlp 不认识这个链接。先试 `yt-dlp -U` 更新，或者确认这个站点还能匿名访问。";
    }
    if (has("403") || has("forbidden")) {
        return "对方拒绝了访问：可能需要登录凭证，或者这条内容有地区限制。";
    }
    if (has("404") || has("video unavailable") || has("not found")) {
        return "目标不存在或已被删除。";
    }
    if (has("ssl") || has("certificate")) {
        return "证书校验失败：检查系统时间，或是否有代理在中间拦截。";
    }
    if (has("timed out") || has("timeout")) {
        return "网络超时：检查网络或代理设置。";
    }
    if (has("unrecognized arguments") || has("no such option")) {
        return "本机的 yt-dlp 版本太旧，不认识这里用的参数。先 `yt-dlp -U` 更新。";
    }
    return {};
}

}  // namespace musicboard
