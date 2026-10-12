#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <vector>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 暂存区
//
// 下载一律先落在暂存区，校验通过之后才由 import_staged_file 搬进曲库。
//
// 原版是让下载器**直接写进曲库目录**，然后用 `_find_newest_file()` 猜哪个是刚下的
// （url_importer.gd:449）。两个后果：
//   · 下载中途失败会在曲库里留下半个文件，而界面上它长得跟正常曲目一样；
//   · "最新那个文件"完全可能不是刚下的 —— 用户同时往曲库拖了个文件就猜错了。
//
// 分成暂存区之后，曲库里要么没有这首歌，要么它是完整可用的。
//
// 每个导入任务一个独立子目录，这一点同时也是"下载完到底产出了什么"的答案来源：
// 不必再去猜文件名，直接列目录就是。**不猜，就没有猜错。**
// ============================================================================

// %LOCALAPPDATA%\MusicBoard\staging
[[nodiscard]] std::filesystem::path default_staging_root();

// 建一个本次导入专用的空目录：<staging_root>/job-<时间>-<pid>-<序号>
[[nodiscard]] bool create_staging_job_dir(const std::filesystem::path& staging_root,
                                         std::filesystem::path& out, Failure& failure);

// 列出目录里的普通文件（只一层，按名字排序，忽略隐藏项）。
[[nodiscard]] bool list_staged_files(const std::filesystem::path& job_dir,
                                    std::vector<std::filesystem::path>& out, Failure& failure);

// 删掉整个任务目录。导入成功、或用户明确取消之后调用。
[[nodiscard]] bool remove_staging_job_dir(const std::filesystem::path& job_dir, Failure& failure);

// 清掉超过 keep 的旧任务目录（上次进程崩了留下的）。removed 是删掉的个数。
// 失败时只报"哪个删不掉"，不中断整体清理 —— 一个删不掉不该让其余的都留着。
[[nodiscard]] bool prune_staging(const std::filesystem::path& staging_root,
                                std::chrono::hours keep, std::size_t& removed, Failure& failure);

}  // namespace musicboard
