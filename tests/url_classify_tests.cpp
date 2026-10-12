// URL 判定与 yt-dlp 输出解析测试
//
// 全是纯函数，所以可以完全离线穷举。这里钉住的每一条都对应一个具体的
// 用户会遇到的判断：
//   · 这个链接该走哪条路（yt-dlp / 直接下载 / 明确拒绝）
//   · 从 URL 推出的文件名会不会是个坑（"watch"、路径分隔符、非法字符）
//   · yt-dlp 的进度行解析得对不对（这决定进度条会不会动）
//   · 失败时能不能给出一句人话，而不是一整段栈

#include <string>
#include <vector>

#include "library/url_classify.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;

namespace {

// yt-dlp 真实打出来的进度行长这样（含行尾空格与单位）。
// 这些都是从 yt-dlp 的输出格式来的，不是我编的。
constexpr std::string_view kRealProgressLines[] = {
    "[download]   0.0% of    5.32MiB at  Unknown B/s ETA Unknown",
    "[download]   0.1% of    5.32MiB at  512.00KiB/s ETA 00:10",
    "[download]  45.2% of    5.32MiB at    1.23MiB/s ETA 00:03",
    "[download] 100.0% of    5.32MiB at    2.00MiB/s ETA 00:00",
    "[download] 100% of 5.32MiB in 00:04",
};

}  // namespace

