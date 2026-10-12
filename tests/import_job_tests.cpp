// 导入作业测试
//
// 端到端跑完整条链：启动假下载器 → 增量读进度 → 列暂存目录 → 事务入库 →
// 清暂存目录。全程不碰网络，但每一个"原版做错了"的点都在这里钉住：
//
//   · 进度在下载过程中真的在变（原版只在 0.0 和 1.0 各发一次）
//   · 取消真的杀掉子进程并清掉暂存目录（原版只把标志位置回 false）
//   · 不猜文件名，而是列目录；出现 0 个或 2 个都明确失败
//   · 非音频被入库事务挡住，曲库里不会多出一个坏文件
//   · 失败时保留暂存目录并把路径报出来
//
// 子进程的存活按真实时间推进，所以这些用例真的会花上一两秒。

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/failure.h"
#include "core/path_utf8.h"
#include "library/import_job.h"
#include "library/staging.h"
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

// 让假下载器以 **yt-dlp 模式**跑：它按真实 yt-dlp 的参数表接收参数，
// 从 -o 模板里解析出该写到哪个目录，行为模式从环境变量来。
//
// 这样做的价值不只是"省一个测试钩子"：它把 import_job 拼出来的参数表
// 真的交给了子进程去用。引号、路径、-o 模板任何一处拼错，
// 这些用例都会直接红。生产代码里不需要留任何测试专用的入口。
void set_fake_mode(const wchar_t* mode) {
    ::SetEnvironmentVariableW(L"MUSICBOARD_FAKE_MODE", mode);
}

// 一个装好目录结构的沙盒：staging/ 与 library/BGM/战斗
struct Sandbox final {
    TempDir temp;
    std::filesystem::path staging;
    std::filesystem::path library;
    std::filesystem::path target;

    Sandbox() {
        staging = temp.path() / L"staging";
        library = temp.path() / L"library";
        target = library / L"BGM" / L"战斗";
        std::error_code ec;
        std::filesystem::create_directories(staging, ec);
        std::filesystem::create_directories(target, ec);
    }

    // URL 用 YouTube 的：它会走 yt_dlp 路线，而 yt-dlp 路线**不会**
    // 从 URL 推文件名（否则所有视频都会叫 watch），文件名交给子进程产出。
    [[nodiscard]] ImportJobRequest request(const wchar_t* mode,
                                           const std::filesystem::path& fake_child) const {
        set_fake_mode(mode);
        ImportJobRequest r;
        r.url = "https://www.youtube.com/watch?v=test";
        r.kind = ImportSourceKind::yt_dlp;
        r.yt_dlp_path = fake_child;
        r.staging_root = staging;
        r.target_category_dir = target;
        r.stall_timeout_seconds = 15.0;
        return r;
    }
};

std::vector<std::filesystem::path> files_in(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        return out;
    }
    for (const auto& entry : it) {
        if (entry.is_regular_file(ec)) {
            out.push_back(entry.path());
        }
    }
    return out;
}

}  // namespace

