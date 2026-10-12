#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "audio/decoder.h"

namespace musicboard {

// ============================================================================
// 波形包络
//
// 界面上的音波图与悬浮球的辉光都吃这一份数据。两条刻意的设计：
//
//   1. **包络从"已经解出来准备播的 PCM"算，不单独再解一遍。**
//      一首 4 分钟的歌是 42 MB 的 PCM16，解码一次就够了 ——
//      为了画波形再解一次，等于把播放前的等待时间翻倍。
//   2. **纯函数。** 收样本指针、帧数、声道数、桶数，返回每个桶的峰值。
//      于是它可以完全离线穷举测（正弦、静音、单点尖峰、削顶、立体声左右不一致），
//      不需要任何真实音频素材。
//
// 原版的 `waveform_analyzer.gd` 走的是另一条路：对**压缩字节**做"能量分析"
// （`_analyze_compressed_energy`），本质是猜 —— 它算的是字节偏离 128 的均方根，
// 和实际响度没有确定关系。那个文件在原型里最后也没被用上（是死代码）。
// 这里改成从真 PCM 算，墙上的波形和耳朵听到的才对得上。
// ============================================================================

// 桶数是界面用多少个像素宽画它。太少会糊，太多是浪费。
inline constexpr std::size_t kDefaultWaveformBuckets = 320;

// On-demand preview: retain only bucket peaks, never the whole song's PCM.
[[nodiscard]] bool extract_waveform_file(const std::filesystem::path& path,
                                         std::vector<float>& envelope, Failure& failure,
                                         const std::atomic<bool>* cancelled = nullptr);

// 从交错的 PCM16 样本算峰值包络，并归一化到 [0,1]。
//
// 归一化按**全局最大值**做（而不是按 1.0 满刻度）：很多素材录得偏轻，
// 按满刻度归一化之后波形会贴在中间一条线上，什么都看不出来。
// 全静音时返回全 0，不返回 NaN，也不伪造一条底噪波形 ——
// 静音就该画成一条直线，而不是"看起来像有声音"。
[[nodiscard]] std::vector<float> compute_waveform_envelope(const std::int16_t* samples,
                                                          std::size_t frame_count,
                                                          std::size_t channels,
                                                          std::size_t buckets,
                                                          Failure& failure);

// 对一段已解码的音频算包络。声道数从 DecodedAudio 里取。
[[nodiscard]] std::vector<float> compute_waveform_envelope(const DecodedAudio& audio,
                                                          std::size_t buckets, Failure& failure);

// 在进度处取幅度。progress 会被夹到 [0,1]（夹取不丢信息，所以不算静默回退）。
// 包络为空时返回 0 —— 悬浮球在"没在播"的时候就该是不发光的。
[[nodiscard]] float amplitude_at(const std::vector<float>& envelope, double progress) noexcept;

}  // namespace musicboard
