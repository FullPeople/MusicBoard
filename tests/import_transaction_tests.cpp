// 导入事务测试
//
// 事务的三步各测一遍：
//   1. 校验来源 —— 路径逃逸、符号链接、不存在的文件
//   2. 校验内容 —— 不是音频、改名成 .mp3 的可执行文件、没有扩展名
//   3. 原子发布 —— 重名避让、失败不留半成品
//
// 关于**没有覆盖到的分支**（写在最前面，免得后来人以为漏了）：
// import_transaction.cpp 里有两个兜底分支在自动化测试里到不了 ——
//   · 跨盘复制路径（同盘 rename 就够了，测试目录永远在同一个卷上）
//   · 发布之后重新探测失败的回滚（发布前后的 probe 读的是同一份字节，
//     除非发布本身写坏了数据，否则不可能一个过一个不过）
// 这两条由代码审阅 + 断点观察覆盖，不假装测过。
//
// 符号链接那一条依赖账户特权（需要开发者模式或管理员），
// 拿不到就 SKIP —— 测试的成败不该取决于跑测试的账户。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>

#include "core/failure.h"
#include "core/path_utf8.h"
#include "library/import_transaction.h"
#include "tests/test_files.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;
using musicboard::test::TempDir;

namespace {

// 目录里的普通文件个数。用来断言"失败之后什么都没留下"。
std::size_t file_count(const std::filesystem::path& directory) {
    std::error_code ec;
    std::size_t count = 0;
    std::filesystem::directory_iterator it(directory, ec);
    if (ec) {
        return 0;
    }
    for (const auto& entry : it) {
        if (entry.is_regular_file(ec)) {
            ++count;
        }
    }
    return count;
}

// 一个装好目录结构的沙盒：staging/ 用于"下载完的暂存文件"，library/BGM/战斗 是目标
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

    [[nodiscard]] ImportRequest request(std::filesystem::path staged) const {
        ImportRequest r;
        r.staged_file = std::move(staged);
        r.staging_root = staging;
        r.target_category_dir = target;
        return r;
    }
};

}  // namespace

