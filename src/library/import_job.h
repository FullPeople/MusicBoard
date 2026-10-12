#pragma once

#include <chrono>
#include <atomic>
#include <thread>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/failure.h"
#include "audio/prepared_audio.h"
#include "library/http_download.h"
#include "library/import_transaction.h"
#include "library/process_runner.h"
#include "library/url_classify.h"

namespace musicboard {

// ============================================================================
// 一次 URL 导入
//
// 状态机：idle → downloading → publishing → preparing（可选后台准备）→ done
//                                        ↘ failed
//                    ↘ cancelled
// publishing 仍使用同步入库事务；跨盘大文件的后台发布尚待改进。
//
// 与原版（url_importer.gd）的差别，每一条都对着一个具体问题：
//
//   · **进度是真的。** 原版只在开始（0.0）和结束（1.0）各发一次进度，
//     中间的百分比是下完之后才从整段输出里抠出来的 —— 进度条永远不会动。
//     这里每一帧都从子进程的管道里读新行，`status().fraction` 在下载过程中真的在变。
//   · **取消是真的。** 原版的 cancel 只把标志位置回 false，下载照跑到完，
//     而且立刻允许再点一次导入（两个后台线程写同一份结果）。
//     这里 cancel() 走 TerminateJobObject，并且顺手清掉暂存目录。
//   · **不猜文件名。** 原版用 `_find_newest_file()` 猜哪个是刚下的。
//     这里每个任务一个独立空目录，列出来就是答案；只要一个文件就走事务发布，
//     出现别的数量就明确失败（连"为什么"一起报出来）。
//   · **失败留证据。** 失败时不删暂存目录，并把路径放进 status().staging_dir。
// ============================================================================

enum class ImportJobState : std::uint8_t {
    idle,
    downloading,
    preparing,
    done,
    failed,
    cancelled,
};

[[nodiscard]] const char* import_job_state_name(ImportJobState state) noexcept;

struct ImportJobRequest final {
    std::string url;
    // 已经在 start() 里判定好了，也允许调用方显式覆盖（测试用）
    ImportSourceKind kind = ImportSourceKind::unsupported;
    // yt_dlp 路线必须给（用 resolve_yt_dlp 找）
    std::filesystem::path yt_dlp_path;
    std::filesystem::path staging_root;
    std::filesystem::path target_category_dir;
    // 期望的文件名主干。空则从 URL 推，再空则用暂存文件自己的名字。
    std::string preferred_stem;
    // 多久没有输出就判定卡死
    double stall_timeout_seconds = 120.0;
    bool prepare_playback = false;
};

struct ImportJobStatus final {
    ImportJobState state = ImportJobState::idle;
    // 0..1。下载过程中真的在动；进入入库阶段之后是 1.0。
    //
    // **直链没有 Content-Length 时它恒为 0**，这不是 bug：没有分母就不编百分比。
    // 那种情况下界面该显示 received_bytes。
    double fraction = 0.0;
    // 直链下载才有意义：已经收到多少字节。yt-dlp 路线下恒为 0。
    std::uint64_t received_bytes = 0;
    bool has_total = false;
    // 给界面的一行状态。例如"正在下载 45%"、"正在转成 mp3"、"正在入库"
    std::string status_text;
    // 最近一条**不是**进度行的子进程输出（例如 yt-dlp 的 Destination / 警告）
    std::string detail;
    // 直链才有：服务器给的 Content-Type，以及它建议的文件名（已净化）。
    // 放进状态里既是为了界面能显示，也是为了让"服务器说了什么"可被断言。
    std::string content_type;
    std::string suggested_name;
    // 失败时：一句原因 + 一句可执行的建议（建议可能为空）
    std::string failure_reason;
    std::string suggestion;
    // 失败时保留的暂存目录，便于排查
    std::filesystem::path staging_dir;
    bool output_truncated = false;
    double source_duration_seconds = 0.0;
    std::string content_warning;
    bool playback_preparation_cancelled = false;
    double playback_preparation_seconds = 0.0;
};

class ImportJob final {
public:
    ImportJob() = default;
    ~ImportJob();

    ImportJob(const ImportJob&) = delete;
    ImportJob& operator=(const ImportJob&) = delete;

    [[nodiscard]] bool start(const ImportJobRequest& request, Failure& failure);

    // 泵一次：读子进程输出、刷新进度、必要时推进到入库。**不阻塞。**
    void pump();

    // 取消。幂等。已经结束时调用不会把 done 改写成 cancelled。
    void cancel();

    [[nodiscard]] const ImportJobStatus& status() const noexcept { return status_; }
    [[nodiscard]] const ImportResult& result() const noexcept { return result_; }
    [[nodiscard]] const std::shared_ptr<const PreparedAudio>& prepared_audio() const noexcept { return prepared_audio_; }
    [[nodiscard]] const AudioInfo& playback_info() const noexcept { return playback_info_; }
    [[nodiscard]] const Failure& preparation_failure() const noexcept { return preparation_failure_; }

private:
    void publish_from_staging();
    // 直链下载完之后，用嗅探到的真实容器给文件改名。
    // 服务器给的名字（Content-Disposition）与 URL 的扩展名都可能是错的 ——
    // 扩展名是用户随手改的，这条纪律在解码层已经立过（见 AGENTS.md 第 5 条边界）。
    [[nodiscard]] bool rename_staged_by_real_container(Failure& failure);

    ImportJobRequest request_;
    ProcessRunner runner_;
    HttpDownload download_;
    bool using_http_download_ = false;
    ImportJobStatus status_;
    ImportResult result_;
    // 主干到底是调用方给的，还是我们从 URL 推的。命名优先级要用到它。
    bool preferred_stem_from_user_ = false;
    std::vector<std::string> yt_dlp_arguments_;
    bool keep_staging_ = true;
    std::thread preparation_worker_;
    std::atomic<bool> preparation_cancelled_{false};
    std::atomic<bool> preparation_ready_{false};
    std::shared_ptr<const PreparedAudio> prepared_audio_;
    AudioInfo playback_info_;
    Failure preparation_failure_;
    std::chrono::steady_clock::time_point preparation_started_;
};

// 找 yt-dlp。找不到时 failure.context 会列出找过哪些位置。
// 只做存在性检查，不试跑 —— 一个"存在但坏了"的 yt-dlp 会在下载时带着它自己的
// 错误信息失败，那比在检测阶段猜一句"不可用"有用。
[[nodiscard]] bool resolve_yt_dlp(std::filesystem::path& out, Failure& failure);

// 拼 yt-dlp 的参数表。单独暴露是为了测试能直接断言"我们到底让它做什么"。
[[nodiscard]] std::vector<std::string> build_yt_dlp_arguments(const std::string& url,
                                                             const std::filesystem::path& job_dir);

}  // namespace musicboard
