#include "library/import_job.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>

#include "audio/container_probe.h"
#include "audio/decoder.h"
#include "core/path_utf8.h"
#include "core/localization.h"
#include "library/library_ops.h"
#include "library/staging.h"

namespace musicboard {
namespace {

// 一个通用浏览器 UA。有些站点对空 UA 直接 403，这跟"要不要装作浏览器"
// 是两件事 —— 这只是让基础反爬别把我们挡在门外。
constexpr std::string_view kBrowserUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/120.0.0.0 Safari/537.36";

[[nodiscard]] std::string directory_of_current_executable() {
    wchar_t buffer[32768]{};
    const DWORD written = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) {
        return {};
    }
    return path_to_utf8(std::filesystem::path(buffer).parent_path());
}

[[nodiscard]] std::string environment_utf8(const wchar_t* name) {
    wchar_t buffer[32768]{};
    const DWORD written = ::GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) {
        return {};
    }
    return path_to_utf8(std::filesystem::path(buffer));
}

// UTF-8 → 路径。不是合法 UTF-8 就返回空路径。
// 别再写 std::filesystem::path(窄字符串) 了 —— 那个走活动代码页。
[[nodiscard]] std::filesystem::path path_of_utf8(std::string_view utf8) {
    std::filesystem::path path;
    Failure ignored;
    if (!path_from_utf8(utf8, path, ignored)) {
        return {};
    }
    return path;
}

// 在 PATH 里找
[[nodiscard]] bool search_path_for(const wchar_t* file_name, std::filesystem::path& out) {
    wchar_t buffer[32768]{};
    const DWORD written = ::SearchPathW(nullptr, file_name, nullptr,
                                       static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
    if (written == 0 || written >= std::size(buffer)) {
        return false;
    }
    out = std::filesystem::path(buffer);
    return true;
}

// 去掉结尾的换行与回车，以及 yt-dlp 的行内进度残留 \r
[[nodiscard]] std::string clean_line(std::string_view line) {
    std::string out(line);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) {
        out.pop_back();
    }
    return out;
}

}  // namespace

const char* import_job_state_name(ImportJobState state) noexcept {
    switch (state) {
        case ImportJobState::idle:
            return "idle";
        case ImportJobState::downloading:
            return "downloading";
        case ImportJobState::preparing:
            return "preparing";
        case ImportJobState::done:
            return "done";
        case ImportJobState::failed:
            return "failed";
        case ImportJobState::cancelled:
            return "cancelled";
    }
    return "unknown";
}

bool resolve_yt_dlp(std::filesystem::path& out, Failure& failure) {
    out.clear();
    std::vector<std::filesystem::path> candidates;

    // 便携版最可能的位置：跟本程序放在一起
    const std::string beside = directory_of_current_executable();
    if (!beside.empty()) {
        candidates.push_back(path_of_utf8(beside) / L"media-tools" / L"yt-dlp.exe");
        candidates.push_back(path_of_utf8(beside) / L"yt-dlp.exe");
    }
    const std::string profile = environment_utf8(L"USERPROFILE");
    if (!profile.empty()) {
        candidates.push_back(path_of_utf8(profile) / L"yt-dlp.exe");
        candidates.push_back(path_of_utf8(profile) / L".local" / L"bin" / L"yt-dlp.exe");
    }
    const std::string app_data = environment_utf8(L"LOCALAPPDATA");
    if (!app_data.empty()) {
        candidates.push_back(path_of_utf8(app_data) / L"Microsoft" / L"WinGet" / L"Links" /
                             L"yt-dlp.exe");
    }
    candidates.push_back(L"C:\\yt-dlp\\yt-dlp.exe");

    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (candidate.empty()) {
            continue;
        }
        if (std::filesystem::is_regular_file(candidate, ec)) {
            out = candidate;
            return true;
        }
    }
    std::filesystem::path from_path;
    if (search_path_for(L"yt-dlp.exe", from_path) &&
        std::filesystem::is_regular_file(from_path, ec)) {
        out = from_path;
        return true;
    }

    std::string searched;
    for (const auto& candidate : candidates) {
        if (!candidate.empty()) {
            searched += "\n  " + path_to_utf8(candidate);
        }
    }
    searched += "\n  PATH 里的 yt-dlp.exe";
    failure.set(fail_code::file_not_found,
                "没有找到 yt-dlp。找过这些位置：" + searched +
                    "\n从 https://github.com/yt-dlp/yt-dlp/releases 下载 yt-dlp.exe "
                    "放到本程序旁边，或者装进 PATH。");
    return false;
}