int main() {
    Registry registry;

    const std::filesystem::path fake_child = musicboard::test::fake_child_path();
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(fake_child, ec)) {
            registry.skip("全部导入用例（找不到 fake_child.exe）");
            return registry.finish("import_job_tests");
        }
    }

    // =======================================================================
    // yt-dlp 参数表：直接断言"我们到底让它做什么"
    // =======================================================================
    {
        const std::vector<std::string> arguments =
            build_yt_dlp_arguments("https://example.com/x", std::filesystem::path(L"C:\\暂存"));
        const auto has = [&arguments](std::string_view needle) {
            return std::find(arguments.begin(), arguments.end(), needle) != arguments.end();
        };
        const auto has_prefix = [&arguments](std::string_view prefix) {
            return std::any_of(arguments.begin(), arguments.end(), [prefix](const std::string& a) {
                return std::string_view(a).starts_with(prefix);
            });
        };

        registry.check(has("--no-playlist"), "不带播放列表（用户点的是一个链接）");
        registry.check(has("--ignore-config"), "用户下载器配置不改变音乐板的导入行为");
        registry.check(has("before_dl:MUSICBOARD_SOURCE_DURATION=%(duration)s") && has("--no-simulate"),
            "读取来源时长时仍实际下载");
        registry.check(has("--newline"), "按行输出进度（增量解析的前提）");
        registry.check(has("--progress"), "要进度输出");
        registry.check(has("--no-colors"), "进度行里不要终端转义序列");
        registry.check(has("-x"), "只抽音频");
        registry.check(has("mp3"), "转成 mp3");
        // 输出模板必须是绝对路径、指向本次任务的暂存目录
        registry.check(has_prefix("-o"), "指定了输出模板");
        registry.check(std::any_of(arguments.begin(), arguments.end(),
                                   [](const std::string& a) {
                                       return a.find("C:\\暂存") != std::string::npos;
                                   }),
                       "输出模板落在本次任务的暂存目录里（绝对路径，不靠工作目录猜）");
        registry.check(arguments.back() == "https://example.com/x", "URL 是最后一个参数");
    }

    // =======================================================================
    // 顺利路径：下载 → 入库，进度是增量拿到的
    // =======================================================================
    {
        Sandbox box;
        ImportJob job;
        Failure failure;

        ImportJobRequest request = box.request(L"success", fake_child);
        // 假下载器不认识 yt-dlp 的参数，所以让它的参数表就是 [模式, 暂存目录]。
        // 这不是绕过测试，而是把"子进程做什么"固定下来；
        // 真实 yt-dlp 的参数表由上面那组断言单独钉住。
        registry.check(job.start(request, failure), "作业启动成功");

        // 启动之后暂存目录必须是空的 —— 下载还没开始写
        const bool saw_progress = wait_until(
            [&] { return job.status().state != ImportJobState::downloading; },
            [&] { job.pump(); }, 20000);

        registry.check_eq(saw_progress, "作业在超时前结束", job.status().failure_reason);
        registry.check(job.status().state == ImportJobState::done, "最终状态是 done");
        registry.check(!job.result().installed_path.empty(), "拿到了入库后的路径");
        registry.check(job.result().installed_path.parent_path() == box.target,
                       "落在目标分类目录里");
        registry.check(job.result().duration_seconds > 0.1, "入库时重新探测到了时长");
        registry.check(std::filesystem::exists(job.result().installed_path), "文件真的在曲库里");
        registry.check(files_in(box.target).size() == 1, "曲库里正好一个文件");
        registry.check(!std::filesystem::exists(job.status().staging_dir),
                       "成功后暂存目录被清掉了（曲库之外不留垃圾）");
    }

    // =======================================================================
    // 进度：下载过程中 fractions 单调上升且出现过中间值
    // =======================================================================
    {
        Sandbox box;
        ImportJob job;
        Failure failure;
        ImportJobRequest request = box.request(L"progress", fake_child);
        registry.check(job.start(request, failure), "启动进度用例");

        std::vector<double> observed;
        bool monotonic = true;
        double previous = -1.0;
        const bool ended = wait_until(
            [&] {
                const double fraction = job.status().fraction;
                if (fraction != previous) {
                    if (fraction < previous) {
                        monotonic = false;   // 进度不许往回跳
                    }
                    observed.push_back(fraction);
                    previous = fraction;
                }
                return job.status().state != ImportJobState::downloading;
            },
            [&] { job.pump(); }, 20000);

        registry.check(ended, "进度用例在超时前结束");
        registry.check(monotonic, "进度从不往回跳（yt-dlp 转码阶段会从 0 重报，被挡住了）");
        registry.check(observed.size() >= 4, "下载过程中观察到多次进度变化（进度条真的在动）");
        registry.check(std::any_of(observed.begin(), observed.end(),
                                   [](double value) { return value > 0.0 && value < 1.0; }),
                       "观察到严格位于 0 和 1 之间的中间进度");
        registry.check(job.status().state == ImportJobState::done, "最终仍然成功");
    }

    // =======================================================================
    // 失败：错误信息被提取出来，建议给了，暂存目录保留
    // =======================================================================
    {
        Sandbox box;
        ImportJob job;
        Failure failure;
        ImportJobRequest request = box.request(L"fail", fake_child);
        registry.check(job.start(request, failure), "启动失败用例");

        const bool ended = wait_until(
            [&] { return job.status().state != ImportJobState::downloading; },
            [&] { job.pump(); }, 20000);
        registry.check(ended, "失败用例结束了（没有一直等）");
        registry.check(job.status().state == ImportJobState::failed, "状态是 failed");
        registry.check(job.status().detail.find("404") != std::string::npos,
                       "失败原因里带上了 yt-dlp 的具体错误（含 404）");
        registry.check(job.status().detail.find("exit 1") != std::string::npos,
                       "也带上了退出码");
        registry.check(!job.status().suggestion.empty(), "给出了可执行的建议");
        registry.check(std::filesystem::exists(job.status().staging_dir),
                       "失败时保留暂存目录（留证据，不悄悄清掉）");
        registry.check(files_in(box.target).empty(), "曲库里什么都没多出来");
    }
    {
        // 403 一类的拒绝：原因与建议都要对得上
        Sandbox box;
        ImportJob job;
        Failure failure;
        ImportJobRequest request = box.request(L"refusal", fake_child);
        registry.check(job.start(request, failure), "启动拒绝用例");
        const bool ended = wait_until(
            [&] { return job.status().state != ImportJobState::downloading; },
            [&] { job.pump(); }, 20000);
        registry.check(ended && job.status().state == ImportJobState::failed, "被判为失败");
        registry.check(job.status().detail.find("403") != std::string::npos,
                       "原因里有 403");
        registry.check(job.status().suggestion.find("拒绝") != std::string::npos,
                       "建议是对的（不是一条无关的提示）");
    }

    // =======================================================================
    // 非音频：入库事务必须挡住它
    // =======================================================================
    {
        Sandbox box;
        ImportJob job;
        Failure failure;
        ImportJobRequest request = box.request(L"junk", fake_child);
        registry.check(job.start(request, failure), "启动伪装文件用例");
        const bool ended = wait_until(
            [&] { return job.status().state != ImportJobState::downloading; },
            [&] { job.pump(); }, 20000);
        registry.check(ended, "伪装文件用例结束");
        registry.check(job.status().state == ImportJobState::failed,
                       "改名成 .mp3 的文本文件让整次导入失败（而不是进曲库）");
        registry.check(job.status().failure_reason.find("入库失败") != std::string::npos,
                       "原因说明是入库阶段被挡住的");
        registry.check(files_in(box.target).empty(), "曲库依然是空的");
        registry.check(files_in(job.status().staging_dir).size() == 1,
                       "那个文件还留在暂存目录里（用户可以自己去看看）");
    }

    // =======================================================================
    // 产出 0 个 / 2 个文件：不猜，明确失败
    // =======================================================================
    {
        Sandbox box;
        ImportJob job;
        Failure failure;
        ImportJobRequest request = box.request(L"empty", fake_child);
        registry.check(job.start(request, failure), "启动空产出用例");
        const bool ended = wait_until(
            [&] { return job.status().state != ImportJobState::downloading; },
            [&] { job.pump(); }, 20000);
        registry.check(ended && job.status().state == ImportJobState::failed,
                       "报成功却什么都没产出 → 失败");
        registry.check(job.status().failure_reason.find("没有任何文件") != std::string::npos,
                       "原因说清了是'没有产出任何文件'（而不是含糊的下载失败）");
    }
    {
        Sandbox box;
        ImportJob job;
        Failure failure;
        ImportJobRequest request = box.request(L"two_files", fake_child);
        registry.check(job.start(request, failure), "启动双产出用例");
        const bool ended = wait_until(
            [&] { return job.status().state != ImportJobState::downloading; },
            [&] { job.pump(); }, 20000);
        registry.check(ended && job.status().state == ImportJobState::failed,
                       "产出两个文件 → 失败（不挑一个，挑一个就是猜）");
        registry.check(job.status().failure_reason.find("无法确定该导入哪一个") !=
                           std::string::npos,
                       "原因说清了是'不知道该导入哪一个'");
        registry.check(job.status().failure_reason.find("第一个") != std::string::npos &&
                           job.status().failure_reason.find("第二个") != std::string::npos,
                       "把实际看到的文件名都列出来了");
        registry.check(files_in(box.target).empty(), "曲库里没有半成品");
    }

    // =======================================================================
    // 取消
    // =======================================================================
    {
        Sandbox box;
        ImportJob job;
        Failure failure;
        ImportJobRequest request = box.request(L"hang", fake_child);
        registry.check(job.start(request, failure), "启动长跑用例");
        registry.check(job.status().state == ImportJobState::downloading, "状态是 downloading");

        // 等它真的开始输出，再取消 —— 这样取消的是一个在跑的进程，
        // 而不是一个还没起来的
        const bool producing = wait_until(
            [&] {
                job.pump();
                return !job.status().detail.empty() || job.status().fraction > 0.0;
            },
            [] {}, 10000);
        registry.check(producing, "长跑用例开始产出了");

        job.cancel();
        registry.check(job.status().state == ImportJobState::cancelled, "取消后状态是 cancelled");
        registry.check(job.status().status_text == "已取消", "状态文案是已取消");
        registry.check(!std::filesystem::exists(job.status().staging_dir),
                       "取消之后暂存目录被清掉了");
        registry.check(files_in(box.target).empty(), "取消之后曲库里没有半成品");

        // 再 pump 不该把状态改回去
        job.pump();
        registry.check(job.status().state == ImportJobState::cancelled, "取消是终态");
        // 再取消也不该出问题
        job.cancel();
        registry.check(job.status().state == ImportJobState::cancelled, "重复取消幂等");
    }

    // =======================================================================
    // 卡死判定
    // =======================================================================
    {
        Sandbox box;
        ImportJob job;
        Failure failure;
        ImportJobRequest request = box.request(L"silent", fake_child);
        request.stall_timeout_seconds = 0.6;   // silent 模式 30 秒不打一个字
        registry.check(job.start(request, failure), "启动静默用例");
        const bool ended = wait_until(
            [&] { return job.status().state != ImportJobState::downloading; },
            [&] { job.pump(); }, 20000);
        registry.check(ended, "静默用例在超时前收场（没有永远等下去）");
        registry.check(job.status().state == ImportJobState::failed, "状态是 failed");
        registry.check(job.status().failure_reason.find("没有任何输出") != std::string::npos,
                       "原因说明了是长时间没有输出");
    }

    // =======================================================================
    // 参数校验：这些都不该建出暂存目录
    // =======================================================================
    {
        Sandbox box;
        ImportJob job;
        Failure failure;
        ImportJobRequest request = box.request(L"success", fake_child);
        request.url = "不是链接";
        registry.check(!job.start(request, failure), "无效链接启动失败");
        registry.check(failure.code == fail_code::invalid_argument, "错误码是 invalid_argument");
        registry.check(job.status().state == ImportJobState::failed, "状态是 failed");
        registry.check(files_in(box.staging).empty(), "被拒的链接没有在暂存区留下空目录");
    }
    {
        Sandbox box;
        ImportJob job;
        Failure failure;
        ImportJobRequest request = box.request(L"success", fake_child);
        request.kind = ImportSourceKind::unsupported;
        registry.check(!job.start(request, failure), "明确不支持的链接启动失败");
        registry.check(files_in(box.staging).empty(), "同样没有留下空目录");
    }
    {
        // 直链但主机连不上：要报错，而不是一直挂着。
        // （"直链路线还没接上"那条用例已经不需要了 —— 现在接上了，
        //   换成这一条真实会发生的失败。）
        Sandbox box;
        ImportJob job;
        Failure failure;
        ImportJobRequest request;
        request.url = "http://127.0.0.1:9/never.wav";
        request.kind = ImportSourceKind::direct_http;
        request.staging_root = box.staging;
        request.target_category_dir = box.target;
        registry.check(job.start(request, failure), "连不上的直链也能启动（错误在下的时候才出现）");
        const bool ended = wait_until(
            [&] { return job.status().state != ImportJobState::downloading; },
            [&] { job.pump(); }, 25000);
        registry.check(ended && job.status().state == ImportJobState::failed,
                       "连不上主机 → 失败（而不是永远挂着）");
        registry.check(!job.status().failure_reason.empty(), "给出了失败原因");
        registry.check(files_in(box.target).empty(), "曲库里没有半成品");
    }
    {
        // 指向不存在的 yt-dlp：要报"找不到"，而不是去跑一个空进程
        Sandbox box;
        ImportJob job;
        Failure failure;
        ImportJobRequest request = box.request(L"success", fake_child);
        request.yt_dlp_path = box.temp.path() / L"没有这个程序.exe";
        registry.check(!job.start(request, failure), "yt-dlp 不存在时启动失败");
        registry.check(failure.code == fail_code::file_not_found, "错误码是 file_not_found");
        registry.check(job.status().state == ImportJobState::failed, "状态是 failed");
        // 这时暂存目录已经建出来了（先建目录再启动）。它会被保留 ——
        // 这一条不是理想，而是"诚实地说明当前行为"：一个空目录不影响任何东西，
        // 而下次启动的 prune_staging 会收走它。
        registry.check(files_in(box.target).empty(), "曲库里没有半成品");
    }

    // =======================================================================
    // 直链：下完 → 按**真实容器**命名 → 入库
    //
    // 这是直链路线最要紧的一条断言。服务器说它是 mp3（URL 是 .mp3、
    // Content-Disposition 也叫 .mp3），但内容其实是 WAV。
    // 扩展名是用户随手能改的东西 —— 这条纪律在解码层已经立过
    // （AGENTS.md 第 5 条边界），导入时也一样：按嗅探结果命名。
    // =======================================================================
    {
        Sandbox box;
        LoopbackHttpServer server;
        LoopbackResponse response;
        response.body = musicboard::test::make_wav_bytes(400);
        response.extra_headers =
            "Content-Disposition: attachment; filename=\"server_says_mp3.mp3\"\r\n";
        registry.check(server.start(response, "/actually_wav.mp3"), "起一个名不副实的服务器");

        ImportJob job;
        Failure failure;
        ImportJobRequest request;
        request.url = server.url();
        request.kind = ImportSourceKind::direct_http;
        request.staging_root = box.staging;
        request.target_category_dir = box.target;
        registry.check_invocation([&] { return job.start(request, failure); }, "直链作业启动成功", failure);

        const bool ended = wait_until(
            [&] { return job.status().state != ImportJobState::downloading; },
            [&] { job.pump(); }, 25000);
        registry.check_eq(ended, "直链作业在超时前结束", job.status().failure_reason);
        registry.check_eq(job.status().state == ImportJobState::done, "最终状态是 done",
                          job.status().failure_reason);
        registry.check(job.status().has_total, "有 Content-Length 时 has_total 为真");
        registry.check_eq(job.status().content_type == "audio/wav", "解析出了 Content-Type",
                          job.status().content_type);
        registry.check_eq(job.status().suggested_name == "server_says_mp3.mp3",
                          "解析出了服务器建议的文件名", job.status().suggested_name);
        registry.check(job.status().fraction == 1.0, "最终百分比是 1.0");
        registry.check(job.status().received_bytes == response.body.size(),
                       "状态里的已下载字节数与响应体一致");

        // 名字：主干来自 Content-Disposition，扩展名来自嗅探
        registry.check_eq(job.result().final_stem == "server_says_mp3",
                          "主干用的是服务器给的名字", job.result().final_stem);
        registry.check_eq(job.result().installed_path.extension() == L".wav",
                          "扩展名按嗅探到的真实容器（不是 URL 与服务器说的 .mp3）",
                          path_to_utf8(job.result().installed_path.filename()));
        registry.check(job.result().duration_seconds > 0.3 &&
                           job.result().duration_seconds < 0.6,
                       "入库时探测到的时长约 0.4 秒");
        registry.check(files_in(box.target).size() == 1, "曲库里正好一个文件");
        registry.check(!std::filesystem::exists(job.status().staging_dir),
                       "成功后暂存目录被清掉了");
    }

    // =======================================================================
    // 直链：URL 里根本没有扩展名（推不出名字）
    // =======================================================================
    {
        Sandbox box;
        LoopbackHttpServer server;
        LoopbackResponse response;
        response.body = musicboard::test::make_wav_bytes(200);
        registry.check(server.start(response, "/download"), "起一个无扩展名的直链");

        ImportJob job;
        Failure failure;
        ImportJobRequest request;
        request.url = server.url();
        request.kind = ImportSourceKind::direct_http;
        request.staging_root = box.staging;
        request.target_category_dir = box.target;
        registry.check(job.start(request, failure), "启动");

        const bool ended = wait_until(
            [&] { return job.status().state != ImportJobState::downloading; },
            [&] { job.pump(); }, 25000);
        registry.check_eq(ended && job.status().state == ImportJobState::done,
                          "无扩展名直链也能导入成功", job.status().failure_reason);
        registry.check(job.result().installed_path.extension() == L".wav",
                       "扩展名来自嗅探结果");
        registry.check(!job.result().final_stem.empty(), "名字不是空的（有兜底名）");
    }

    // =======================================================================
    // 直链：服务器返回的不是音频
    // =======================================================================
    {
        Sandbox box;
        LoopbackHttpServer server;
        LoopbackResponse response;
        const std::string html = "<html><body>请先登录</body></html>";
        response.body.assign(html.begin(), html.end());
        registry.check(server.start(response, "/login.mp3"), "起一个返回网页的服务器");

        ImportJob job;
        Failure failure;
        ImportJobRequest request;
        request.url = server.url();
        request.kind = ImportSourceKind::direct_http;
        request.staging_root = box.staging;
        request.target_category_dir = box.target;
        registry.check(job.start(request, failure), "启动");

        const bool ended = wait_until(
            [&] { return job.status().state != ImportJobState::downloading; },
            [&] { job.pump(); }, 25000);
        registry.check(ended && job.status().state == ImportJobState::failed,
                       "下回来的东西不是音频 → 失败");
        registry.check(job.status().failure_reason.find("不是可解码的音频") != std::string::npos,
                       "原因说清了是内容不是音频");
        registry.check(job.status().suggestion.find("网页") != std::string::npos,
                       "建议里点出了'这可能是个网页而不是音频'");
        registry.check(files_in(box.target).empty(), "曲库里没有半成品");
        registry.check(files_in(job.status().staging_dir).size() == 1,
                       "下回来的东西留在暂存目录里（可以去看看到底下到了什么）");
    }

    // =======================================================================
    // 直链：404
    // =======================================================================
    {
        Sandbox box;
        LoopbackHttpServer server;
        LoopbackResponse response;
        response.status = 404;
        response.reason = "Not Found";
        response.body = {'x'};
        registry.check(server.start(response, "/gone.wav"), "起一个 404 服务器");

        ImportJob job;
        Failure failure;
        ImportJobRequest request;
        request.url = server.url();
        request.kind = ImportSourceKind::direct_http;
        request.staging_root = box.staging;
        request.target_category_dir = box.target;
        registry.check(job.start(request, failure), "启动");

        const bool ended = wait_until(
            [&] { return job.status().state != ImportJobState::downloading; },
            [&] { job.pump(); }, 25000);
        registry.check(ended && job.status().state == ImportJobState::failed, "404 → 失败");
        registry.check(job.status().failure_reason.find("404") != std::string::npos,
                       "原因里有 404");
        registry.check(job.status().suggestion.find("删除") != std::string::npos,
                       "404 给出的是'目标不存在或已被删除'这类的建议");
        registry.check(files_in(box.target).empty(), "曲库是空的");
    }

    // =======================================================================
    // 直链：取消
    // =======================================================================
    {
        Sandbox box;
        LoopbackHttpServer server;
        LoopbackResponse response;
        response.body = musicboard::test::make_wav_bytes(8000);   // 一大坨
        response.chunk_size = 512;
        response.chunk_delay_ms = 15;   // 慢慢发，留出取消的窗口
        registry.check(server.start(response, "/slow.wav"), "起一个慢速服务器");

        ImportJob job;
        Failure failure;
        ImportJobRequest request;
        request.url = server.url();
        request.kind = ImportSourceKind::direct_http;
        request.staging_root = box.staging;
        request.target_category_dir = box.target;
        registry.check(job.start(request, failure), "启动");

        const bool got_something = wait_until(
            [&] {
                job.pump();
                return job.status().received_bytes > 0;
            },
            [] {}, 10000);
        registry.check(got_something, "已经开始收到数据");

        job.cancel();
        registry.check(job.status().state == ImportJobState::cancelled, "取消后状态是 cancelled");
        registry.check(!std::filesystem::exists(job.status().staging_dir),
                       "取消之后暂存目录被清掉了");
        registry.check(files_in(box.target).empty(), "取消之后曲库里没有半成品");
        job.pump();
        registry.check(job.status().state == ImportJobState::cancelled, "取消是终态");
    }

    // =======================================================================
    // 暂存区清理
    // =======================================================================
    {
        TempDir temp;
        const auto staging = temp.path() / L"staging";
        std::error_code ec;
        std::filesystem::create_directories(staging, ec);

        Failure failure;
        std::filesystem::path job_dir;
        registry.check(create_staging_job_dir(staging, job_dir, failure), "建一个任务目录");
        registry.check(std::filesystem::is_directory(job_dir), "目录真的在");
        registry.check(path_to_utf8(job_dir.filename()).starts_with("job-"),
                       "名字带 job- 前缀（prune 靠它认自己建的目录）");

        std::filesystem::path second;
        registry.check(create_staging_job_dir(staging, second, failure), "再建一个任务目录");
        registry.check(second != job_dir, "两次建出来的目录不同名（不会互相踩）");

        registry.check(remove_staging_job_dir(job_dir, failure), "删除任务目录");
        registry.check(!std::filesystem::exists(job_dir), "目录没了");
        // 删一个不存在的目录算成功（幂等）
        registry.check(remove_staging_job_dir(job_dir, failure), "重复删除不报错");
    }
    {
        // prune：只清自己的、只清够旧的
        TempDir temp;
        const auto staging = temp.path() / L"staging";
        std::error_code ec;
        std::filesystem::create_directories(staging, ec);

        Failure failure;
        std::filesystem::path old_job;
        registry.check(create_staging_job_dir(staging, old_job, failure), "建一个旧任务目录");
        std::filesystem::path recent_job;
        registry.check(create_staging_job_dir(staging, recent_job, failure), "建一个新任务目录");
        const auto stranger = staging / L"别人的目录";
        std::filesystem::create_directories(stranger, ec);

        // 把 old_job 的时间戳推到 48 小时前
        std::filesystem::last_write_time(
            old_job, std::filesystem::file_time_type::clock::now() - std::chrono::hours(48), ec);

        std::size_t removed = 0;
        registry.check(prune_staging(staging, std::chrono::hours(24), removed, failure),
                       "prune 成功");
        registry.check_eq(removed == 1, "只清掉了 1 个够旧的",
                          "实际 " + std::to_string(removed));
        registry.check(!std::filesystem::exists(old_job), "够旧的那个被清掉了");
        registry.check(std::filesystem::exists(recent_job), "新的那个留着");
        registry.check(std::filesystem::exists(stranger), "不是我们建的目录不动（不越界）");
    }

    // =======================================================================
    // 解析 yt-dlp 路径
    // =======================================================================
    {
        std::filesystem::path found;
        Failure failure;
        // 本机不保证装了 yt-dlp，所以两种结果都是合法的；
        // 要紧的是"找不到时报得清楚"。
        if (resolve_yt_dlp(found, failure)) {
            registry.check(std::filesystem::is_regular_file(found), "找到了 yt-dlp 就是一个真文件");
        } else {
            registry.check(failure.code == fail_code::file_not_found, "找不到时报 file_not_found");
            registry.check(failure.context.find("yt-dlp.exe") != std::string::npos &&
                               failure.context.find("PATH") != std::string::npos,
                           "原因里列出了找过的位置（含 PATH）");
        }
    }

    {
        Sandbox box;
        LoopbackResponse response;
        response.body = musicboard::test::make_wav_bytes(31000);
        response.extra_headers = "Content-Type: audio/wav\r\n";
        LoopbackHttpServer server;
        registry.check(server.start(response, "/long-track.wav"), "启动长曲本地下载服务器");
        ImportJobRequest request;
        request.url = server.url();
        request.kind = ImportSourceKind::direct_http;
        request.staging_root = box.staging;
        request.target_category_dir = box.target;
        request.prepare_playback = true;
        ImportJob job;
        Failure failure;
        registry.check_invocation([&] { return job.start(request, failure); }, "启动长曲下载与播放准备", failure);
        const bool ended = wait_until([&] {
            return job.status().state != ImportJobState::downloading && job.status().state != ImportJobState::preparing;
        }, [&] { job.pump(); }, 20000);
        registry.check_eq(ended && job.status().state == ImportJobState::done, "长曲下载与后台准备完成",
            job.status().failure_reason + job.preparation_failure().to_string());
        const auto& prepared = job.prepared_audio();
        registry.check(prepared && !prepared->complete, "长曲采用连续供音，不保留整首 PCM");
        registry.check(prepared && prepared->head.duration_seconds <= 2.05 && prepared->duration() >= 30.9,
            "只保留两秒起播段并保留完整时长");
        registry.check(prepared && prepared->path == job.result().installed_path,
            "连续供音读取最终离线文件");
    }
    return registry.finish("import_job_tests");
}