int main() {
    Registry registry;

    // =======================================================================
    // 1. 顺利路径
    // =======================================================================
    {
        Sandbox box;
        const auto staged = box.staging / L"新曲子.wav";
        registry.check(musicboard::test::write_test_wav(staged, 200), "准备暂存音频文件");

        ImportRequest request = box.request(staged);
        ImportResult result;
        Failure failure;
        const bool ok = import_staged_file(request, result, failure);

        registry.check_eq(ok, "正常导入成功", failure.to_string());
        registry.check(result.installed_path == box.target / L"新曲子.wav", "落点就是目标分类下同名文件");
        registry.check(result.final_stem == "新曲子", "最终主干是中文原名");
        registry.check(!result.renamed_to_avoid_collision, "没有发生重名避让");
        registry.check_eq(result.duration_seconds > 0.1 && result.duration_seconds < 0.5,
                          "返回的时长约等于 0.2 秒",
                          std::to_string(result.duration_seconds) + " 秒");
        registry.check(!std::filesystem::exists(staged), "暂存文件已经被搬走（不是复制留一份）");
        registry.check(file_count(box.target) == 1, "目标目录里正好一个文件");
    }

    // =======================================================================
    // 2. 校验来源
    // =======================================================================
    {
        Sandbox box;

        // 路径逃逸：暂存区之外的合法音频
        const auto outside = box.temp.path() / L"逃逸.wav";
        registry.check(musicboard::test::write_test_wav(outside, 200), "在暂存区之外放一个音频");

        ImportRequest request = box.request(outside);
        ImportResult result;
        Failure failure;
        const bool ok = import_staged_file(request, result, failure);

        registry.check(!ok, "暂存区之外的文件被拒");
        registry.check(failure.code == fail_code::invalid_argument, "错误码是 invalid_argument");
        registry.check(failure.context.find("逃逸") != std::string::npos, "错误信息说清了是路径逃逸");
        registry.check(file_count(box.target) == 0, "被拒之后目标目录仍然是空的");
        registry.check(std::filesystem::exists(outside), "源文件没有被删掉");
    }
    {
        Sandbox box;
        const auto staged = box.staging / L"曲子.wav";
        registry.check(musicboard::test::write_test_wav(staged, 200), "准备音频");

        ImportRequest request = box.request(staged);
        request.staging_root.clear();   // 没给暂存区根
        ImportResult result;
        Failure failure;
        registry.check(!import_staged_file(request, result, failure) &&
                           failure.code == fail_code::invalid_argument,
                       "没指定暂存区根目录时拒绝导入（否则逃逸检查形同虚设）");
    }
    {
        Sandbox box;
        ImportRequest request = box.request(box.staging / L"根本不存在.wav");
        ImportResult result;
        Failure failure;
        registry.check(!import_staged_file(request, result, failure) &&
                           failure.code == fail_code::file_not_found,
                       "暂存文件不存在时报 file_not_found");
    }
    {
        // 符号链接：暂存区里的一个链接可以指向任意位置，
        // 所以"落在暂存区之内"必须先把链接本身挡掉。
        Sandbox box;
        const auto real_audio = box.temp.path() / L"真实文件.wav";
        registry.check(musicboard::test::write_test_wav(real_audio, 200), "准备链接目标");

        const auto link = box.staging / L"链接.wav";
        // 先试标准库；它失败时再用带"无需特权"标志的原生调用试一次 ——
        // 开了开发者模式的机器上前者也可能失败，而后者能成功。
        std::error_code ec;
        std::filesystem::create_symlink(real_audio, link, ec);
        if (ec) {
            static_cast<void>(::CreateSymbolicLinkW(link.c_str(), real_audio.c_str(),
                                                   0x2 /* ALLOW_UNPRIVILEGED_CREATE */));
        }

        if (!std::filesystem::exists(link, ec)) {
            // 建符号链接需要开发者模式或管理员权限。没有就跳过而不是失败 ——
            // 测试的成败不该取决于跑测试的账户有没有这个特权。
            registry.skip("符号链接被拒（本机没有创建符号链接的权限）");
        } else {
            ImportRequest request = box.request(link);
            ImportResult result;
            Failure failure;
            registry.check(!import_staged_file(request, result, failure) &&
                               failure.code == fail_code::invalid_argument,
                           "暂存区里的符号链接被拒");
            registry.check(file_count(box.target) == 0, "被拒之后目标目录仍然是空的");
            registry.check(std::filesystem::exists(real_audio), "链接指向的真实文件没有被删掉");
        }
    }
    {
        Sandbox box;
        const auto staged = box.staging / L"好音频.wav";
        registry.check(musicboard::test::write_test_wav(staged, 200), "准备音频");

        ImportRequest request = box.request(staged);
        request.target_category_dir = box.library / L"BGM" / L"不存在的分类";
        ImportResult result;
        Failure failure;
        registry.check(!import_staged_file(request, result, failure) &&
                           failure.code == fail_code::file_not_found,
                       "目标分类目录不存在时报 file_not_found（不悄悄新建）");
        registry.check(std::filesystem::exists(staged), "源文件还在");
    }

    // =======================================================================
    // 3. 校验内容
    // =======================================================================
    {
        Sandbox box;
        // 改名成 .mp3 的文本文件 —— 原版会照单全收
        const auto fake = box.staging / L"伪装.mp3";
        registry.check(musicboard::test::write_text(fake, "这不是音频，只是一行字"), "准备伪装文件");

        ImportRequest request = box.request(fake);
        ImportResult result;
        Failure failure;
        registry.check(!import_staged_file(request, result, failure) &&
                           failure.code == fail_code::unsupported_format,
                       "改名成 .mp3 的非音频被拒（不需要专门认 PE 头，解不出来的就不是音频）");
        registry.check(file_count(box.target) == 0, "被拒之后没有半成品落进曲库");
        registry.check(std::filesystem::exists(fake), "暂存文件保留在原地，交给上层决定怎么处理");
    }
    {
        Sandbox box;
        // 没有扩展名、但内容是合法 WAV —— 正好验证"按魔数判容器，不按扩展名"
        const auto no_ext = box.staging / L"没有扩展名";
        registry.check(musicboard::test::write_bytes(no_ext, musicboard::test::make_wav_bytes(200)),
                       "准备一个没有扩展名的合法 WAV");

        ImportRequest request = box.request(no_ext);
        request.preferred_stem = "补上名字";
        ImportResult result;
        Failure failure;
        registry.check(!import_staged_file(request, result, failure) &&
                           failure.code == fail_code::invalid_argument,
                       "没有扩展名时拒绝导入（无法确定目标文件名，也不猜）");
        registry.check(file_count(box.target) == 0, "目标目录仍然是空的");
    }
    {
        Sandbox box;
        const auto empty_file = box.staging / L"空文件.ogg";
        registry.check(musicboard::test::write_text(empty_file, ""), "准备一个 0 字节的 .ogg");

        ImportRequest request = box.request(empty_file);
        ImportResult result;
        Failure failure;
        registry.check(!import_staged_file(request, result, failure) &&
                           failure.code == fail_code::unsupported_format,
                       "0 字节文件被拒");
    }

    // =======================================================================
    // 4. 重名永不覆盖
    // =======================================================================
    {
        Sandbox box;
        // 目标里先放一个 0.4 秒的同名文件（和要导入的 0.2 秒版本大小不同，便于区分）
        const auto existing = box.target / L"重名.wav";
        registry.check(musicboard::test::write_test_wav(existing, 400), "目标里先放一个同名文件");
        const auto existing_size = std::filesystem::file_size(existing);

        const auto staged = box.staging / L"重名.wav";
        registry.check(musicboard::test::write_test_wav(staged, 200), "准备要导入的同名文件");

        ImportRequest request = box.request(staged);
        ImportResult result;
        Failure failure;
        const bool ok = import_staged_file(request, result, failure);

        registry.check_eq(ok, "导入成功（改名而不是失败）", failure.to_string());
        registry.check(result.renamed_to_avoid_collision, "结果里标明了发生过重名避让");
        registry.check(result.installed_path == box.target / L"重名(2).wav", "落点是 重名(2).wav");
        registry.check(result.final_stem == "重名(2)", "最终主干带上了序号");
        registry.check(std::filesystem::file_size(existing) == existing_size, "原有文件大小没变（没被覆盖）");

        // 再来一次，应该得到 (3)
        const auto staged2 = box.staging / L"重名.wav";
        registry.check(musicboard::test::write_test_wav(staged2, 200), "再准备一个同名文件");
        ImportRequest request2 = box.request(staged2);
        ImportResult result2;
        Failure failure2;
        registry.check(import_staged_file(request2, result2, failure2) &&
                           result2.installed_path == box.target / L"重名(3).wav",
                       "第二次导入得到 重名(3).wav");
        registry.check(file_count(box.target) == 3, "目标目录里现在有三个文件");
    }

    // =======================================================================
    // 5. 名字净化
    // =======================================================================
    {
        Sandbox box;
        const auto staged = box.staging / L"带斜杠的标题.wav";
        registry.check(musicboard::test::write_test_wav(staged, 200), "准备音频");

        ImportRequest request = box.request(staged);
        // 从 URL 标题来的名字里出现 "/" 是常态（"歌手/曲名"）
        request.preferred_stem = "歌手/曲名:draft";
        ImportResult result;
        Failure failure;
        const bool ok = import_staged_file(request, result, failure);

        registry.check_eq(ok, "非法字符的名字被净化后导入成功", failure.to_string());
        registry.check(result.final_stem == "歌手_曲名_draft", "斜杠与冒号净化成下划线");
        registry.check(result.installed_path == box.target / L"歌手_曲名_draft.wav", "落点正确");
        registry.check(file_count(box.target) == 1, "没有因为斜杠多建出一层目录");
        registry.check(!std::filesystem::exists(box.target / L"歌手"), "目标下没有多出 \"歌手\" 子目录");
    }
    {
        Sandbox box;
        const auto staged = box.staging / L"原名保留.wav";
        registry.check(musicboard::test::write_test_wav(staged, 200), "准备音频");

        ImportRequest request = box.request(staged);
        request.preferred_stem.clear();   // 不指定，应沿用暂存文件的主干
        ImportResult result;
        Failure failure;
        registry.check(import_staged_file(request, result, failure) &&
                           result.final_stem == "原名保留",
                       "不指定名字时沿用暂存文件的主干");
    }

    // =======================================================================
    // 6. 真实曲库目录同样适用
    // =======================================================================
    {
        Sandbox box;
        // 分类名本身带中文与空格：整条链路（路径转 UTF-8、MoveFileExW、重新 probe）
        // 都得在非 ASCII 路径上走通。这是本项目最怕出问题的地方。
        const auto nested = box.library / L"BGM" / L"城镇 白天" / L"子分类（测试）";
        std::error_code ec;
        std::filesystem::create_directories(nested, ec);

        const auto staged = box.staging / L"深水城 主题曲.wav";
        registry.check(musicboard::test::write_test_wav(staged, 200), "准备中文名音频");

        ImportRequest request = box.request(staged);
        request.target_category_dir = nested;
        ImportResult result;
        Failure failure;
        const bool ok = import_staged_file(request, result, failure);

        registry.check_eq(ok, "导入到深层中文目录成功", failure.to_string());
        registry.check(result.installed_path == nested / L"深水城 主题曲.wav", "落点正确");
        registry.check(result.duration_seconds > 0.1, "发布后重新探测拿到了时长");
    }

    {
        Sandbox box;
        const auto staged = box.staging / L"来源片段.wav";
        registry.check(musicboard::test::write_test_wav(staged, 200), "准备时长远短于来源的片段");
        auto request = box.request(staged);
        request.source_duration_seconds = 197.0;
        ImportResult result;
        Failure failure;
        const bool ok = import_staged_file(request, result, failure);
        registry.check_eq(ok, "可解码的片段允许导入", failure.to_string());
        registry.check(result.content_incomplete, "导入结果明确标注片段");
        registry.check(result.installed_path == box.target / L"来源片段 (试听片段).wav",
            "离线文件名保留片段标示，重新扫描也可见");
    }
    return registry.finish("import_transaction_tests");
}