std::vector<std::string> build_yt_dlp_arguments(const std::string& url,
                                               const std::filesystem::path& job_dir) {
    // 输出模板用**绝对路径**指向本任务的暂存目录。
    // 相对路径会落在 yt-dlp 的工作目录上，那里可能是任何地方 —— 原版正是这样
    // 才需要事后去猜文件落在哪。
    const std::string output_template =
        path_to_utf8(job_dir / L"%(title)s.%(ext)s");

    std::vector<std::string> arguments{
        "--ignore-config",
        "--no-plugin-dirs",
        "--socket-timeout", "15",
        "--retries", "2",
        "--fragment-retries", "2",
        "--print", "before_dl:MUSICBOARD_SOURCE_DURATION=%(duration)s",
        "--no-simulate",
        "--no-quiet",
        // 不要整张专辑：用户点的是一个链接，不是一份播放列表
        "--no-playlist",
        // 进度按行输出，便于增量解析
        "--newline",
        "--progress",
        // 进度行里不要终端转义序列，否则解析要额外对付它们
        "--no-colors",
        // 只留音频，并转成 mp3（与原版一致）
        "-x",
        "--audio-format", "mp3",
        "--audio-quality", "0",
        "-o", output_template,
        "--user-agent", std::string(kBrowserUserAgent),
    };
    arguments.push_back(url);
    return arguments;
}

ImportJob::~ImportJob() {
    // 还在跑就杀掉整棵进程树
    cancel();
    if (preparation_worker_.joinable()) preparation_worker_.join();
    // keep_staging_ 为 true 表示"这次留下了证据"（失败），不动它。
    // 为 false 表示这次本该是干净的（成功/取消），再补一次清理尝试。
    if (!keep_staging_ && !status_.staging_dir.empty()) {
        Failure ignored;
        static_cast<void>(remove_staging_job_dir(status_.staging_dir, ignored));
    }
}

