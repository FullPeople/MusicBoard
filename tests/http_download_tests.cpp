// 直链下载测试（对着 127.0.0.1 上的回环 HTTP 服务器，不碰公网）
//
// 这一层要钉住的是"直链下载也能被信任"：
//   · 有 Content-Length 时百分比是真的（收到的字节 / 声明的字节）
//   · **没有 Content-Length 时不编百分比** —— 那正是"进度条是假的"的来源之一
//   · HTTP 错误码带着自己的含义回来（404 与 403 的建议不一样）
//   · 声明了长度却没下满 → 明确失败，不把半个文件当成功
//   · 200 但 0 字节 → 明确失败（很多站点用"空体 + 200"表示"你需要登录"）
//   · 取消能真的停下来
//   · 下载完之后按**嗅探到的真实容器**命名，而不是照 URL 的扩展名

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "core/path_utf8.h"
#include "library/http_download.h"
#include "tests/test_files.h"
#include "tests/test_harness.h"
#include "tests/test_http_server.h"

using namespace musicboard;
using musicboard::test::LoopbackHttpServer;
using musicboard::test::LoopbackResponse;
using musicboard::test::Registry;
using musicboard::test::TempDir;
using musicboard::test::wait_until;

namespace {

// 一段足够长的合法 WAV：8 kHz 单声道 16 bit，约 1.6 秒
std::vector<std::uint8_t> long_wav() {
    return musicboard::test::make_wav_bytes(1600);
}

bool run_to_finish(HttpDownload& download, int timeout_ms = 20000) {
    return wait_until([&download] { return download.finished(); },
                      [&download] { download.pump(); }, timeout_ms);
}

}  // namespace

