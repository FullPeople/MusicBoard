#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "audio/container_probe.h"
#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 封面提取
//
// 设计稿里"有封面时整盘显示封面，像光盘的印刷面"是这一层存在的原因。
// 来源按优先级：**文件内嵌图 → 同目录封面文件 → 没有**（回退黑胶）。
//
// 三条刻意的设计：
//
//   1. **只读标签区域，不读整个文件。**
//      一个 mp3 的 ID3v2 标签在文件开头、长度写在头里；FLAC 的元数据块同理。
//      为了一张封面把 3.7 GB 曲库里某个 20 MB 的文件整个读进内存，
//      和"扫描阶段就 decode"是同一类错误。所以每种容器都只读它声明的那一段。
//   2. **返回的是编码后的图片字节，不是解码后的像素。**
//      解码成位图是 UI 层的事（WIC）；这一层只负责"从容器里把那段字节抠出来"。
//      于是它可以完全离线测，也不需要 D3D。
//   3. **取不到封面不是错误。** 返回 false 且 failure 为空是正常结果 ——
//      "这首歌没有封面"和"读不出来"必须分得开。前者回退黑胶，后者要能上屏。
//
// 这一层也是原版没有的：Godot 版的曲目卡上从来没有封面，
// 唱片永远是黑胶 —— 而素材库里带内嵌封面的文件占相当一部分。
// ============================================================================

// 图片编码格式。UI 层按它决定怎么交给 WIC（其实 WIC 自己认魔数，
// 这个字段主要是给日志与界面提示用的）。
enum class CoverImageFormat : std::uint8_t {
    unknown,
    png,
    jpeg,
    bmp,
    gif,
    webp,
};

[[nodiscard]] const char* cover_format_name(CoverImageFormat format) noexcept;
[[nodiscard]] const char* cover_format_extension(CoverImageFormat format) noexcept;

struct CoverArt final {
    std::vector<std::uint8_t> bytes;      // 图片文件的原始字节
    CoverImageFormat format = CoverImageFormat::unknown;
    // 来自哪里："id3v2" / "flac" / "vorbis" / "mp4" / "sidecar:<文件名>"
    std::string source;
    // 内嵌图的 MIME 声明（"image/jpeg" 之类），可能为空
    std::string mime;

    [[nodiscard]] bool empty() const noexcept { return bytes.empty(); }
};

// 内嵌封面上限。超过就不取 —— 一张 50 MB 的"封面"是素材出了问题，
// 不是我们该默默吃下去的东西。
inline constexpr std::size_t kMaximumCoverBytes = 16u * 1024u * 1024u;

// 从音频文件里抠出内嵌封面。
//
// 返回 true 表示"找到了封面"。返回 false 且 failure 为空表示"这首歌没有内嵌封面"
// （这是正常结果）；failure 非空表示"读的过程中出了问题"。
[[nodiscard]] bool extract_embedded_cover(const std::filesystem::path& path, CoverArt& cover,
                                         Failure& failure);

// 按优先级取封面：先内嵌，再同目录封面文件。
// 两者都没有时返回 false 且 failure 为空。
[[nodiscard]] bool find_cover_art(const std::filesystem::path& audio_path, CoverArt& cover,
                                 Failure& failure);

// 同目录封面文件的候选名（不含目录）。给测试与文档用，顺序即优先级。
[[nodiscard]] std::vector<std::string> sidecar_cover_names(const std::string& audio_stem);

// 按魔数判断图片格式。UI 层可以信任它，不必自己再嗅一遍。
[[nodiscard]] CoverImageFormat sniff_image_format(const std::uint8_t* bytes,
                                                 std::size_t size) noexcept;

}  // namespace musicboard