bool ImportJob::start(const ImportJobRequest& request, Failure& failure) {
    if (status_.state != ImportJobState::idle) {
        failure.set(fail_code::invalid_argument, "这个导入任务已经启动过了，请新建一个");
        return false;
    }

    request_ = request;
    request_.url = normalize_import_url(request_.url);
    status_ = ImportJobStatus{};

    if (!url_is_well_formed(request_.url)) {
        failure.set(fail_code::invalid_argument,
                    "链接格式不对：" + request_.url +
                        "（需要一个 http:// 或 https:// 开头、带主机名的完整地址）");
        status_.state = ImportJobState::failed;
        status_.failure_reason = failure.context;
        return false;
    }

    // 先把"走哪条路"判完，再建暂存目录。
    // 顺序反过来的话，一个注定要拒绝的链接也会在磁盘上留下一个空目录。
    if (request_.kind != ImportSourceKind::direct_http &&
        request_.kind != ImportSourceKind::yt_dlp) {
        failure.set(fail_code::invalid_argument, "这个链接不能导入：" + request_.url);
        status_.state = ImportJobState::failed;
        status_.failure_reason = failure.context;
        return false;
    }

    // 期望的名字只在直链路线下从 URL 推。yt-dlp 路线**不能**推：
    // 一个 YouTube 链接的最后一段是 "watch"，那样所有视频都会叫 watch。
    // 让暂存文件用自己的名字（yt-dlp 写的就是视频标题）才对。
    const bool preferred_stem_from_user = !request_.preferred_stem.empty();
    preferred_stem_from_user_ = preferred_stem_from_user;
    if (request_.kind == ImportSourceKind::direct_http && !preferred_stem_from_user) {
        request_.preferred_stem = stem_from_url(request_.url);
    }

    // ---- 暂存目录 ----
    if (request_.staging_root.empty()) {
        request_.staging_root = default_staging_root();
    }
    if (!create_staging_job_dir(request_.staging_root, status_.staging_dir, failure)) {
        status_.state = ImportJobState::failed;
        status_.failure_reason = failure.context;
        return false;
    }

    // ---- 直链：自己下，不必要求用户装 yt-dlp ----
    if (request_.kind == ImportSourceKind::direct_http) {
        // 下载时先用一个中立的临时名。真实扩展名要等嗅探结果 ——
        // URL 里写的是 .mp3 而内容是 ogg 这种事太常见了，
        // 直接照 URL 命名等于把这个错误固化到曲库里。
        const std::filesystem::path temporary = status_.staging_dir / L"download.part";
        HttpDownloadRequest download_request;
        download_request.url = request_.url;
        download_request.destination = temporary;
        if (!download_.start(download_request, failure)) {
            status_.state = ImportJobState::failed;
            status_.failure_reason = failure.context;
            return false;
        }
        using_http_download_ = true;
        status_.state = ImportJobState::downloading;
        status_.status_text = "正在下载…";
        return true;
    }

    // ---- 启动子进程 ----
    std::filesystem::path yt_dlp = request_.yt_dlp_path;
    if (yt_dlp.empty()) {
        Failure resolve_failure;
        if (!resolve_yt_dlp(yt_dlp, resolve_failure)) {
            failure = resolve_failure;
            status_.state = ImportJobState::failed;
            status_.failure_reason = failure.context;
            return false;
        }
    }

    yt_dlp_arguments_ = build_yt_dlp_arguments(request_.url, status_.staging_dir);
    const std::filesystem::path media_directory = yt_dlp.parent_path();
    std::error_code media_error;
    if (std::filesystem::is_directory(media_directory / L"plugins", media_error)) {
        yt_dlp_arguments_.insert(yt_dlp_arguments_.end() - 1, "--plugin-dirs");
        yt_dlp_arguments_.insert(yt_dlp_arguments_.end() - 1, path_to_utf8(media_directory / L"plugins"));
    }
    if (std::filesystem::is_regular_file(media_directory / L"ffmpeg.exe", media_error)) {
        yt_dlp_arguments_.insert(yt_dlp_arguments_.end() - 1, "--ffmpeg-location");
        yt_dlp_arguments_.insert(yt_dlp_arguments_.end() - 1, path_to_utf8(media_directory));
    }

    ProcessRunOptions run_options;
    run_options.executable = yt_dlp;
    run_options.arguments = yt_dlp_arguments_;
    run_options.stall_timeout_seconds = request_.stall_timeout_seconds;

    if (!runner_.start(run_options, failure)) {
        status_.state = ImportJobState::failed;
        status_.failure_reason = failure.context;
        return false;
    }

    status_.state = ImportJobState::downloading;
    status_.status_text = "正在获取音频信息…";
    return true;
}

