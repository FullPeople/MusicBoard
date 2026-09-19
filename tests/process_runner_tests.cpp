// 子进程运行器测试
//
// 拿 tests/fake_child.cpp 当子进程，所以全程不碰网络、不依赖 yt-dlp 是否安装。
// 这里钉住的正是原版那两个问题所在的地方：
//
//   取消         —— 真的杀掉进程树（而不是只把标志位置回 false）
//   增量读输出   —— 下载过程中就能拿到行，不是下完才拿到
//   卡死判定     —— 一个字都不打时能自己收场，而不是永远等下去
//   退出码       —— 原样回报，不把非零当成"运行器失败"
//   参数引号     —— 空格、引号、反斜杠、中文都要原样送达
//
// 进程的存活与管道里的字节都按真实时间推进，所以这里的等待是真的等。

#include <algorithm>
#include <string>
#include <system_error>
#include <vector>

#include "core/path_utf8.h"
#include "library/process_runner.h"
#include "library/url_classify.h"
#include "tests/test_files.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;
using musicboard::test::TempDir;
using musicboard::test::wait_until;

namespace {

// 跑一整轮：建目录 → 启动 → 泵到结束。返回是否在超时前结束。
bool run_to_completion(ProcessRunner& runner, const std::filesystem::path& fake_child,
                       const std::wstring& mode, const std::filesystem::path& work_dir,
                       Failure& failure, int timeout_ms = 20000) {
    ProcessRunOptions options;
    options.executable = fake_child;
    options.arguments = {path_to_utf8(mode), path_to_utf8(work_dir)};
    options.stall_timeout_seconds = 0.0;   // 默认不判卡死，由具体用例自己设
    if (!runner.start(options, failure)) {
        return false;
    }
    return wait_until([&runner] { return runner.state() != ProcessState::running; },
                      [&runner] { runner.pump(); }, timeout_ms);
}

}  // namespace

