// 曲库扫描测试
//
// 分两段：
//   A. 合成曲库 —— 目录结构的每条规则各测一遍（只扫一层、忽略隐藏项、忽略非音频、
//      空分类保留、失败逐条记成 warning、可取消、两次扫描顺序一致）。
//   B. 真实曲库 —— 由 MUSICBOARD_TEST_LIBRARY 指定。没有就 SKIP，不是 FAIL。
//      这一段盯的是本项目最在意的一条：**扫描只 probe，不解码**。
//      同时把耗时和内存增量打出来 —— 那才是这条边界存在的理由。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "core/path_utf8.h"
#include "library/library_scanner.h"
#include "tests/test_files.h"
#include "tests/test_harness.h"

#pragma comment(lib, "psapi.lib")

using namespace musicboard;
using musicboard::test::Registry;
using musicboard::test::TempDir;

namespace {

// 当前进程的工作集（字节）。用来验证"扫描不解码"这条边界。
std::size_t working_set_bytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)) == FALSE) {
        return 0;
    }
    return static_cast<std::size_t>(counters.WorkingSetSize);
}

std::filesystem::path library_root_from_env() {
    // 必须用宽字符 API：曲库路径里有中文，getenv 会按 936 代码页把它打乱。
    wchar_t buffer[32768]{};
    const DWORD written = ::GetEnvironmentVariableW(L"MUSICBOARD_TEST_LIBRARY", buffer,
                                                   static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(buffer);
}

// 分类名列表 / 曲目名列表，用来做集合与顺序断言
std::vector<std::string> category_names(const std::vector<LibraryCategory>& categories) {
    std::vector<std::string> names;
    names.reserve(categories.size());
    for (const auto& category : categories) {
        names.push_back(category.name);
    }
    return names;
}

std::vector<std::string> track_names(const std::vector<LibraryCategory>& categories,
                                     std::string_view category) {
    for (const auto& item : categories) {
        if (item.name == category) {
            std::vector<std::string> names;
            names.reserve(item.tracks.size());
            for (const auto& track : item.tracks) {
                names.push_back(track.name);
            }
            return names;
        }
    }
    return {};
}

bool is_sorted(const std::vector<std::string>& names) {
    return std::is_sorted(names.begin(), names.end());
}

// 独立于扫描器，按同样的目录约定数一遍"界面上应该能看到几个音频文件"。
//
// 这个数字必须等于 snapshot.candidate_count。它检验的是一条硬要求：
// **曲库里没有一个音频文件是看不见的** —— 磁盘上明明在、界面上永远看不到、
// 也没有任何提示，正是这一层最要避免的失败方式。
// 实测真实曲库就藏着一个（BGM/背景恐怖音2.mp3，直接躺在栏目根下）。
std::size_t count_expected_candidates(const std::filesystem::path& root) {
    std::error_code ec;
    const bool has_bgm = std::filesystem::is_directory(root / L"BGM", ec);
    const bool has_sfx = std::filesystem::is_directory(root / L"SFX", ec);
    std::size_t count = 0;

    const auto is_audio = [](const std::filesystem::path& path) {
        std::string extension = path_to_utf8(path.extension());
        if (extension.size() <= 1) {
            return false;
        }
        extension = extension.substr(1);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return is_supported_audio_extension(extension);
    };
    const auto is_visible = [](const std::filesystem::path& path) {
        const std::string name = path_to_utf8(path.filename());
        return !name.empty() && name.front() != '.';
    };

    // 一个分类目录里（只一层）
    const auto count_level = [&](const std::filesystem::path& folder) {
        std::error_code level_ec;
        std::filesystem::directory_iterator it(folder, level_ec);
        if (level_ec) {
            return;
        }
        for (const auto& entry : it) {
            if (!is_visible(entry.path()) || !entry.is_regular_file(level_ec)) {
                continue;
            }
            if (is_audio(entry.path())) {
                ++count;
            }
        }
    };

    const auto visit_section = [&](const std::filesystem::path& base, std::string_view excluded) {
        std::error_code section_ec;
        std::filesystem::directory_iterator it(base, section_ec);
        if (section_ec) {
            return;
        }
        std::vector<std::filesystem::path> subfolders;
        for (const auto& entry : it) {
            if (!is_visible(entry.path())) {
                continue;
            }
            if (!excluded.empty() && path_to_utf8(entry.path().filename()) == excluded) {
                continue;
            }
            if (entry.is_directory(section_ec)) {
                subfolders.push_back(entry.path());
            }
        }
        for (const auto& folder : subfolders) {
            count_level(folder);
        }
        // 栏目根下散落的文件也要算 —— 它们进"默认"分类
        count_level(base);
    };

    visit_section(has_bgm ? root / L"BGM" : root,
                  has_bgm ? std::string_view{} : std::string_view("SFX"));
    if (has_sfx) {
        visit_section(root / L"SFX", {});
    }
    return count;
}

void build_synthetic_library(const std::filesystem::path& root, Registry& registry) {
    std::error_code ec;
    const auto bgm = root / L"BGM";
    const auto battle = bgm / L"战斗";
    const auto town = bgm / L"城镇";
    const auto empty_category = bgm / L"空分类";
    const auto sfx = root / L"SFX";
    const auto attack = sfx / L"攻击";
    for (const auto& directory : {bgm, battle, town, empty_category, sfx, attack}) {
        std::filesystem::create_directories(directory, ec);
    }

    bool prepared = true;
    prepared &= musicboard::test::write_test_wav(battle / L"B 曲目.wav", 200);
    prepared &= musicboard::test::write_test_wav(battle / L"A 曲目.wav", 200);
    prepared &= musicboard::test::write_test_wav(battle / L".隐藏曲目.wav", 200);
    prepared &= musicboard::test::write_test_wav(town / L"城镇主题.wav", 200);
    prepared &= musicboard::test::write_test_wav(attack / L"挥砍.wav", 200);
    registry.check(prepared, "合成曲库：音频文件写入成功");

    // 这四样都必须被忽略，而且不能被当成失败
    static_cast<void>(musicboard::test::write_text(battle / L"说明.txt", "不是音频"));
    static_cast<void>(musicboard::test::write_text(root / L"README.md", "根目录的说明文件"));
    std::filesystem::create_directories(battle / L"子目录", ec);
    static_cast<void>(musicboard::test::write_test_wav(battle / L"子目录" / L"不该被扫到.wav", 200));

    // 这一个必须被计成失败并写进 warnings
    static_cast<void>(musicboard::test::write_text(attack / L"坏文件.mp3", "封面图误存成了 mp3"));
}

}  // namespace