void ImportJob::pump() {
    if (status_.state == ImportJobState::preparing) {
        if (!preparation_ready_.load(std::memory_order_acquire)) return;
        preparation_worker_.join();
        status_.playback_preparation_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - preparation_started_).count();
        status_.state = ImportJobState::done;
        status_.status_text = tr("已导入：", "Imported: ") + result_.final_stem;
        return;
    }
    if (status_.state != ImportJobState::downloading) {
        return;
    }

    // ---- 直链：读计数器，不阻塞 ----
    if (using_http_download_) {
        download_.pump();
        const HttpDownloadStatus& http = download_.status();
        status_.received_bytes = http.received_bytes;
        status_.has_total = http.has_total;
        if (http.has_total) {
            status_.fraction = http.fraction;
            status_.status_text =
                "正在下载 " + std::to_string(static_cast<int>(http.fraction * 100.0 + 0.5)) + "%";
        } else {
            // 没有 Content-Length 就不编百分比，老实显示已经下了多少
            status_.status_text =
                "正在下载 " + std::to_string(http.received_bytes / 1024) + " KB（对方未给出总长度）";
        }
        status_.content_type = http.content_type;
        status_.suggested_name = http.suggested_name;
        if (!http.content_type.empty()) {
            status_.detail = "Content-Type: " + http.content_type;
        }

        if (!download_.finished()) {
            return;
        }
        if (http.state == HttpDownloadState::cancelled) {
            status_.state = ImportJobState::cancelled;
            status_.status_text = "已取消";
            keep_staging_ = false;
            return;
        }
        if (http.state == HttpDownloadState::failed) {
            status_.state = ImportJobState::failed;
            status_.failure_reason = download_.failure_reason();
            if (http.http_status == 404) {
                status_.suggestion = "目标不存在或已被删除。";
            } else if (http.http_status == 403) {
                status_.suggestion = "对方拒绝了访问，可能需要登录凭证或带 Referer 才能下。";
            } else if (http.http_status >= 400) {
                status_.suggestion = "服务器拒绝了这次请求（HTTP " +
                                     std::to_string(http.http_status) + "）。";
            }
            return;
        }

        // 下完了：先按真实容器改名，再入库
        status_.fraction = 1.0;
        status_.status_text = "正在入库…";
        Failure rename_failure;
        if (!rename_staged_by_real_container(rename_failure)) {
            status_.state = ImportJobState::failed;
            status_.failure_reason = "下载完成但内容不是可解码的音频：" + rename_failure.to_string();
            status_.suggestion = "这个链接指的可能是一个网页（比如登录页或播放器页面），"
                                 "而不是音频文件本身。";
            return;
        }
        publish_from_staging();
        return;
    }

    runner_.pump();

    // ---- 吸收新行：进度是真的在动 ----
    for (const std::string& line : runner_.take_new_lines()) {
        const std::string text = clean_line(line);
        if (text.empty()) {
            continue;
        }
        if (parse_yt_dlp_duration_line(text, status_.source_duration_seconds)) continue;
        double fraction = 0.0;
        if (parse_yt_dlp_progress_line(text, fraction)) {
            // 进度只许往前走。yt-dlp 在合并/转码阶段会重新从 0 开始报，
            // 那时候数字往回跳对用户就是"卡了一下又重来"。
            if (fraction > status_.fraction) {
                status_.fraction = fraction;
            }
            status_.status_text =
                "正在下载 " + std::to_string(static_cast<int>(status_.fraction * 100.0 + 0.5)) + "%";
            continue;
        }
        status_.detail = text;
        if (text.find("[ExtractAudio]") != std::string::npos ||
            text.find("[ffmpeg]") != std::string::npos) {
            status_.status_text = "正在转成 mp3…";
        }
    }

    if (runner_.state() == ProcessState::running) {
        return;
    }

    status_.output_truncated = runner_.output_truncated();

    // ---- 子进程结束了 ----
    if (runner_.state() == ProcessState::cancelled) {
        status_.state = ImportJobState::cancelled;
        status_.status_text = "已取消";
        keep_staging_ = false;
        return;
    }
    if (runner_.state() == ProcessState::failed) {
        status_.state = ImportJobState::failed;
        status_.failure_reason = runner_.failure_reason();
        return;
    }

    const std::uint32_t exit_code = runner_.exit_code();
    if (exit_code != 0) {
        status_.state = ImportJobState::failed;
        const std::string tail = runner_.flush_tail_line();
        std::string output = runner_.captured_output();
        if (!tail.empty()) {
            output += "\n" + tail;
        }
        std::string reason = extract_yt_dlp_error(output);
        if (reason.empty()) {
            reason = "yt-dlp 没有给出可识别的错误信息";
        }
        status_.detail = "Downloader exit " + std::to_string(exit_code) + ": " + reason;
        status_.failure_reason = summarize_yt_dlp_failure(output);
        status_.suggestion = suggest_yt_dlp_fix(output);
        return;
    }

    // 成功：列暂存目录，不猜文件名
    status_.fraction = 1.0;
    status_.status_text = "正在入库…";
    publish_from_staging();
}