int main() {
    Registry registry;

    // =======================================================================
    // URL 形状检查
    // =======================================================================
    registry.check(url_is_well_formed("https://www.youtube.com/watch?v=abc"), "普通 https 链接通过");
    registry.check(url_is_well_formed("http://127.0.0.1:8080/a.mp3"), "带端口的 http 通过");
    registry.check(url_is_well_formed("https://a.b/c"), "最短合法链接通过");

    registry.check(!url_is_well_formed(""), "空串不通过");
    registry.check(!url_is_well_formed("   "), "全空白不通过");
    registry.check(!url_is_well_formed("www.youtube.com/watch?v=abc"), "缺协议头不通过");
    registry.check(!url_is_well_formed("ftp://a.b/c"), "ftp 不通过");
    registry.check(!url_is_well_formed("https://"), "只有协议头不通过");
    registry.check(!url_is_well_formed("https:///path"), "主机名为空不通过");
    registry.check(!url_is_well_formed("https://."), "主机名只有一个点不通过");
    registry.check(!url_is_well_formed("https://.com"), "主机名以点开头不通过");
    registry.check(!url_is_well_formed("https://a./x"), "主机名以点结尾不通过");
    registry.check(!url_is_well_formed("https://a.b/含 空格"), "含空格不通过（粘贴时带进来的）");
    registry.check(!url_is_well_formed("https://a.b/含\t制表符"), "含制表符不通过");
    registry.check(!url_is_well_formed(std::string("https://a.b/") + std::string(5000, 'x')),
                   "超长链接不通过");

    // =======================================================================
    // 平台判定
    // =======================================================================
    {
        const auto result = classify_url("https://www.youtube.com/watch?v=dQw4w9WgXcQ");
        registry.check(result.kind == ImportSourceKind::yt_dlp, "YouTube → yt-dlp");
        registry.check(result.platform == "youtube", "平台是 youtube");
        registry.check(result.display_name == "YouTube", "显示名是 YouTube");
    }
    {
        const auto result = classify_url("https://youtu.be/dQw4w9WgXcQ");
        registry.check(result.kind == ImportSourceKind::yt_dlp && result.platform == "youtube",
                       "youtu.be 短链也认得出");
    }
    {
        const auto result = classify_url("https://music.163.com/song?id=1234567");
        registry.check(result.platform == "netease", "网易云认得出");
        registry.check(result.display_name == "网易云音乐", "网易云显示名是中文");
    }
    {
        const auto result = classify_url("https://soundcloud.com/artist/track");
        registry.check(result.platform == "soundcloud", "SoundCloud 认得出");
    }
    {
        const auto result = classify_url("https://www.bilibili.com/video/BV1xx411c7mD");
        registry.check(result.kind == ImportSourceKind::yt_dlp && result.platform == "bilibili", "哔哩哔哩交给已支持的提取器");
        registry.check(result.rejection_reason.empty(), "可导入平台没有预先拒绝原因");
    }
    {
        const auto result = classify_url("https://b23.tv/abcdefg");
        registry.check(result.kind == ImportSourceKind::yt_dlp && result.platform == "bilibili", "b23.tv 短链可以导入");
    }
    {
        registry.check(classify_url("https://y.qq.com/n/ryqq/songDetail/001234").platform == "qqmusic", "QQ 音乐主机识别");
        registry.check(classify_url("https://m.y.qq.com/portal/song.html?id=123").platform == "qqmusic", "QQ 音乐手机链接识别");
        registry.check(classify_url("https://www.kugou.com/song/abc.html").platform == "kugou", "酷狗链接识别");
        registry.check(classify_url("https://www.kuwo.cn/play_detail/123").platform == "kuwo", "酷我链接识别");
        registry.check(classify_url("https://artist.bandcamp.com/track/example").platform == "bandcamp", "Bandcamp 子域名识别");
        registry.check(classify_url("https://player.vimeo.com/video/123").platform == "vimeo", "Vimeo 子域名识别");
        registry.check(classify_url("https://youtube.com.invalid.example/song").platform == "generic", "相似主机名不会冒充平台");
        registry.check(classify_url("https://example.com/music.163.com/song.mp3").kind == ImportSourceKind::direct_http, "路径中的平台文字不改变直链类型");
        registry.check(classify_url("https://example.com/page?next=https://www.bilibili.com/video/123").platform == "generic", "查询串中的平台文字不改变来源");
    }
    {
        // 直接指向音频文件 → 自己下就行，不该要求用户装 yt-dlp
        const auto result = classify_url("https://example.com/music/song.mp3");
        registry.check(result.kind == ImportSourceKind::direct_http, "直链 mp3 → direct_http");
        registry.check(result.display_name == "直接链接", "显示名是直接链接");
    }
    {
        const auto result = classify_url("https://files.example.com/a/b/track.ogg?token=xyz");
        registry.check(result.kind == ImportSourceKind::direct_http,
                       "带查询串的 ogg 直链也认得（查询串不参与扩展名判断）");
    }
    {
        const auto result = classify_url("https://example.com/FLAC/a.FLAC");
        registry.check(result.kind == ImportSourceKind::direct_http, "大写扩展名同样认得");
    }
    {
        // 未知站点 → 交给 yt-dlp 试，它能处理的站点远多于我们列出的
        const auto result = classify_url("https://some-unknown-site.example/page/12345");
        registry.check(result.kind == ImportSourceKind::yt_dlp, "未知站点 → yt-dlp");
        registry.check(result.platform == "generic", "平台是 generic");
    }
    {
        const auto result = classify_url("不是链接");
        registry.check(result.kind == ImportSourceKind::unsupported, "乱输的内容被判为不支持");
        registry.check(!result.rejection_reason.empty(), "而且给出了原因");
        registry.check(result.platform == "invalid", "平台标为 invalid");
    }

    // =======================================================================
    // 从 URL 推文件名
    // =======================================================================
    {
        // 关键一条：YouTube 链接的最后一段是 watch。
        // 如果拿它当文件名，所有视频都会叫 watch —— 所以调用方不该用。
        // 这里只验证"推出来的东西本身是合法的"，用它与否由 import_job 决定。
        const std::string stem = stem_from_url("https://www.youtube.com/watch?v=abc");
        registry.check(stem == "watch", "watch 链接推出来的主干就是 watch（所以调用方不该用）");
    }
    registry.check(stem_from_url("https://example.com/music/我 的 歌.mp3") == "我 的 歌",
                   "直链 mp3 推出中文名");
    registry.check(stem_from_url("https://example.com/a/b/track.ogg?token=1") == "track",
                   "查询串被丢掉");
    registry.check(stem_from_url("https://example.com/%E4%B8%AD%E6%96%87%E6%AD%8C%E6%9B%B2.mp3") ==
                       "中文歌曲",
                   "URL 百分号编码解码成中文");
    registry.check(stem_from_url("https://example.com/") == "", "只有目录名的链接推不出名字");
    registry.check(stem_from_url("https://example.com") == "", "没有路径的链接推不出名字");
    registry.check(stem_from_url("https://example.com/a%3Ab%2Fc.mp3") == "a_b_c",
                   "编码出来的路径分隔符与冒号被净化（否则会建出嵌套目录或多一层）");
    registry.check(stem_from_url("https://example.com/CON.mp3") == "CON_",
                   "推出保留设备名时补后缀");
    registry.check(stem_from_url("https://example.com/....mp3") == "未命名",
                   "推出来的名字全是点 → 用兜底名");

    // =======================================================================
    // 百分号解码
    // =======================================================================
    registry.check(percent_decode("%E4%B8%AD") == "中", "UTF-8 三字节百分号解码");
    registry.check(percent_decode("a%20b") == "a b", "%20 解成空格");
    registry.check(percent_decode("%2B") == "+", "%2B 解成加号");
    registry.check(percent_decode("100%") == "100%", "裸的 % 原样保留");
    registry.check(percent_decode("%ZZ") == "%ZZ", "非法十六进制原样保留");
    registry.check(percent_decode("%4") == "%4", "截断的序列原样保留");
    registry.check(percent_decode("中文") == "中文", "没有百分号时原样返回");

    // =======================================================================
    // 进度行解析 —— 这一条决定进度条会不会动
    // =======================================================================
    {
        int parsed = 0;
        for (const std::string_view line : kRealProgressLines) {
            double fraction = -1.0;
            if (parse_yt_dlp_progress_line(line, fraction) && fraction >= 0.0 &&
                fraction <= 1.0) {
                ++parsed;
            }
        }
        registry.check(parsed == 5, "5 条真实的 yt-dlp 进度行全部解析成功");
    }
    {
        double fraction = 0.0;
        registry.check(parse_yt_dlp_progress_line("[download]  45.2% of 5.32MiB", fraction),
                       "45.2% 解析成功");
        registry.check_eq(fraction > 0.451 && fraction < 0.453, "数值正确（0.452）",
                          std::to_string(fraction));
    }
    {
        double fraction = -1.0;
        registry.check(parse_yt_dlp_progress_line("[download] 100% of 5.32MiB in 00:04", fraction) &&
                           fraction == 1.0,
                       "100% 解析成 1.0");
    }
    {
        double fraction = -1.0;
        registry.check(parse_yt_dlp_progress_line("[download]   0.0% of 5.32MiB", fraction) &&
                           fraction == 0.0,
                       "0.0% 解析成 0.0");
    }

    // 不是进度行的必须明确返回 false，不能瞎猜一个数字出来
    {
        double fraction = 0.0;
        registry.check(!parse_yt_dlp_progress_line("[youtube] abc: Downloading webpage", fraction),
                       "非进度行返回 false");
        registry.check(!parse_yt_dlp_progress_line("[download] Destination: /a/b.webm", fraction),
                       "Destination 行没有百分比 → false");
        registry.check(!parse_yt_dlp_progress_line("", fraction), "空行 → false");
        registry.check(!parse_yt_dlp_progress_line("[download] 完成", fraction), "没有 % → false");
        // 转码阶段的百分比不该被当成下载进度
        registry.check(!parse_yt_dlp_progress_line("[ffmpeg] 30% done", fraction),
                       "[ffmpeg] 行的百分比不算下载进度（否则进度条会跳回去）");
        // 不合理的数值直接拒绝，而不是夹到 [0,1]
        registry.check(!parse_yt_dlp_progress_line("[download] 250% of 1MiB", fraction),
                       "250% 被拒绝（不静默夹到 100%）");
        registry.check(!parse_yt_dlp_progress_line("[download] -5% of 1MiB", fraction),
                       "负数被拒绝");
    }

    // =======================================================================
    // 错误提取与建议
    // =======================================================================
    {
        const std::string output =
            "[youtube] abc: Downloading webpage\n"
            "WARNING: something odd\n"
            "ERROR: Unable to download webpage: HTTP Error 404: Not Found\n";
        const std::string error = extract_yt_dlp_error(output);
        registry.check(error.find("404") != std::string::npos, "错误行被提取出来（含 404）");
        registry.check(error.find("ERROR") != std::string::npos, "提取的是 ERROR 那一行");
    }
    {
        // 没有 ERROR 行时退而取最后一行非空输出，而不是返回空
        const std::string output = "第一行\n\n最后一行\n\n";
        registry.check(extract_yt_dlp_error(output) == "最后一行", "没有 ERROR 时取最后一行非空输出");
    }
    registry.check(extract_yt_dlp_error("").empty(), "空输出提取不到错误（返回空而不是编一个）");
    registry.check(extract_yt_dlp_error("\n\n").empty(), "只有换行也提取不到");

    {
        const auto suggestion = suggest_yt_dlp_fix("ERROR: Postprocessing: ffmpeg not found");
        registry.check(suggestion.find("重新安装") != std::string::npos, "缺少转换组件提示重新安装");
    }
    {
        const auto suggestion = suggest_yt_dlp_fix("ERROR: HTTP Error 403: Forbidden");
        registry.check(suggestion.find("拒绝") != std::string::npos, "403 给出对应建议");
    }
    {
        const auto suggestion = suggest_yt_dlp_fix("ERROR: Unsupported URL: https://x.y/z");
        registry.check(!suggestion.empty(), "unsupported url 给出对应建议");
    }
    {
        const auto suggestion =
            suggest_yt_dlp_fix("yt-dlp: error: unrecognized arguments: --progress-delta");
        registry.check(suggestion.find("更新") != std::string::npos,
                       "参数不认识 → 建议更新 yt-dlp（这正是不能依赖新版参数的原因）");
    }
    registry.check(suggest_yt_dlp_fix("完全看不懂的输出").empty(),
                   "看不懂的输出不给建议（总比给一条错的好）");
    registry.check(url_is_well_formed("hTtPs://music.163.com/song?id=1"), "混合大小写协议有效");
    registry.check(normalize_import_url("  hTtPs://music.163.com/SONG?id=AaBb#KeepCase  ") ==
        "https://music.163.com/SONG?id=AaBb#KeepCase", "规范化协议和粘贴空白，保留路径参数大小写");
    registry.check(summarize_yt_dlp_failure("The web client only works when logged-in. Use --cookies") ==
        "此链接需要登录后才能获取音频。", "登录提示不暴露下载器参数");
    registry.check(summarize_yt_dlp_failure("This video is unavailable") == "此内容已不可用。", "不可用内容有明确说明");
    registry.check(suggest_yt_dlp_fix("HTTP Error 429: Too Many Requests").find("稍后") != std::string::npos,
        "限流建议稍后重试");
    double duration = 0.0;
    registry.check(parse_yt_dlp_duration_line("MUSICBOARD_SOURCE_DURATION=197.5", duration) && duration == 197.5,
        "读取来源时长");
    for (const auto* invalid : {"MUSICBOARD_SOURCE_DURATION=NA", "MUSICBOARD_SOURCE_DURATION=nan",
            "MUSICBOARD_SOURCE_DURATION=inf", "MUSICBOARD_SOURCE_DURATION=-1", "MUSICBOARD_SOURCE_DURATION=197junk"})
        registry.check(!parse_yt_dlp_duration_line(invalid, duration), std::string("非法来源时长被拒绝：") + invalid);
    registry.check(downloaded_audio_is_incomplete(197.5, 30.0), "试听片段被标注为不完整");
    registry.check(!downloaded_audio_is_incomplete(197.5, 197.2), "编解码微小差异不误报");
    registry.check(!downloaded_audio_is_incomplete(0.0, 30.0), "未知来源时长不猜测完整性");

    return registry.finish("url_classify_tests");
}