int main() {
    Registry registry;

    // =======================================================================
    // Content-Disposition 解析（纯函数）
    // =======================================================================
    registry.check(filename_from_content_disposition(
                       "attachment; filename=\"曲子.mp3\"") == "曲子.mp3",
                   "带引号的 filename 解析出来");
    registry.check(filename_from_content_disposition("attachment; filename=plain.mp3") ==
                       "plain.mp3",
                   "不带引号的 filename 解析出来");
    registry.check(filename_from_content_disposition(
                       "attachment; filename*=UTF-8''%E4%B8%AD%E6%96%87.wav") == "中文.wav",
                   "RFC 5987 的 filename* 解码成中文");
    registry.check(filename_from_content_disposition(
                       "attachment; filename=\"wrong.mp3\"; filename*=UTF-8''right.ogg") ==
                       "right.ogg",
                   "filename* 优先于 filename（它才是为多字节名准备的）");
    registry.check(filename_from_content_disposition("inline").empty(),
                   "没有 filename 时返回空（不编一个名字）");
    registry.check(filename_from_content_disposition("").empty(), "空头返回空");

    // =======================================================================
    // 有 Content-Length：百分比是真的
    // =======================================================================
    {
        TempDir temp;
        LoopbackHttpServer server;
        LoopbackResponse response;
        response.body = long_wav();
        // 分片慢发，让测试能在中途观察到进度
        response.chunk_size = 2048;
        response.chunk_delay_ms = 5;
        registry.check(server.start(response, "/track.wav"), "回环服务器起来了");

        HttpDownload download;
        Failure failure;
        HttpDownloadRequest request;
        request.url = server.url();
        request.destination = temp.path() / L"out.part";
        registry.check_invocation([&] { return download.start(request, failure); }, "启动直链下载", failure);

        std::vector<double> observed;
        double previous = -1.0;
        bool monotonic = true;
        const bool ended = wait_until(
            [&] {
                download.pump();
                const double fraction = download.status().fraction;
                if (fraction != previous) {
                    if (fraction < previous) {
                        monotonic = false;
                    }
                    observed.push_back(fraction);
                    previous = fraction;
                }
                return download.finished();
            },
            [] {}, 20000);

        registry.check(ended, "下载在超时前结束");
        registry.check(download.status().state == HttpDownloadState::done, "状态是 done");
        registry.check(download.status().has_total, "拿到了总长度（服务器给了 Content-Length）");
        registry.check_eq(download.status().content_type == "audio/wav",
                          "Content-Type 被解析出来", download.status().content_type);
        registry.check(download.status().total_bytes == response.body.size(),
                       "总长度与服务器声明的一致");
        registry.check(download.status().received_bytes == response.body.size(),
                       "收到的字节数与内容一致");
        registry.check(download.status().fraction == 1.0, "最终百分比是 1.0");
        registry.check(monotonic, "进度不往回跳");
        registry.check(observed.size() >= 3, "下载**过程中**观察到多次进度变化");

        // 内容真的写对了
        std::error_code ec;
        registry.check(std::filesystem::file_size(request.destination, ec) == response.body.size(),
                       "落盘文件大小与响应体一致");
    }

    // =======================================================================
    // 没有 Content-Length：不编百分比
    // =======================================================================
    {
        TempDir temp;
        LoopbackHttpServer server;
        LoopbackResponse response;
        response.body = long_wav();
        response.send_content_length = false;   // 关键：不给长度
        registry.check(server.start(response, "/nolength.wav"), "起一个不给长度的服务器");

        HttpDownload download;
        Failure failure;
        HttpDownloadRequest request;
        request.url = server.url();
        request.destination = temp.path() / L"out.part";
        registry.check(download.start(request, failure), "启动下载");
        const bool ended = run_to_finish(download);

        registry.check(ended && download.status().state == HttpDownloadState::done,
                       "没有 Content-Length 也能正常下完");
        registry.check(!download.status().has_total, "has_total 为假（老实承认不知道总长）");
        registry.check_eq(download.status().fraction == 0.0,
                          "不给分母时百分比恒为 0（不编一个数字出来）",
                          std::to_string(download.status().fraction));
        registry.check(download.status().received_bytes == response.body.size(),
                       "但已下载字节数是准的（界面该显示这个）");
    }

    // =======================================================================
    // HTTP 错误码
    // =======================================================================
    {
        TempDir temp;
        LoopbackHttpServer server;
        LoopbackResponse response;
        response.status = 404;
        response.reason = "Not Found";
        response.body = {'n', 'o'};
        registry.check(server.start(response, "/missing.wav"), "起一个 404 服务器");

        HttpDownload download;
        Failure failure;
        HttpDownloadRequest request;
        request.url = server.url();
        request.destination = temp.path() / L"out.part";
        registry.check(download.start(request, failure), "启动下载");
        const bool ended = run_to_finish(download);
        registry.check(ended && download.status().state == HttpDownloadState::failed, "404 → 失败");
        registry.check(download.status().http_status == 404, "HTTP 状态码原样记下来");
        registry.check(download.failure_reason().find("404") != std::string::npos,
                       "原因里带上了状态码");
    }
    {
        TempDir temp;
        LoopbackHttpServer server;
        LoopbackResponse response;
        response.status = 500;
        response.reason = "Server Error";
        response.body = {'x'};
        registry.check(server.start(response, "/boom.wav"), "起一个 500 服务器");

        HttpDownload download;
        Failure failure;
        HttpDownloadRequest request;
        request.url = server.url();
        request.destination = temp.path() / L"out.part";
        registry.check(download.start(request, failure), "启动下载");
        registry.check(run_to_finish(download) &&
                           download.status().state == HttpDownloadState::failed,
                       "500 → 失败");
        registry.check(download.failure_reason().find("HTTP 500") != std::string::npos,
                       "原因里写的是 HTTP 500");
    }

    // =======================================================================
    // 传输被截断：声明了长度却没下满
    // =======================================================================
    {
        TempDir temp;
        LoopbackHttpServer server;
        LoopbackResponse response;
        response.body = long_wav();
        // 声明完整长度，但只发一半就关连接
        response.truncate_after_bytes = response.body.size() / 2;
        registry.check(server.start(response, "/cut.wav"), "起一个会掐断的服务器");

        HttpDownload download;
        Failure failure;
        HttpDownloadRequest request;
        request.url = server.url();
        request.destination = temp.path() / L"out.part";
        registry.check(download.start(request, failure), "启动下载");
        const bool ended = run_to_finish(download);
        registry.check(ended && download.status().state == HttpDownloadState::failed,
                       "传输被截断 → 失败（不能当成成功）");
        registry.check(download.failure_reason().find("传输不完整") != std::string::npos,
                       "原因说清了是传输不完整");
        registry.check(download.failure_reason().find("实际收到") != std::string::npos,
                       "原因里同时给出了声明值与实际值");
    }

    // =======================================================================
    // 200 但零字节
    // =======================================================================
    {
        TempDir temp;
        LoopbackHttpServer server;
        LoopbackResponse response;
        response.body.clear();
        registry.check(server.start(response, "/empty.wav"), "起一个空体服务器");

        HttpDownload download;
        Failure failure;
        HttpDownloadRequest request;
        request.url = server.url();
        request.destination = temp.path() / L"out.part";
        registry.check(download.start(request, failure), "启动下载");
        const bool ended = run_to_finish(download);
        registry.check(ended && download.status().state == HttpDownloadState::failed,
                       "200 但 0 字节 → 失败");
        registry.check(download.failure_reason().find("没有任何数据") != std::string::npos,
                       "原因说清了是 0 字节");
    }

    // =======================================================================
    // 取消
    // =======================================================================
    {
        TempDir temp;
        LoopbackHttpServer server;
        LoopbackResponse response;
        response.body = long_wav();
        response.chunk_size = 256;        // 慢慢发
        response.chunk_delay_ms = 20;     // 一共要好几秒
        registry.check(server.start(response, "/slow.wav"), "起一个慢速服务器");

        HttpDownload download;
        Failure failure;
        HttpDownloadRequest request;
        request.url = server.url();
        request.destination = temp.path() / L"out.part";
        registry.check(download.start(request, failure), "启动下载");

        const bool got_something = wait_until(
            [&] {
                download.pump();
                return download.status().received_bytes > 0;
            },
            [] {}, 10000);
        registry.check(got_something, "已经开始收到数据");
        registry.check(!download.finished(), "此刻还没下完");

        download.cancel();
        registry.check(download.finished(), "取消之后立刻算结束");
        registry.check(download.status().state == HttpDownloadState::cancelled,
                       "状态是 cancelled");
        registry.check(download.status().received_bytes < response.body.size(),
                       "确实是在下完之前停下的");
        // 取消必须真的停：再等一会儿，字节数不该再涨
        const std::uint64_t after_cancel = download.status().received_bytes;
        ::Sleep(200);
        download.pump();
        registry.check(download.status().received_bytes == after_cancel,
                       "取消之后不再有新数据（下载线程真的停了）");
    }

    // =======================================================================
    // URL 校验：能在这里报的错不推到工作线程
    // =======================================================================
    {
        TempDir temp;
        HttpDownload download;
        Failure failure;
        HttpDownloadRequest request;
        request.url = "不是链接";
        request.destination = temp.path() / L"out.part";
        registry.check(!download.start(request, failure), "无效链接直接拒绝");
        registry.check(failure.code == fail_code::invalid_argument, "错误码是 invalid_argument");

        request.url = "https://example.com/a.mp3";
        request.destination = std::filesystem::path{};
        Failure failure2;
        registry.check(!download.start(request, failure2), "没给目标文件直接拒绝");

        request.destination = temp.path() / L"out.part";
        request.url = "ftp://example.com/a.mp3";
        Failure failure3;
        registry.check(!download.start(request, failure3), "非 http/https 直接拒绝");
    }

    // =======================================================================
    // 连不上的主机：要报错，不能一直挂着
    // =======================================================================
    {
        TempDir temp;
        HttpDownload download;
        Failure failure;
        HttpDownloadRequest request;
        // 127.0.0.1 上没人听的端口（RFC 5737 的保留地址不用，回环更稳）
        request.url = "http://127.0.0.1:9/never.wav";
        request.destination = temp.path() / L"out.part";
        request.connect_timeout_seconds = 2.0;
        request.receive_timeout_seconds = 0.25;
        registry.check(download.start(request, failure), "启动");
        const bool ended = run_to_finish(download, 20000);
        registry.check(ended && download.status().state == HttpDownloadState::failed,
                       "连不上 → 失败（而不是永远挂着）");
        registry.check(!download.failure_reason().empty(), "给出了失败原因");
    }

    return registry.finish("http_download_tests");
}