bool ImportJob::rename_staged_by_real_container(Failure& failure) {
    std::vector<std::filesystem::path> staged;
    if (!list_staged_files(status_.staging_dir, staged, failure)) {
        return false;
    }
    if (staged.size() != 1) {
        failure.set(fail_code::file_not_found,
                    "暂存目录里期望恰好 1 个文件，实际 " + std::to_string(staged.size()) + " 个");
        return false;
    }

    const std::filesystem::path source = staged.front();
    AudioInfo info;
    if (!probe_audio_file(source, info, failure)) {
        return false;
    }

    // 用嗅探到的容器当扩展名，而不是用 URL 或服务器给的那个。
    // 一个 # 号后面跟着 .mp3 实为 ogg 的链接很常见，照抄 URL 就等于
    // 把这个错误固化进曲库，以后每次都要靠嗅探去纠正它。
    const std::string extension = container_name(info.container);
    if (extension.empty() || extension == "unknown") {
        failure.set(fail_code::unsupported_format,
                    "嗅探出来的容器是 unknown，不能确定扩展名");
        return false;
    }

    // 名字优先级（从高到低）：
    //   1. 调用方明确给的主干（用户在导入对话框里改过名字）
    //   2. 服务器在 Content-Disposition 里给的名字 —— 它比 URL 更权威
    //   3. 从 URL 推出来的主干
    //   4. 兜底名
    // 所以要记住"主干到底是用户给的、还是我们从 URL 推的"：
    // 否则服务器给的好名字会被 URL 里那个（常常是哈希串的）名字压掉。
    const HttpDownloadStatus& http = download_.status();
    std::string stem = request_.preferred_stem;
    if (!preferred_stem_from_user_ && !http.suggested_name.empty()) {
        stem = http.suggested_name;
    }
    // 主干要去掉扩展名：服务器给的名字常常是 "song.mp3"。
    // 扩展名该由嗅探结果决定 —— 照抄服务器说的那个正是这一步要避免的事。
    if (const std::size_t dot = stem.find_last_of('.'); dot != std::string::npos && dot > 0) {
        stem.resize(dot);
    }
    {
        Failure validation;
        if (stem.empty() || !validate_file_stem(stem, validation)) {
            stem = sanitize_to_valid_name(stem);
        }
    }
    if (stem.empty()) {
        stem = "导入的音频";
    }

    const std::filesystem::path target = track_path_for(status_.staging_dir, stem, extension);
    if (target.empty()) {
        failure.set(fail_code::invalid_argument, "算不出目标文件名（名字不是合法 UTF-8）");
        return false;
    }

    // 暂存目录是每个任务独有的空目录，所以这里的重名只可能是
    // "download.part" 与被下出来的文件同名 —— 用带序号的名字避开。
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        failure.set(fail_code::file_open_failed,
                    "暂存目录里已经有 " + path_to_utf8(target.filename()));
        return false;
    }
    std::filesystem::rename(source, target, ec);
    if (ec) {
        failure.set(fail_code::file_open_failed,
                    "给下载下来的文件改名失败 " + path_to_utf8(source) + " → " +
                        path_to_utf8(target) + ": " + ec.message());
        return false;
    }
    return true;
}

