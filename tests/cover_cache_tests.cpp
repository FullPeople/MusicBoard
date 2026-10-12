// 封面位图缓存测试
//
// 这里要真解一次图，所以需要一个 D2D 设备上下文 —— headless 也能建
// （D3D11CreateDevice 不需要窗口），于是这个测试可以在没有桌面的环境里跑。
//
// 测的是三件事：
//   · 抠出来的字节真的能被解成位图（用 WIC 现场生成一张真 PNG，不依赖素材）
//   · 缓存命中时**不重新解码**（用 stats 证明，不是靠"看起来一样"）
//   · 超预算时会淘汰，且**不淘汰刚插进来那张**（否则 get 会返回空，
//     而调用方会把空理解成"没有封面"）

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <d2d1_1.h>
#include <d3d11.h>
#include <shlwapi.h>

#include "core/path_utf8.h"
#include "tests/test_files.h"
#include "tests/test_harness.h"
#include "ui/cover_cache.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace musicboard;
using musicboard::test::Registry;
using musicboard::test::TempDir;
using namespace musicboard::ui;

namespace {

// ---- 用 WIC 现场生成一张真 PNG，编码进内存流再取回字节。不依赖任何素材。----
std::vector<std::uint8_t> encode_png_to_memory(UINT width, UINT height) {
    std::vector<std::uint8_t> out;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(wic.GetAddressOf())))) {
        return out;
    }

    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapCacheOnLoad, bitmap.GetAddressOf()))) {
        return out;
    }
    {
        Microsoft::WRL::ComPtr<IWICBitmapLock> lock;
        WICRect rect{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
        if (FAILED(bitmap->Lock(&rect, WICBitmapLockWrite, lock.GetAddressOf()))) {
            return out;
        }
        UINT stride = 0;
        UINT buffer_size = 0;
        BYTE* data = nullptr;
        if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&buffer_size, &data)) ||
            data == nullptr) {
            return out;
        }
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                BYTE* pixel =
                    data + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4;
                const bool dark = ((x / 16) + (y / 16)) % 2 == 0;
                pixel[0] = dark ? 0x20 : 0xF0;
                pixel[1] = dark ? 0x40 : 0xA0;
                pixel[2] = dark ? 0x90 : 0x40;
                pixel[3] = 0xFF;
            }
        }
    }

    // SHCreateMemStream 给一个可写的内存流，WIC 能往里编码
    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(nullptr, 0));
    if (!stream) {
        return out;
    }

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf())) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        return out;
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> properties;
    if (FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf())) ||
        FAILED(frame->Initialize(properties.Get())) || FAILED(frame->SetSize(width, height))) {
        return out;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WriteSource(bitmap.Get(), nullptr)) || FAILED(frame->Commit()) ||
        FAILED(encoder->Commit())) {
        return out;
    }

    // 从内存流里取回字节
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
        return out;
    }
    const ULONG size = static_cast<ULONG>(stat.cbSize.QuadPart);
    out.resize(size);
    LARGE_INTEGER zero{};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
        return {};
    }
    ULONG read = 0;
    if (FAILED(stream->Read(out.data(), size, &read))) {
        return {};
    }
    out.resize(read);
    return out;
}

// COM 的初始化/反初始化守卫。声明在最前面 → 析构在最后面，
// 保证它比所有持有 COM 对象的局部变量活得更久。
class ComGuard final {
public:
    ComGuard() : result(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComGuard() {
        if (SUCCEEDED(result)) {
            ::CoUninitialize();
        }
    }
    ComGuard(const ComGuard&) = delete;
    ComGuard& operator=(const ComGuard&) = delete;

    HRESULT result = E_FAIL;
};

// ---- 一个最小的 headless D2D 设备上下文 ----
class TestDevice final {
public:
    [[nodiscard]] bool create() {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                      D3D11_SDK_VERSION, device_.GetAddressOf(), nullptr, nullptr))) {
            // 没有硬件就退 WARP —— 这个测试不该因为显卡而红
            if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                                          D3D11_SDK_VERSION, device_.GetAddressOf(), nullptr,
                                          nullptr))) {
                return false;
            }
        }
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        if (FAILED(device_.As(&dxgi))) {
            return false;
        }
        D2D1_FACTORY_OPTIONS options{};
        if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                      &options,
                                      reinterpret_cast<void**>(factory_.GetAddressOf())))) {
            return false;
        }
        if (FAILED(factory_->CreateDevice(dxgi.Get(), d2d_device_.GetAddressOf()))) {
            return false;
        }
        return SUCCEEDED(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                          context_.GetAddressOf()));
    }
    [[nodiscard]] ID2D1DeviceContext* context() const noexcept { return context_.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
};

}  // namespace

