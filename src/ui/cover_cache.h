#pragma once

#include <cstddef>
#include <filesystem>
#include <list>
#include <string>
#include <unordered_map>

#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "core/failure.h"

namespace musicboard::ui {

// ============================================================================
// 封面位图缓存
//
// 把 cover_art 抠出来的图片字节解码成 D2D 位图，并按文件路径缓存。
//
// 三条设计：
//
//   1. **解码时就缩小到唱片尺寸。** 一张 3000×3000 的专辑图解码出来是 36 MB，
//      而我们最多按 140 px 画。同一个曲库翻两页就能把显存吃光。
//      所以解码路径上直接缩到长边 512（比最大的用法还留了余量）。
//   2. **按总字节数淘汰，不按张数。** "缓存 64 张"在每张 12 KB 和每张 4 MB
//      的两种素材下完全不是一回事。上限按像素字节算才可控。
//   3. **失败也要能上屏。** "这首歌没有封面"（不是错误）与"封面读不出来"
//      （是错误）必须分得开 —— 前者回退黑胶，后者要能告诉用户为什么。
//
// 线程：只在 UI 线程用。D2D 设备上下文不是多线程的，缓存跟着它走。
// ============================================================================

struct CoverCacheStats final {
    std::size_t hits = 0;
    std::size_t misses = 0;      // 需要解码的次数
    std::size_t evictions = 0;
    std::size_t failures = 0;    // 解码失败
    std::size_t no_cover = 0;    // 这首歌本来就没有封面
    std::size_t bytes_cached = 0;
};

class CoverCache final {
public:
    // 解码后长边超过这个就缩小。140px 的唱片、以及将来可能有的详情页，
    // 512 都够用；再大只是白占显存。
    static constexpr UINT kMaximumDecodedEdge = 512;
    // 缓存占用的上限（解码后的像素字节）
    static constexpr std::size_t kDefaultByteBudget = 64u * 1024u * 1024u;

    CoverCache() = default;
    ~CoverCache();

    CoverCache(const CoverCache&) = delete;
    CoverCache& operator=(const CoverCache&) = delete;

    // context 必须比缓存活得久。**不持有所有权**，因为设备上下文由窗口持有。
    [[nodiscard]] bool initialize(ID2D1DeviceContext* context, Failure& failure);

    // 取封面位图。
    //   返回非空  → 拿到了。
    //   返回空且 failure 为空 → 这首歌没有封面（正常结果，界面回退黑胶）。
    //   返回空且 failure 非空 → 读或解码出了问题，原因要能上屏。
    [[nodiscard]] ID2D1Bitmap1* get(const std::filesystem::path& audio_path, Failure& failure);

    void clear();
    void set_byte_budget(std::size_t bytes) noexcept { byte_budget_ = bytes; }
    [[nodiscard]] const CoverCacheStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t entry_count() const noexcept { return entries_.size(); }

private:
    struct Entry final {
        std::wstring key;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
        std::size_t bytes = 0;
    };

    [[nodiscard]] bool decode(const std::filesystem::path& path,
                             Microsoft::WRL::ComPtr<ID2D1Bitmap1>& out, Failure& failure);
    void touch(std::list<Entry>::iterator it);
    void evict_until_within_budget();

    ID2D1DeviceContext* context_ = nullptr;   // 非持有
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    // 最近用过的放前面
    std::list<Entry> entries_;
    std::unordered_map<std::wstring, std::list<Entry>::iterator> index_;
    std::size_t byte_budget_ = kDefaultByteBudget;
    std::size_t bytes_used_ = 0;
    CoverCacheStats stats_;
};

}  // namespace musicboard::ui