void ImportJob::publish_from_staging() {
    std::vector<std::filesystem::path> staged;
    Failure failure;
    if (!list_staged_files(status_.staging_dir, staged, failure)) {
        status_.state = ImportJobState::failed;
        status_.failure_reason = failure.context;
        return;
    }

    if (staged.empty()) {
        // yt-dlp 报成功却什么都没产出 —— 说清楚，不假装成功
        status_.state = ImportJobState::failed;
        status_.failure_reason =
            "yt-dlp 报告成功，但暂存目录里没有任何文件。它可能只找到了元数据但没有可下载的音频流。";
        return;
    }
    if (staged.size() > 1) {
        // --no-playlist 之下不该出现，所以这是"我们不知道发生了什么"，
        // 那就把实际看到的东西报出来，而不是挑一个（挑一个就是猜）。
        std::string names;
        for (const auto& path : staged) {
            names += "\n  " + path_to_utf8(path.filename());
        }
        status_.state = ImportJobState::failed;
        status_.failure_reason =
            "暂存目录里出现了 " + std::to_string(staged.size()) +
            " 个文件，无法确定该导入哪一个（期望 1 个）：" + names;
        return;
    }

    ImportRequest import_request;
    import_request.staged_file = staged.front();
    import_request.staging_root = status_.staging_dir;
    import_request.target_category_dir = request_.target_category_dir;
    // 只把**用户明确给过的**名字传下去。
    //
    // 直链路线下 request_.preferred_stem 可能已经被我们从 URL 推过了
    // （"actually_wav" 这种），而服务器给的好名字已经在改名那一步写进了
    // 文件名里 —— 再把 URL 推的那个传下去会把好名字压掉。
    // yt-dlp 路线同理：暂存文件的名字就是视频标题，那才是该用的。
    import_request.preferred_stem =
        preferred_stem_from_user_ ? request_.preferred_stem : std::string{};
    import_request.source_duration_seconds = status_.source_duration_seconds;

    Failure import_failure;
    if (!import_staged_file(import_request, result_, import_failure)) {
        status_.state = ImportJobState::failed;
        status_.failure_reason = "入库失败：" + import_failure.to_string();
        // 暂存目录留着 —— 文件还在，用户可以自己去拿
        return;
    }

    status_.state = ImportJobState::done;
    status_.fraction = 1.0;
    status_.status_text = "已导入：" + result_.final_stem;
    if (result_.content_incomplete) {
        std::ostringstream warning;
        warning << std::fixed << std::setprecision(0)
            << tr("来源标注 ", "The source states ") << status_.source_duration_seconds
            << tr(" 秒，但只取得 ", " seconds, but only ") << result_.duration_seconds
            << tr(" 秒。可能仅提供试听片段。", " seconds were received. This may be a preview.");
        status_.content_warning = warning.str();
        status_.status_text += "；" + status_.content_warning;
    }
    keep_staging_ = false;
    // 文件已经被事务搬进曲库，这个目录现在是空的（或只剩 .part 残片）。
    // 清不掉也不报错：next 启动时的 prune_staging 会把旧目录收走。
    Failure ignored;
    static_cast<void>(remove_staging_job_dir(status_.staging_dir, ignored));
    if (request_.prepare_playback) {
        status_.state = ImportJobState::preparing;
        status_.status_text = tr("已下载，正在准备播放…", "Downloaded; preparing playback…");
        preparation_started_ = std::chrono::steady_clock::now();
        preparation_worker_ = std::thread([this] {
            if (probe_audio_file(result_.installed_path, playback_info_, preparation_failure_))
                static_cast<void>(prepare_audio(result_.installed_path, playback_info_, prepared_audio_,
                    preparation_failure_, &preparation_cancelled_, {}));
            preparation_ready_.store(true, std::memory_order_release);
        });
    }
}

void ImportJob::cancel() {
    if (status_.state == ImportJobState::preparing) {
        preparation_cancelled_.store(true);
        preparation_worker_.join();
        status_.state = ImportJobState::done;
        status_.playback_preparation_cancelled = true;
        status_.status_text = tr("音频已保存，已取消播放准备。", "Audio saved; playback preparation cancelled.");
        return;
    }
    if (status_.state != ImportJobState::downloading) {
        return;
    }
    if (using_http_download_) {
        download_.cancel();
    } else {
        runner_.cancel();
    }
    status_.state = ImportJobState::cancelled;
    status_.status_text = "已取消";
    keep_staging_ = false;

    // 取消之后子进程/下载线程已经停了，但文件可能还被句柄占着一小会儿。
    // 清不掉也不算失败 —— 下次启动的 prune_staging 会收拾。
    Failure ignored;
    static_cast<void>(remove_staging_job_dir(status_.staging_dir, ignored));
}

}  // namespace musicboard
