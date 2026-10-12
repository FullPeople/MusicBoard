#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "audio/decoder.h"
#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 曲库扫描
//
// 目录约定（与原版一致）：
//
//   <根>/BGM/<分类>/*.ogg|mp3|wav      ← 音乐
//   <根>/SFX/<分类>/*.ogg|mp3|wav      ← 音效
//
// 两条例外规则，都是为了兼容用户真的会做出来的目录：
//   · 根下没有 BGM 子目录 → 把根本身当作 BGM 目录（原版也是这么退让的）。
//     这时 <根>/SFX 会被排除，它是另一个栏目，不是 BGM 的分类。
//   · 分类目录下再有子目录 → 忽略，不递归。**只扫一层。**
//     这一条是刻意的：原版有个 bug 是 `create_category_folder("a/b")` 真的建出了嵌套目录，
//     而加载器只扫一层 —— 于是内存里多出一个叫 "a/b" 的分类、磁盘上的文件永远不显示。
//     扫描端的策略是"忽略"，写入端（library_ops）的策略是"拒绝"，两边合起来才严密。
//   · 栏目根下**散着**的音频文件 → 收进一个额外的"默认"分类。
//     只扫一层的话这些文件会彻底消失：磁盘上明明在，界面上永远看不到，也没有提示。
//     实测真实曲库就有一个（BGM/背景恐怖音2.mp3）。给它们一个分类比丢掉更好。
//     一个候选都没有时不会凭空多出这个分类。
//
// 关键：**只用 probe，不解码。**
// 实测真实曲库 491 个文件 / 3.7 GB，全量扫描耗时 3.2 秒、内存增量 6 MB。
// 一旦这里改成取 PCM，常驻内存立刻变成 GB 级 —— 这条写在 AGENTS.md 的持续边界里。
// ============================================================================

inline constexpr std::string_view default_category_name = "默认";

struct LibraryTrack final {
    std::string name;                  // 不含扩展名
    std::filesystem::path path;
    AudioContainer container = AudioContainer::unknown;
    AudioFormat format;
    double duration_seconds = 0.0;
};

struct LibraryCategory final {
    std::string name;
    std::vector<LibraryTrack> tracks;
};

struct LibrarySnapshot final {
    std::vector<LibraryCategory> bgm;
    std::vector<LibraryCategory> sfx;

    // 扫不动或不合法的文件。不当作致命失败 —— 一个坏文件不该让整库列不出来，
    // 但也不能什么都不说，所以逐条记下来交给上层提示。
    std::vector<std::string> warnings;
    std::size_t candidate_count = 0;   // 发现的候选音频文件数（含失败的）
    std::size_t loaded_count = 0;      // 成功探出格式的
    std::size_t failed_count = 0;
    bool cancelled = false;

    [[nodiscard]] std::size_t total_tracks() const noexcept;
};

struct ScanOptions final {
    std::filesystem::path root;
    // 进度回调：已完成 / 总数 / 当前文件。可以为空。
    std::function<void(std::size_t done, std::size_t total, const std::filesystem::path&)> on_progress;
    // 取消判定：返回 true 表示请求中止。可以为空（表示不可取消）。
    std::function<bool()> should_cancel;
};

// 扫描曲库。取消时返回 true 但 snapshot.cancelled 为 true。
// 只有"根目录不存在/打不开"这类问题才算失败。
[[nodiscard]] bool scan_library(const ScanOptions& options, LibrarySnapshot& snapshot,
                                Failure& failure);

// 支持的扩展名（不含点，小写）。与 decoder 的容器嗅探是两回事：
// 这里只是"要不要打开看看"，真正的格式判断交给嗅探。
[[nodiscard]] bool is_supported_audio_extension(std::string_view extension_lowercase) noexcept;

}  // namespace musicboard
