#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 曲库文件操作
//
// 这一层的核心职责是**在校验上不放水**。原版有三个具体的坑：
//
//   1. `create_category_folder("a/b")` 会真的建出嵌套目录，而加载器只扫一层 ——
//      内存里多出一个叫 "a/b" 的分类、磁盘上的文件永远不显示。
//   2. 重命名不检查 Windows 非法字符与保留名（CON、PRN、NUL…），
//      失败后只写控制台，界面上表现为"点了没反应"。
//   3. 删除是直接从磁盘抹掉，没有回收站这一层缓冲。
//
// 所以这里的规矩是：**先校验，再动盘；任何一步失败都带错误码与上下文返回。**
// 校验函数是纯函数，可以完全离线穷举测。
// ============================================================================

// 文件名主干（不含扩展名）的长度上限。NTFS 单个组件上限 255，留一点余量。
inline constexpr std::size_t maximum_file_stem_length = 200;
// 分类名用同一个上限 —— 它们最终都是目录名
inline constexpr std::size_t maximum_category_name_length = 100;

// ---------------------------------------------------------------------------
// 校验（纯函数）
// ---------------------------------------------------------------------------

// 校验一个用作"文件名主干"的字符串。不合法时给出具体原因。
// 拒绝：空 / 全空白、Windows 非法字符、控制字符、保留设备名、
//       结尾的点或空格、过长。
[[nodiscard]] bool validate_file_stem(std::string_view stem, Failure& failure);

// 校验分类名。规则同上，额外拒绝 "." 与 ".."。
[[nodiscard]] bool validate_category_name(std::string_view name, Failure& failure);

// 把任意字符串净化成一个可用的名字：替换非法字符、去掉首尾空白与结尾的点。
// 用于"从 URL 标题生成文件名"这类场景 —— 净化后仍需过一遍 validate_*。
[[nodiscard]] std::string sanitize_to_valid_name(std::string_view raw,
                                                std::size_t max_length = maximum_file_stem_length);

// ---------------------------------------------------------------------------
// 删除方式
// ---------------------------------------------------------------------------

enum class DeleteMode : std::uint8_t {
    // 默认走回收站。用户的音乐库是私人财产，误删一下不该是不可逆的。
    recycle_bin,
    // 明确要求永久删除时才用（例如"清理导入失败产生的残片"）
    permanent,
};

[[nodiscard]] const char* delete_mode_name(DeleteMode mode) noexcept;

// ---------------------------------------------------------------------------
// 操作
// ---------------------------------------------------------------------------

struct LibraryOpResult final {
    // 变化之后的实际路径。重命名与移动会给新路径；删除后为空。
    std::filesystem::path resulting_path;
    // 目标已存在时自动加了后缀（避免覆盖）
    bool renamed_to_avoid_collision = false;
    DeleteMode delete_mode = DeleteMode::recycle_bin;
};

// 重命名曲目。new_stem 不含扩展名，扩展名保持不变。
[[nodiscard]] bool rename_track(const std::filesystem::path& track_path, std::string_view new_stem,
                                LibraryOpResult& result, Failure& failure);

// 删除曲目。
[[nodiscard]] bool delete_track(const std::filesystem::path& track_path, DeleteMode mode,
                                LibraryOpResult& result, Failure& failure);

// 把曲目移到目标分类目录（跨盘也支持）。
[[nodiscard]] bool move_track(const std::filesystem::path& track_path,
                              const std::filesystem::path& target_category_dir,
                              LibraryOpResult& result, Failure& failure);

// 在栏目根下新建分类目录。
[[nodiscard]] bool create_category(const std::filesystem::path& section_base,
                                   std::string_view name, Failure& failure);

// 重命名分类目录。
[[nodiscard]] bool rename_category(const std::filesystem::path& section_base,
                                   std::string_view old_name, std::string_view new_name,
                                   Failure& failure);

// 删除**空**分类目录。非空时明确拒绝并说明，不递归删除里面的东西。
[[nodiscard]] bool delete_empty_category(const std::filesystem::path& section_base,
                                         std::string_view name, Failure& failure);

// ---------------------------------------------------------------------------
// 路径安全
// ---------------------------------------------------------------------------

// 判断 child 是否落在 parent 之内（规范化之后）。
// 用于挡住 "分类名写成 ../../Windows" 这类路径逃逸。
[[nodiscard]] bool path_is_inside(const std::filesystem::path& parent,
                                  const std::filesystem::path& child);

// 目标目录里已经存在同名文件时，给出一个不冲突的名字：name(2).ogg、name(3).ogg……
[[nodiscard]] std::filesystem::path unique_path_in(const std::filesystem::path& directory,
                                                  std::string_view stem, std::string_view extension);

// 目录 + UTF-8 名字 + 扩展名。
//
// 需要它的原因不是"少写一行"，而是**唯一入口**：绝不要写 `directory / std::string(...)`
// 或 `path / std::string_view`，那个重载按活动代码页（本机 936/GBK）解释窄字符串，
// 一个 UTF-8 的中文名字会被转成另一个路径。实测的表现是
// "create_category 返回成功，但扫描器找不到那个分类" —— 走的就是这条路。
// 任何"把 UTF-8 名字接成路径"的地方都必须经过这里。
[[nodiscard]] std::filesystem::path track_path_for(const std::filesystem::path& directory,
                                                  std::string_view stem,
                                                  std::string_view extension);

}  // namespace musicboard