int main() {
    Registry registry;

    // WIC 与 D2D 都经由 COM。**必须显式初始化** —— 不初始化时
    // CoCreateInstance 返回 CO_E_NOTINITIALIZED，而症状是"封面缓存初始化失败"
    // 加一句看不出所以然的 hr。真实程序里由启动时统一初始化（见 spike 的入口）。
    //
    // 用 RAII 而不是在结尾手写 CoUninitialize：**它必须是最后一个被析构的东西**。
    // 手写在 return 之前的话，COM 会先被反初始化，然后才轮到下面那些
    // 持有 COM 对象的局部变量析构 —— 实测就是这么段错误了一次。
    // 声明在最前面 → 析构在最后面。
    const ComGuard com_guard;
    registry.check_eq(SUCCEEDED(com_guard.result), "COM 初始化成功",
                      "hr=0x" + std::to_string(static_cast<unsigned long>(com_guard.result)));

    TestDevice device;
    if (!device.create()) {
        registry.skip("全部封面缓存用例（无法创建 D2D 设备）");
        return registry.finish("cover_cache_tests");
    }

    // =======================================================================
    // 真 PNG 的解码（这条不过，后面全是空谈）
    // =======================================================================
    const std::vector<std::uint8_t> png = encode_png_to_memory(64, 48);
    registry.check(png.size() > 100, "WIC 现场生成了一张真 PNG");

    TempDir temp;
    registry.check(musicboard::test::write_bytes(temp.path() / L"cover.png", png),
                   "把 PNG 作为同目录封面写下去");

    // 一个最小但合法的 wav，让 cover_art 的容器嗅探有东西可判
    const auto audio = temp.path() / L"一首歌.wav";
    registry.check(musicboard::test::write_test_wav(audio, 200), "写一首音频");

    CoverCache cache;
    Failure failure;
    registry.check_invocation([&] { return cache.initialize(device.context(), failure); }, "初始化封面缓存", failure);

    // =======================================================================
    // 解码成功
    // =======================================================================
    {
        Failure get_failure;
        ID2D1Bitmap1* bitmap = cache.get(audio, get_failure);
        registry.check_eq(bitmap != nullptr, "拿到封面位图", get_failure.to_string());
        if (bitmap != nullptr) {
            const D2D1_SIZE_U size = bitmap->GetPixelSize();
            registry.check(size.width == 64 && size.height == 48,
                           "位图尺寸与原图一致（没有缩放的图不该被动过）");
            registry.check(bitmap->GetPixelFormat().format == DXGI_FORMAT_B8G8R8A8_UNORM &&
                               bitmap->GetPixelFormat().alphaMode ==
                                   D2D1_ALPHA_MODE_PREMULTIPLIED,
                           "像素格式是预乘 BGRA（与合成表面一致）");
        }
        registry.check(get_failure.empty(), "成功时 failure 是空的");
        registry.check(cache.stats().misses == 1, "第一次是未命中（要解码）");
        registry.check(cache.entry_count() == 1, "缓存里有一张");
    }

    // =======================================================================
    // 缓存命中：不重新解码
    // =======================================================================
    {
        const std::size_t decodes_before = cache.stats().misses;
        Failure get_failure;
        ID2D1Bitmap1* again = cache.get(audio, get_failure);
        ID2D1Bitmap1* third = cache.get(audio, get_failure);
        registry.check(again != nullptr && third != nullptr, "第二次第三次都拿得到");
        registry.check(cache.stats().hits == 2, "命中计数 +2");
        registry.check_eq(cache.stats().misses == decodes_before,
                          "没有重新解码（用 stats 证明，不是靠'看起来一样'）",
                          "misses=" + std::to_string(cache.stats().misses));
        registry.check(again == third, "返回的是同一个位图对象");
    }

    // =======================================================================
    // 没有封面：不是错误
    // =======================================================================
    {
        TempDir bare;
        const auto bare_audio = bare.path() / L"光秃秃.wav";
        registry.check(musicboard::test::write_test_wav(bare_audio, 200), "写一首没有封面的音频");

        Failure get_failure;
        ID2D1Bitmap1* bitmap = cache.get(bare_audio, get_failure);
        registry.check(bitmap == nullptr, "没有封面 → 没有位图");
        registry.check_invocation([&] { return get_failure.empty(); }, "failure 是空的（'没有封面'不是错误，界面回退黑胶）", get_failure);
        registry.check(cache.stats().no_cover == 1, "no_cover 计数 +1");
        registry.check(cache.stats().failures == 0, "这不算一次失败");
    }

    // =======================================================================
    // 封面读不出来：是错误，而且原因要能上屏
    // =======================================================================
    {
        TempDir broken;
        const auto broken_audio = broken.path() / L"坏封面.wav";
        registry.check(musicboard::test::write_test_wav(broken_audio, 200), "写音频");
        registry.check(musicboard::test::write_text(broken.path() / L"cover.png", "这不是图片"),
                       "放一个名叫 cover.png 的文本文件");

        Failure get_failure;
        ID2D1Bitmap1* bitmap = cache.get(broken_audio, get_failure);
        registry.check(bitmap == nullptr, "坏封面 → 没有位图");
        registry.check(!get_failure.empty(), "failure 非空（与'没有封面'分得开）");
        registry.check(get_failure.code == fail_code::unsupported_format,
                       "错误码是 unsupported_format");
        registry.check(cache.stats().failures == 1, "failures 计数 +1");
    }
    {
        // 文件根本不存在
        TempDir missing;
        Failure get_failure;
        ID2D1Bitmap1* bitmap = cache.get(missing.path() / L"没有这个文件.wav", get_failure);
        registry.check(bitmap == nullptr && !get_failure.empty(), "文件不存在 → 明确失败");
    }

    // =======================================================================
    // 大图会被缩到上限之内
    // =======================================================================
    {
        const std::vector<std::uint8_t> big = encode_png_to_memory(1200, 900);
        registry.check(big.size() > 100, "生成一张 1200x900 的 PNG");

        TempDir large;
        const auto large_audio = large.path() / L"大封面.wav";
        registry.check(musicboard::test::write_test_wav(large_audio, 200), "写音频");
        registry.check(musicboard::test::write_bytes(large.path() / L"cover.png", big),
                       "放大封面");

        Failure get_failure;
        ID2D1Bitmap1* bitmap = cache.get(large_audio, get_failure);
        registry.check_eq(bitmap != nullptr, "大封面也能拿到", get_failure.to_string());
        if (bitmap != nullptr) {
            const D2D1_SIZE_U size = bitmap->GetPixelSize();
            registry.check(size.width <= CoverCache::kMaximumDecodedEdge &&
                               size.height <= CoverCache::kMaximumDecodedEdge,
                           "解码时缩到长边 " +
                               std::to_string(CoverCache::kMaximumDecodedEdge) + " 以内（不吃显存）");
            // 1200x900 → 512x384，宽高比要保持
            registry.check_eq(size.width == 512 && size.height == 384,
                              "缩放保持宽高比",
                              std::to_string(size.width) + "x" + std::to_string(size.height));
        }
    }

    // =======================================================================
    // 预算与淘汰
    // =======================================================================
    {
        TempDir many;
        std::vector<std::filesystem::path> paths;
        for (int i = 0; i < 4; ++i) {
            const std::wstring name = L"歌" + std::to_wstring(i) + L".wav";
            const auto item_audio = many.path() / name;
            registry.check(musicboard::test::write_test_wav(item_audio, 100), "写音频");
            const std::wstring cover_name = L"歌" + std::to_wstring(i) + L".png";
            registry.check(musicboard::test::write_bytes(many.path() / cover_name, png),
                           "写同名封面");
            paths.push_back(item_audio);
        }

        CoverCache limited;
        Failure init_failure;
        registry.check(limited.initialize(device.context(), init_failure), "初始化受限缓存");
        // 64x48 的预乘 BGRA = 12288 字节。预算只够放三张。
        limited.set_byte_budget(12288u * 3u + 1000u);

        int fetched = 0;
        for (const auto& path : paths) {
            Failure get_failure;
            if (limited.get(path, get_failure) != nullptr) {
                ++fetched;
            }
        }
        registry.check(fetched == 4, "四张都能拿到（预算再紧也不能拿不到刚取的那张）");
        registry.check(limited.entry_count() == 3, "缓存里只留下三张（超预算的旧图被淘汰）");
        registry.check(limited.stats().evictions == 1, "淘汰了一次");
        registry.check(limited.stats().bytes_cached <= 12288u * 3u + 1000u,
                       "占用没有超过预算");

        // 被淘汰的那张要重新解码 —— 这才证明淘汰是真的发生了
        const std::size_t misses_before = limited.stats().misses;
        Failure get_failure;
        registry.check(limited.get(paths.front(), get_failure) != nullptr, "再取第一张");
        registry.check(limited.stats().misses == misses_before + 1,
                       "被淘汰的那张重新解码了（eviction 不是只记账）");

        // 预算小到连一张都放不下时，仍然必须拿得到位图
        CoverCache tiny;
        registry.check(tiny.initialize(device.context(), init_failure), "初始化极小缓存");
        tiny.set_byte_budget(1);   // 一张都放不下
        Failure tiny_failure;
        registry.check_eq(tiny.get(paths.front(), tiny_failure) != nullptr,
                          "预算为 1 字节时仍然拿得到位图（不会返回空被误读成'没有封面'）",
                          tiny_failure.to_string());
        registry.check(tiny.entry_count() == 1, "而且至少留了一张在缓存里");
    }

    // =======================================================================
    // clear
    // =======================================================================
    {
        cache.clear();
        registry.check(cache.entry_count() == 0, "clear 之后缓存空了");
        registry.check(cache.stats().hits > 0, "统计不会被 clear 清掉（它记的是历史）");
        Failure get_failure;
        registry.check(cache.get(audio, get_failure) != nullptr, "clear 之后还能重新取");
    }

    return registry.finish("cover_cache_tests");
}
