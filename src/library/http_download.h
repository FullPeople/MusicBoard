#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 直链下载（WinHTTP 流式）
//
// 为什么直链不走 yt-dlp：一个 `https://…/track.mp3` 让用户先去装 yt-dlp
// 是没道理的。原版也是这么分的（`_is_direct_audio_url`）。
//
// 为什么要有独立的「已下载字节数」而不只有百分比：很多文件服务器不给
// Content-Length（分块传输）。这时分母是未知的，**只能显示已经下了多少字节**。
// 原版的做法是编一个进度 —— 那正是"进度条是假的"的来源之一。
// 这里 `has_total` 为假就是假，界面该显示字节数而不是假装有百分比。
//
// 线程模型：下载在一个工作线程上跑（WinHTTP 的读是阻塞的，把它塞进渲染循环
// 一定会卡帧），主线程只读原子计数。`cancel()` 只置一个标志位，
// 工作线程在最多一个接收超时之内看到并自己收拾干净 ——
// **不**从另一个线程去关它的句柄，那样省 200 毫秒但换来一类很难查的竞态。
// ============================================================================

struct HttpDownloadRequest final {
    std::string url;
    // 目标文件。调用方必须保证它落在暂存区里（这个模块自己不认暂存区）。
    std::filesystem::path destination;
    double resolve_timeout_seconds = 15.0;
    double connect_timeout_seconds = 15.0;
    double send_timeout_seconds = 30.0;
    // 单次接收超时。它同时决定了取消的响应延迟上限。
    double receive_timeout_seconds = 0.25;
    // 上限保护：一个直链不该把磁盘写满
    std::uint64_t maximum_bytes = 2ull * 1024ull * 1024ull * 1024ull;
};

enum class HttpDownloadState : std::uint8_t {
    idle,
    running,
    // 收到完整响应并写完文件
    done,
    // 失败：网络、HTTP 状态码、写文件
    failed,
    // 被 cancel() 中止
    cancelled,
};

[[nodiscard]] const char* http_download_state_name(HttpDownloadState state) noexcept;

struct HttpDownloadStatus final {
    HttpDownloadState state = HttpDownloadState::idle;
    std::uint64_t received_bytes = 0;
    // 服务器给了 Content-Length 时才有意义（has_total 为真）
    std::uint64_t total_bytes = 0;
    bool has_total = false;
    // has_total 为真时是 received/total；否则恒为 0（不编数字）
    double fraction = 0.0;
    int http_status = 0;
    std::string content_type;
    // 服务器/URL 给出的文件名（已净化）。可能是空的。
    std::string suggested_name;
};

class HttpDownload final {
public:
    HttpDownload() = default;
    ~HttpDownload();

    HttpDownload(const HttpDownload&) = delete;
    HttpDownload& operator=(const HttpDownload&) = delete;

    [[nodiscard]] bool start(const HttpDownloadRequest& request, Failure& failure);

    // 把工作线程已经更新的计数搬到 status()。**不阻塞。**
    void pump();

    // 置取消标志。工作线程在最多一个接收超时内收场。
    void cancel();

    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] const HttpDownloadStatus& status() const noexcept { return status_; }
    [[nodiscard]] const std::string& failure_reason() const noexcept { return failure_reason_; }

private:
    void run();   // 工作线程主体

    HttpDownloadRequest request_;
    std::thread worker_;
    std::atomic<HttpDownloadState> state_{HttpDownloadState::idle};
    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> total_{0};
    std::atomic<int> http_status_{0};
    std::atomic<bool> cancel_requested_{false};
    // 服务器给过 Content-Length。主线程读它，所以是原子的 ——
    // 一个"只在工作线程里写的普通 bool"就是一条数据竞态。
    std::atomic<bool> has_total_{false};

    mutable std::mutex text_mutex_;
    std::string failure_reason_;
    std::string content_type_;
    std::string suggested_name_;

    HttpDownloadStatus status_;
};

// 从 Content-Disposition 头里取文件名（支持 RFC 5987 的 filename*=UTF-8''）。
// 取不到就返回空串。
[[nodiscard]] std::string filename_from_content_disposition(std::string_view header);

}  // namespace musicboard
