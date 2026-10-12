#pragma once

#include <filesystem>
#include <string>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 导入事务
//
// 把"一个已经躺在暂存区的文件"安全地放进曲库。三步，每一步都不能省：
//
//   1. **校验来源** —— 文件存在、是普通文件、不是符号链接，
//      而且**确实落在暂存区之内**（挡住 "../.." 这类路径逃逸）。
//   2. **校验内容** —— 用 probe_audio_file 确认它真是能解的音频。
//      一个改名成 .mp3 的可执行文件在这一步被挡住 —— 不需要专门去认 PE 头，
//      解不出来的就不是音频。
//   3. **原子发布** —— 同盘用 MoveFileExW 原子替换；跨盘走
//      复制 → 校验大小 → 删源。发布之后再探一次，失败就把刚发布的文件删掉。
//
// 为什么不直接让下载器写进曲库目录：原版就是这么做的，然后用
// `_find_newest_file()` 猜哪个是刚下的（url_importer.gd:449）。
// 下载中途失败会留下半个文件，而"最新那个"可能根本是别的文件。
// 分成暂存区 + 事务发布之后，曲库里要么没有、要么是完整可用的。
//
// 关于重名：**永不覆盖**。目标已有同名文件时自动加 " (2)"、"(3)" 后缀，
// 并通过 renamed_to_avoid_collision 告诉调用方 —— 界面该让用户知道名字变了。
// ============================================================================

struct ImportRequest final {
    // 已经下载/生成好的暂存文件
    std::filesystem::path staged_file;
    // 暂存区根目录。staged_file 必须落在它里面。
    std::filesystem::path staging_root;
    // 目标分类目录（必须在曲库里、必须已存在）
    std::filesystem::path target_category_dir;
    // 期望的文件名主干（不含扩展名）。为空则沿用暂存文件的主干。
    std::string preferred_stem;
    double source_duration_seconds = 0.0;
};

struct ImportResult final {
    std::filesystem::path installed_path;
    std::string final_stem;
    // 因为目标已有同名文件而换了名字
    bool renamed_to_avoid_collision = false;
    // 发布之后重新探测得到的时长（给界面直接显示用）
    double duration_seconds = 0.0;
    bool content_incomplete = false;
};

// 执行导入。成功时 result.installed_path 是曲库里的最终位置。
[[nodiscard]] bool import_staged_file(const ImportRequest& request, ImportResult& result,
                                      Failure& failure);

}  // namespace musicboard
