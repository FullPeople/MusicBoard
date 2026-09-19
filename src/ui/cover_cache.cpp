#include "ui/cover_cache.h"

#include <algorithm>

#include "core/path_utf8.h"
#include "library/cover_art.h"

#pragma comment(lib, "windowscodecs.lib")

namespace musicboard::ui {
namespace {

[[nodiscard]] std::string hresult_hex(HRESULT hr) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

// 位图的像素字节数（缓存预算按它算）
[[nodiscard]] std::size_t bitmap_bytes(const D2D1_SIZE_U& size) noexcept {
    return static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height) * 4u;
}

}  // namespace

CoverCache::~CoverCache() {
    // 位图必须在设备上下文还活着的时候释放。调用方负责这个顺序 ——
    // 头文件里写明了 context 不归缓存所有，缓存必须比它先死。
    entries_.clear();
}

bool CoverCache::initialize(ID2D1DeviceContext* context, Failure& failure) {
    if (context == nullptr) {
        failure.set(fail_code::invalid_argument, "设备上下文为空，封面缓存没法建位图");
        return false;
    }
    context_ = context;

    const HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(wic_.GetAddressOf()));
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed,
                    "创建 WIC 工厂失败，hr=" + hresult_hex(hr));
        return false;
    }
    return true;
}

void CoverCache::clear() {
    entries_.clear();
    index_.clear();
    bytes_used_ = 0;
    stats_.bytes_cached = 0;
}

void CoverCache::touch(std::list<Entry>::iterator it) {
    if (it == entries_.begin()) {
        return;
    }
    entries_.splice(entries_.begin(), entries_, it);
}

void CoverCache::evict_until_within_budget() {
    // 从最久没用过的开始丢，直到装得下。
    // 按字节数而不是张数：每张 12 KB 和每张 4 MB 的素材，同样的"64 张"差着量级。
    //
    // **至少留一张**：刚插进来这张正在被调用方用着，把它自己淘汰掉
    // 会让 get() 返回空 —— 而调用方会把"空"理解成"这首歌没有封面"，
    // 于是界面上偶尔少一张图，还查不出原因。
    while (entries_.size() > 1 && bytes_used_ > byte_budget_) {
        Entry& victim = entries_.back();
        bytes_used_ -= victim.bytes;
        index_.erase(victim.key);
        entries_.pop_back();
        ++stats_.evictions;
    }
    stats_.bytes_cached = bytes_used_;
}

bool CoverCache::decode(const std::filesystem::path& path,
                        Microsoft::WRL::ComPtr<ID2D1Bitmap1>& out, Failure& failure) {
    // 1. 先把封面字节抠出来（cover_art 那一层只管抠，不管解码）
    CoverArt cover;
    Failure cover_failure;
    const bool found = find_cover_art(path, cover, cover_failure);
    if (!found) {
        if (!cover_failure.empty()) {
            failure = cover_failure;
            return false;
        }
        // 没有封面 —— 不是错误，只是没有
        ++stats_.no_cover;
        return false;
    }

    // 2. 从内存解出位图。**不落临时文件** ——
    //    落盘再读回来在中文路径与并发访问上都是多余的失败点。
    Microsoft::WRL::ComPtr<IWICStream> stream;
    HRESULT hr = wic_->CreateStream(stream.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "创建 WIC 流失败，hr=" + hresult_hex(hr));
        return false;
    }
    hr = stream->InitializeFromMemory(const_cast<BYTE*>(cover.bytes.data()),
                                      static_cast<DWORD>(cover.bytes.size()));
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "WIC 流绑定内存失败，hr=" + hresult_hex(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = wic_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                      decoder.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::decode_failed,
                    path_to_utf8(path) + " 的封面（来源 " + cover.source +
                        "）无法解码，hr=" + hresult_hex(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::decode_failed, "取封面帧失败，hr=" + hresult_hex(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = wic_->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "创建格式转换器失败，hr=" + hresult_hex(hr));
        return false;
    }
    // 统一转成预乘 BGRA —— 与合成表面一致，画的时候不用再想通道顺序
    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                              WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) {
        failure.set(fail_code::decode_failed,
                    "封面的像素格式不支持，hr=" + hresult_hex(hr));
        return false;
    }

    // 3. 太大就缩。这一步是"不要把显存吃光"的关键 ——
    //    一张 3000×3000 的专辑图解码出来是 36 MB，而唱片只有 140 px。
    UINT width = 0;
    UINT height = 0;
    hr = converter->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) {
        failure.set(fail_code::decode_failed, "读封面尺寸失败，hr=" + hresult_hex(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapSource> source = converter;
    if (width > kMaximumDecodedEdge || height > kMaximumDecodedEdge) {
        const double scale = static_cast<double>(kMaximumDecodedEdge) /
                             static_cast<double>(std::max(width, height));
        const UINT scaled_width = std::max<UINT>(1, static_cast<UINT>(width * scale));
        const UINT scaled_height = std::max<UINT>(1, static_cast<UINT>(height * scale));

        Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
        hr = wic_->CreateBitmapScaler(scaler.GetAddressOf());
        if (FAILED(hr)) {
            failure.set(fail_code::platform_api_failed, "创建缩放器失败，hr=" + hresult_hex(hr));
            return false;
        }
        hr = scaler->Initialize(converter.Get(), scaled_width, scaled_height,
                               WICBitmapInterpolationModeFant);
        if (FAILED(hr)) {
            failure.set(fail_code::decode_failed, "封面缩放失败，hr=" + hresult_hex(hr));
            return false;
        }
        source = scaler;
        width = scaled_width;
        height = scaled_height;
    }

    // 4. 交给 D2D
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    hr = context_->CreateBitmapFromWicBitmap(source.Get(), &properties, out.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed,
                    "从 WIC 位图创建 D2D 位图失败，hr=" + hresult_hex(hr));
        return false;
    }
    static_cast<void>(width);
    static_cast<void>(height);
    return true;
}

ID2D1Bitmap1* CoverCache::get(const std::filesystem::path& audio_path, Failure& failure) {
    failure.clear();
    if (context_ == nullptr || !wic_) {
        failure.set(fail_code::invalid_argument, "封面缓存还没初始化");
        return nullptr;
    }

    const std::wstring key = audio_path.wstring();
    const auto found = index_.find(key);
    if (found != index_.end()) {
        ++stats_.hits;
        touch(found->second);
        return found->second->bitmap.Get();
    }

    ++stats_.misses;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
    if (!decode(audio_path, bitmap, failure)) {
        if (!failure.empty()) {
            ++stats_.failures;
        }
        return nullptr;
    }

    const D2D1_SIZE_U size = bitmap->GetPixelSize();
    Entry entry;
    entry.key = key;
    entry.bytes = bitmap_bytes(size);
    entry.bitmap = std::move(bitmap);

    entries_.push_front(std::move(entry));
    index_[key] = entries_.begin();
    bytes_used_ += entries_.front().bytes;
    evict_until_within_budget();

    // 一定拿得到：evict 保证不会把刚插进来的这张淘汰掉
    return entries_.front().bitmap.Get();
}

}  // namespace musicboard::ui