int main() {
    Registry registry;

    const std::filesystem::path fake_child = musicboard::test::fake_child_path();
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(fake_child, ec)) {
            registry.skip("全部子进程用例（找不到 fake_child.exe）");
            return registry.finish("process_runner_tests");
        }
    }

    // =======================================================================
    // 启动参数
    // =======================================================================
    {
        ProcessRunner runner;
        Failure failure;
        ProcessRunOptions options;
        // 故意指向一个不存在的东西：要的是"明确报错"，不是静默用默认值
        options.executable = fake_child.parent_path() / L"根本不存在的程序.exe";
        registry.check(!runner.start(options, failure), "可执行文件不存在时启动失败");
        registry.check(failure.code == fail_code::file_not_found, "错误码是 file_not_found");
        registry.check(runner.state() == ProcessState::idle, "启动失败后状态仍是 idle");
    }

    // =======================================================================
    // 正常结束：退出码与输出
    // =======================================================================
    {
        TempDir temp;
        ProcessRunner runner;
        Failure failure;
        registry.check(run_to_completion(runner, fake_child, L"success", temp.path(), failure),
                       "success 模式在超时前结束");
        registry.check_eq(runner.state() == ProcessState::exited, "状态是 exited",
                          process_state_name(runner.state()));
        registry.check(runner.exit_code() == 0, "退出码 0");
        registry.check(failure.empty(), "运行器自身没有失败");
        registry.check(runner.failure_reason().empty(), "没有失败原因");
        registry.check(std::filesystem::exists(temp.path() / L"下载来的曲子.wav"),
                       "子进程真的在指定目录里写出了文件");

        const std::string& output = runner.captured_output();
        registry.check(output.find("[download]") != std::string::npos, "捕到了子进程输出");
        registry.check(output.find("[ExtractAudio]") != std::string::npos, "也捕到了转码那一行");
    }

    // =======================================================================
    // 非零退出码是子进程的结论，不是运行器的失败
    // =======================================================================
    {
        TempDir temp;
        ProcessRunner runner;
        Failure failure;
        registry.check(run_to_completion(runner, fake_child, L"fail", temp.path(), failure),
                       "fail 模式在超时前结束");
        registry.check(runner.state() == ProcessState::exited,
                       "非零退出仍然算 exited（运行器本身没出错）");
        registry.check(runner.exit_code() == 1, "退出码原样回报（1）");
        registry.check(runner.captured_output().find("ERROR") != std::string::npos,
                       "错误输出被完整捕到");
    }

    // =======================================================================
    // 输出是增量读的 —— 这是"进度条会动"的前提
    // =======================================================================
    {
        TempDir temp;
        ProcessRunner runner;
        Failure failure;
        ProcessRunOptions options;
        options.executable = fake_child;
        options.arguments = {path_to_utf8(L"progress"), path_to_utf8(temp.path())};
        options.stall_timeout_seconds = 0.0;
        registry.check(runner.start(options, failure), "启动 progress 模式");

        // 关键点：**在子进程还没结束的时候**就应该已经能拿到好几行进度。
        // 原版是等 OS.execute 返回之后才一次性拿到整段输出 ——
        // 那样进度条永远只在最后跳一下。
        std::vector<double> observed;
        bool saw_intermediate = false;
        bool finished = wait_until(
            [&] {
                double fraction = 0.0;
                for (const std::string& line : runner.take_new_lines()) {
                    if (parse_yt_dlp_progress_line(line, fraction)) {
                        observed.push_back(fraction);
                    }
                }
                // 中途看见过严格在 (0,1) 之间的进度
                for (const double value : observed) {
                    if (value > 0.0 && value < 1.0) {
                        saw_intermediate = true;
                    }
                }
                return runner.state() != ProcessState::running;
            },
            [&] { runner.pump(); }, 20000);

        registry.check(finished, "progress 模式在超时前结束");
        registry.check(saw_intermediate,
                       "下载**过程中**就拿到了中间进度（不是下完才拿到）");
        registry.check(observed.size() >= 8, "至少拿到 8 次进度上报");
        registry.check(observed.size() >= 2 && observed.back() == 1.0, "最后一次是 100%");
        registry.check(runner.take_new_lines().empty(), "取走的行不会重复交付");
    }

    // =======================================================================
    // 卡死判定
    // =======================================================================
    {
        TempDir temp;
        ProcessRunner runner;
        Failure failure;
        ProcessRunOptions options;
        options.executable = fake_child;
        options.arguments = {path_to_utf8(L"silent"), path_to_utf8(temp.path())};
        // silent 模式一个字都不打然后睡 30 秒。给它半秒就判定卡死。
        options.stall_timeout_seconds = 0.5;
        registry.check(runner.start(options, failure), "启动 silent 模式");

        const bool ended = wait_until(
            [&] { return runner.state() != ProcessState::running; }, [&] { runner.pump(); }, 15000);
        registry.check(ended, "卡死判定生效（没有永远等下去）");
        registry.check_eq(runner.state() == ProcessState::failed, "状态是 failed",
                          process_state_name(runner.state()));
        registry.check_eq(runner.failure_reason().find("没有任何输出") != std::string::npos,
                          "原因说明了是长时间没有输出", runner.failure_reason());
        // 判定卡死之后必须把那个还睡着的子进程杀掉。
        // exit_code 为 STILL_ACTIVE(259) 就说明它其实还活着 —— 那正是要防的事。
        registry.check(runner.exit_code() != 259, "那个子进程确实被终止了（不是还活着）");
    }

    // =======================================================================
    // 取消：真的杀掉，而且幂等
    // =======================================================================
    {
        TempDir temp;
        ProcessRunner runner;
        Failure failure;
        ProcessRunOptions options;
        options.executable = fake_child;
        options.arguments = {path_to_utf8(L"hang"), path_to_utf8(temp.path())};
        options.stall_timeout_seconds = 0.0;   // hang 一直在输出，卡死判定不会触发
        registry.check(runner.start(options, failure), "启动 hang 模式");

        // 等它确实跑起来并且有输出
        const bool started = wait_until([&] { return !runner.captured_output().empty(); },
                                        [&] { runner.pump(); }, 10000);
        registry.check(started, "hang 模式开始输出了");
        registry.check(runner.state() == ProcessState::running, "此刻仍在运行");

        runner.cancel();
        registry.check(runner.state() == ProcessState::cancelled, "取消之后状态是 cancelled");
        registry.check(runner.exit_code() != 0, "退出码不是 0（进程是被杀的）");
        // cancel() 会等最多 5 秒并复查退出码。原因非空就说明它其实没死 ——
        // "以为取消了、其实还在下"正是原版的问题，所以这一条要钉住。
        registry.check_eq(runner.failure_reason().empty(), "子进程真的退出了（不是还在跑）",
                          runner.failure_reason());

        // 幂等：再取消几次不该出问题，也不该把状态改成别的东西
        runner.cancel();
        runner.cancel();
        registry.check(runner.state() == ProcessState::cancelled, "重复取消仍是 cancelled");

        // 取消之后不该再有新输出
        const std::size_t size_after_cancel = runner.captured_output().size();
        runner.pump();
        registry.check(runner.captured_output().size() == size_after_cancel,
                       "取消之后不再有新输出");
    }
    {
        // 进程已经自然结束时调用 cancel()，不该把"下载完了"改写成"被取消了"
        TempDir temp;
        ProcessRunner runner;
        Failure failure;
        registry.check(run_to_completion(runner, fake_child, L"success", temp.path(), failure),
                       "先正常跑完");
        registry.check(runner.state() == ProcessState::exited, "状态是 exited");
        runner.cancel();
        registry.check(runner.state() == ProcessState::exited,
                       "对已结束的进程调 cancel 不会覆盖真实结局");
    }

    // =======================================================================
    // 参数引号与 UTF-8
    // =======================================================================
    {
        TempDir temp;
        ProcessRunner runner;
        Failure failure;
        ProcessRunOptions options;
        options.executable = fake_child;
        // 这些参数各自对应一类会把命令行拼坏的东西
        const std::string with_space = "带 空格 的 名字";
        const std::string with_quote = "引号\"在里面";
        const std::string with_backslash = "结尾反斜杠\\";
        const std::string with_chinese = "深水城 主题曲（中文）";
        const std::string empty_argument;
        options.arguments = {path_to_utf8(L"echo_args"), with_space, with_quote, with_backslash,
                             with_chinese, empty_argument};
        options.stall_timeout_seconds = 0.0;
        registry.check(runner.start(options, failure), "启动 echo_args 模式");
        const bool ended = wait_until(
            [&] { return runner.state() != ProcessState::running; }, [&] { runner.pump(); }, 15000);
        registry.check_eq(ended, "echo_args 结束", runner.failure_reason());

        // 用拆好的行来比，而不是在原始输出里 find 子串：
        // 子进程的 stdout 是文本模式，CRT 会把 '\n' 写成 "\r\n"，
        // 直接搜 "...\n" 永远搜不到。行本身也更精确。
        const std::vector<std::string> lines = runner.take_new_lines();
        const auto has_line = [&lines](const std::string& wanted) {
            return std::find(lines.begin(), lines.end(), wanted) != lines.end();
        };

        registry.check(has_line("ARG: " + with_space), "带空格的参数原样送达");
        registry.check(has_line("ARG: " + with_quote), "带双引号的参数原样送达");
        registry.check(has_line("ARG: " + with_backslash),
                       "以反斜杠结尾的参数原样送达（没被当成转义符吃掉）");
        registry.check(has_line("ARG: " + with_chinese),
                       "中文参数原样送达（UTF-8 → 宽字符 → 命令行）");
        registry.check(has_line("ARG: "), "空参数原样送达");
    }

    // =======================================================================
    // 输出累积上限：超限要报出来，不能悄悄截断也不能爆内存
    // =======================================================================
    {
        TempDir temp;
        ProcessRunner runner;
        Failure failure;
        ProcessRunOptions options;
        options.executable = fake_child;
        options.arguments = {path_to_utf8(L"spam"), path_to_utf8(temp.path())};
        options.stall_timeout_seconds = 0.0;
        options.maximum_output_bytes = 4096;   // 故意调得很小
        registry.check(runner.start(options, failure), "启动 spam 模式");
        const bool ended = wait_until(
            [&] { return runner.state() != ProcessState::running; }, [&] { runner.pump(); }, 20000);
        registry.check(ended, "spam 模式结束（超过上限也照样读，子进程不会写阻塞）");
        registry.check(runner.output_truncated(), "超限这件事被明确报出来了");
        registry.check(runner.captured_output().size() <= 8192,
                       "累积输出没有超过上限的量级（内存不会爆）");
    }

    // =======================================================================
    // 工作目录
    // =======================================================================
    {
        TempDir temp;
        const std::filesystem::path work = temp.path() / L"工作目录";
        std::error_code ec;
        std::filesystem::create_directories(work, ec);

        ProcessRunner runner;
        Failure failure;
        ProcessRunOptions options;
        options.executable = fake_child;
        options.arguments = {path_to_utf8(L"success"), path_to_utf8(work)};
        options.working_directory = work;
        options.stall_timeout_seconds = 0.0;
        registry.check(runner.start(options, failure), "带工作目录启动");
        const bool ended = wait_until(
            [&] { return runner.state() != ProcessState::running; }, [&] { runner.pump(); }, 15000);
        registry.check(ended && runner.exit_code() == 0, "带工作目录时正常运行");
        registry.check(std::filesystem::exists(work / L"下载来的曲子.wav"), "文件落在指定目录里");
    }

    // =======================================================================
    // 参数不是合法 UTF-8 → 明确拒绝，不静默丢参数
    // =======================================================================
    {
        TempDir temp;
        ProcessRunner runner;
        Failure failure;
        ProcessRunOptions options;
        options.executable = fake_child;
        std::string invalid = "前缀";
        invalid.push_back(static_cast<char>(0xFF));   // 非法 UTF-8 字节
        invalid += "后缀";
        options.arguments = {path_to_utf8(L"echo_args"), invalid};
        registry.check(!runner.start(options, failure), "非法 UTF-8 参数被拒");
        registry.check(failure.code == fail_code::invalid_argument, "错误码是 invalid_argument");
    }

    return registry.finish("process_runner_tests");
}