int main() {
    Registry registry;

    // =======================================================================
    // 扩展名白名单
    // =======================================================================
    {
        int supported = 0;
        for (const char* extension : {"ogg", "mp3", "wav", "flac", "m4a", "aac", "wma"}) {
            if (is_supported_audio_extension(extension)) {
                ++supported;
            }
        }
        registry.check(supported == 7, "7 种支持的扩展名全部识别");
        registry.check(!is_supported_audio_extension("txt"), "txt 不在白名单");
        registry.check(!is_supported_audio_extension("mp4"), "mp4 不在白名单（视频不扫）");
        registry.check(!is_supported_audio_extension(""), "空扩展名不在白名单");
        registry.check(!is_supported_audio_extension("OGG"), "大写视为不在白名单（调用方负责转小写）");
    }

    // =======================================================================
    // A. 合成曲库
    // =======================================================================
    {
        TempDir temp;
        build_synthetic_library(temp.path(), registry);

        ScanOptions options;
        options.root = temp.path();

        std::vector<std::pair<std::size_t, std::size_t>> progress_calls;
        options.on_progress = [&progress_calls](std::size_t done, std::size_t total,
                                                const std::filesystem::path&) {
            progress_calls.emplace_back(done, total);
        };

        LibrarySnapshot snapshot;
        Failure failure;
        const bool ok = scan_library(options, snapshot, failure);
        registry.check_eq(ok, "合成曲库扫描成功", failure.to_string());

        // ---- BGM 分类 ----
        std::vector<std::string> bgm_names = category_names(snapshot.bgm);
        registry.check(bgm_names.size() == 3, "BGM 扫出 3 个分类（含空分类）");
        registry.check(std::find(bgm_names.begin(), bgm_names.end(), "空分类") != bgm_names.end(),
                       "空分类被保留（用户刚建的分类要看得见）");
        registry.check(is_sorted(bgm_names), "分类顺序稳定（按 UTF-8 字节序）");

        const std::vector<std::string> battle_tracks = track_names(snapshot.bgm, "战斗");
        registry.check(battle_tracks.size() == 2, "战斗分类只有 2 首（隐藏文件与非音频都排除）");
        registry.check(std::find(battle_tracks.begin(), battle_tracks.end(), ".隐藏曲目") ==
                           battle_tracks.end(),
                       "以点开头的文件被忽略");
        registry.check(std::find(battle_tracks.begin(), battle_tracks.end(), "说明") ==
                           battle_tracks.end(),
                       "txt 文件被忽略");
        registry.check(std::find(battle_tracks.begin(), battle_tracks.end(), "不该被扫到") ==
                           battle_tracks.end(),
                       "分类下的子目录不被递归（只扫一层）");
        registry.check(is_sorted(battle_tracks), "曲目顺序稳定");

        const std::vector<std::string> empty_tracks = track_names(snapshot.bgm, "空分类");
        registry.check(empty_tracks.empty(), "空分类确实没有曲目");

        // ---- SFX ----
        const std::vector<std::string> sfx_names = category_names(snapshot.sfx);
        registry.check(sfx_names.size() == 1 && sfx_names.front() == "攻击", "SFX 扫出 1 个分类");
        const std::vector<std::string> attack_tracks = track_names(snapshot.sfx, "攻击");
        registry.check(attack_tracks.size() == 1, "攻击分类只有 1 首成功（坏文件被排除）");

        // ---- 计数与 warnings ----
        registry.check_eq(snapshot.candidate_count == 5, "候选 5 个（BGM 3 + SFX 2）",
                          std::to_string(snapshot.candidate_count));
        registry.check_eq(snapshot.loaded_count == 4, "成功 4 个", std::to_string(snapshot.loaded_count));
        registry.check_eq(snapshot.failed_count == 1, "失败 1 个", std::to_string(snapshot.failed_count));
        registry.check(snapshot.loaded_count + snapshot.failed_count == snapshot.candidate_count,
                       "成功 + 失败 == 候选（没有既不算成功也不算失败的文件）");
        registry.check(snapshot.total_tracks() == 4, "总曲目数 4");
        registry.check(!snapshot.cancelled, "未取消标志为假");

        registry.check(snapshot.warnings.size() == 1, "恰好 1 条 warning");
        bool warning_names_file = false;
        bool warning_has_reason = false;
        for (const std::string& warning : snapshot.warnings) {
            if (warning.find("坏文件.mp3") != std::string::npos) {
                warning_names_file = true;
            }
            if (warning.find("audio.") != std::string::npos) {
                warning_has_reason = true;   // 带错误码，不是一句"加载失败"
            }
        }
        registry.check(warning_names_file, "warning 里点名了是哪个文件");
        registry.check(warning_has_reason, "warning 里带上了错误码，而不是一句空话");

        // ---- 进度回调 ----
        registry.check(!progress_calls.empty(), "进度回调被调用过");
        registry.check(progress_calls.back().second == 5 && progress_calls.back().first == 4,
                       "最后一次进度回调给出最终计数 (4/5)");
        registry.check(std::all_of(progress_calls.begin(), progress_calls.end(),
                                   [](const auto& call) { return call.first <= 5; }),
                       "进度不会超过总数");

        // ---- 确定性 ----
        LibrarySnapshot again;
        Failure failure_again;
        registry.check(scan_library(options, again, failure_again), "第二次扫描成功");
        registry.check(category_names(again.bgm) == bgm_names &&
                           track_names(again.bgm, "战斗") == battle_tracks,
                       "两次扫描得到完全相同的顺序（界面上曲目不会自己跳）");
    }

    // =======================================================================
    // A2. 取消
    // =======================================================================
    {
        TempDir temp;
        build_synthetic_library(temp.path(), registry);

        ScanOptions options;
        options.root = temp.path();

        int seen = 0;
        options.should_cancel = [&seen]() {
            ++seen;
            return true;   // 一开始就要求取消
        };

        LibrarySnapshot snapshot;
        Failure failure;
        const bool ok = scan_library(options, snapshot, failure);
        registry.check(ok && snapshot.cancelled, "立即取消：返回成功且 cancelled 为真");
        registry.check(snapshot.bgm.empty() && snapshot.sfx.empty(), "取消后没有交出半份分类列表");
        registry.check(failure.empty(), "取消不算失败（没有错误码）");
    }
    {
        TempDir temp;
        build_synthetic_library(temp.path(), registry);

        ScanOptions options;
        options.root = temp.path();

        std::size_t checked = 0;
        options.should_cancel = [&checked]() {
            // 放过去 2 个文件再取消。取消判定在**每个文件开头**问一次。
            return ++checked > 2;
        };

        LibrarySnapshot snapshot;
        Failure failure;
        registry.check(scan_library(options, snapshot, failure) && snapshot.cancelled,
                       "扫两个文件之后取消：cancelled 为真");
        registry.check(snapshot.loaded_count <= 2, "取消时最多已经探了 2 个文件");
        registry.check(snapshot.bgm.empty(), "取消后不交出没扫完的分类");
    }

    // =======================================================================
    // A3. 目录约定的三条退让规则
    // =======================================================================
    {
        // 有分类子目录，但栏目根下也散着音频文件 —— 那些文件不能消失。
        // 实测真实曲库就有一个这种文件（BGM/背景恐怖音2.mp3）。
        TempDir temp;
        std::error_code ec;
        std::filesystem::create_directories(temp.path() / L"BGM" / L"战斗", ec);
        static_cast<void>(
            musicboard::test::write_test_wav(temp.path() / L"BGM" / L"战斗" / L"分类里的.wav", 200));
        static_cast<void>(musicboard::test::write_test_wav(temp.path() / L"BGM" / L"散落的.wav", 200));

        ScanOptions options;
        options.root = temp.path();
        LibrarySnapshot snapshot;
        Failure failure;
        registry.check(scan_library(options, snapshot, failure), "散落文件场景扫描成功");

        const std::vector<std::string> names = category_names(snapshot.bgm);
        registry.check(names.size() == 2, "散落的文件多出一个分类，而不是被丢掉");
        registry.check(std::find(names.begin(), names.end(), std::string(default_category_name)) !=
                           names.end(),
                       "那个分类叫\"默认\"");
        registry.check(track_names(snapshot.bgm, "默认").size() == 1, "\"默认\"里有那个散落的文件");
        registry.check(track_names(snapshot.bgm, "战斗").size() == 1, "原有分类不受影响");
        registry.check(snapshot.total_tracks() == 2, "两个文件都在，一个都没丢");
    }
    {
        // 没有散落文件时，不该凭空多出一个空的"默认"分类
        TempDir temp;
        std::error_code ec;
        std::filesystem::create_directories(temp.path() / L"BGM" / L"战斗", ec);
        static_cast<void>(
            musicboard::test::write_test_wav(temp.path() / L"BGM" / L"战斗" / L"就这一首.wav", 200));

        ScanOptions options;
        options.root = temp.path();
        LibrarySnapshot snapshot;
        Failure failure;
        registry.check(scan_library(options, snapshot, failure), "密集分类场景扫描成功");
        registry.check(category_names(snapshot.bgm).size() == 1,
                       "没有散落文件时不凭空多出\"默认\"分类");
    }
    {
        // 根下没有 BGM 子目录 → 把根本身当 BGM，且必须排除 SFX 子目录
        TempDir temp;
        std::error_code ec;
        const auto sfx = temp.path() / L"SFX" / L"环境";
        std::filesystem::create_directories(sfx, ec);
        static_cast<void>(musicboard::test::write_test_wav(temp.path() / L"散放的曲子.wav", 200));
        static_cast<void>(musicboard::test::write_test_wav(sfx / L"雨声.wav", 200));

        ScanOptions options;
        options.root = temp.path();
        LibrarySnapshot snapshot;
        Failure failure;
        registry.check(scan_library(options, snapshot, failure), "根下没有 BGM 目录时仍能扫描");

        const std::vector<std::string> names = category_names(snapshot.bgm);
        registry.check(names.size() == 1 && names.front() == std::string(default_category_name),
                       "退让成单个\"默认\"分类（SFX 没有被当成 BGM 的分类）");
        registry.check(track_names(snapshot.bgm, "默认").size() == 1, "根下的曲子进了\"默认\"分类");
        registry.check(snapshot.sfx.size() == 1, "SFX 仍然按 SFX 子目录扫");
        registry.check(snapshot.total_tracks() == 2, "两首歌各归各位，没有被重复计算");
    }
    {
        // 完全没有 SFX 目录 → sfx 为空，但不是失败，也不把 BGM 的内容误当音效
        TempDir temp;
        std::error_code ec;
        std::filesystem::create_directories(temp.path() / L"BGM" / L"战斗", ec);
        static_cast<void>(
            musicboard::test::write_test_wav(temp.path() / L"BGM" / L"战斗" / L"曲子.wav", 200));

        ScanOptions options;
        options.root = temp.path();
        LibrarySnapshot snapshot;
        Failure failure;
        registry.check(scan_library(options, snapshot, failure), "没有 SFX 目录时扫描仍成功");
        registry.check(snapshot.sfx.empty(), "SFX 为空列表");
        registry.check(snapshot.total_tracks() == 1, "曲目没有被误算成音效");
        registry.check(failure.empty(), "缺 SFX 目录不算错误");
    }

    // =======================================================================
    // A4. 参数错误
    // =======================================================================
    {
        ScanOptions options;
        LibrarySnapshot snapshot;
        Failure failure;
        registry.check(!scan_library(options, snapshot, failure) &&
                           failure.code == fail_code::invalid_argument,
                       "根目录为空被拒");

        options.root = L"Z:\\这个盘不存在_音乐板测试";
        Failure failure2;
        registry.check(!scan_library(options, snapshot, failure2) &&
                           failure2.code == fail_code::file_not_found,
                       "根目录不存在时报 file_not_found");
    }

    // =======================================================================
    // B. 真实曲库
    // =======================================================================
    {
        const std::filesystem::path root = library_root_from_env();
        if (root.empty()) {
            registry.skip("真实曲库扫描（没有设置 MUSICBOARD_TEST_LIBRARY）");
            registry.skip("真实曲库扫描的内存增量");
        } else if (!std::filesystem::is_directory(root)) {
            registry.skip("真实曲库扫描（MUSICBOARD_TEST_LIBRARY 指向的目录不存在）");
            registry.skip("真实曲库扫描的内存增量");
        } else {
            // 先探一个文件把 Media Foundation 的 DLL 全部加载进来。
            // 不然第一次 probe 的加载开销会被算进"扫描的内存增量"，数字就没意义了。
            LibrarySnapshot warmup;
            Failure warmup_failure;
            ScanOptions warmup_options;
            warmup_options.root = root;
            static_cast<void>(scan_library(warmup_options, warmup, warmup_failure));

            const std::size_t before = working_set_bytes();
            const auto started = std::chrono::steady_clock::now();

            LibrarySnapshot snapshot;
            Failure failure;
            ScanOptions options;
            options.root = root;
            const bool ok = scan_library(options, snapshot, failure);
            const auto elapsed = std::chrono::steady_clock::now() - started;
            const std::size_t after = working_set_bytes();
            const double seconds = std::chrono::duration<double>(elapsed).count();

            std::printf("     真实曲库: %zu 个候选, %zu 成功, %zu 失败, %zu 个 BGM 分类, "
                        "%zu 个 SFX 分类, 耗时 %.2f 秒, 工作集增量 %.1f MB\n",
                        snapshot.candidate_count, snapshot.loaded_count, snapshot.failed_count,
                        snapshot.bgm.size(), snapshot.sfx.size(), seconds,
                        static_cast<double>(after > before ? after - before : 0) / (1024.0 * 1024.0));

            registry.check_eq(ok, "真实曲库扫描成功", failure.to_string());
            registry.check(snapshot.candidate_count > 0, "真实曲库里有候选文件");
            registry.check(snapshot.loaded_count + snapshot.failed_count == snapshot.candidate_count,
                           "真实曲库：成功 + 失败 == 候选");
            registry.check(snapshot.warnings.size() == snapshot.failed_count,
                           "真实曲库：每个失败都对应一条 warning");
            registry.check(snapshot.total_tracks() == snapshot.loaded_count,
                           "真实曲库：曲目总数等于成功数");
        registry.check(!snapshot.bgm.empty(), "真实曲库扫出了 BGM 分类");

        // 这条是本次扫描存在的全部理由：491 个文件 / 3.7 GB 全量扫描的内存增量
        // 必须是几 MB 量级，而不是 GB 量级。上限给得很松（64 MB），
        // 只为抓住"有人把 probe 换成了 decode"这种量级上的错误。
        const std::size_t delta = after > before ? after - before : 0;
        registry.check_eq(delta < 64u * 1024u * 1024u,
                          "全量扫描的内存增量在几十 MB 以内（只 probe，不解码）",
                          std::to_string(delta / (1024 * 1024)) + " MB");

        // 硬要求：磁盘上每一个（扫描深度内的）音频文件都必须在结果里出现。
        const std::size_t expected = count_expected_candidates(root);
        registry.check_eq(snapshot.candidate_count == expected,
                          "曲库里没有一个音频文件是看不见的",
                          "扫描器 " + std::to_string(snapshot.candidate_count) + " 个 / 盘上应有 " +
                              std::to_string(expected) + " 个");


            // 真实曲库上再验一次取消：立刻取消不该交出任何分类
            ScanOptions cancel_options;
            cancel_options.root = root;
            cancel_options.should_cancel = []() { return true; };
            LibrarySnapshot cancelled;
            Failure cancel_failure;
            registry.check(scan_library(cancel_options, cancelled, cancel_failure) &&
                               cancelled.cancelled && cancelled.bgm.empty(),
                           "真实曲库上立即取消同样不交出半份结果");
        }
    }

    return registry.finish("library_scanner_tests");
}
