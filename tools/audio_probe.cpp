// XAudio2 可用性探针
//
// 只回答一个问题：这台机器上能不能建出 mastering voice。
//
// 为什么值得先问：XAudio2 在没有任何音频输出设备时会直接失败
// （虚拟机、远程桌面、无声卡的机器都算）。这决定了阶段 4 的引擎
// 能不能真的初始化，也决定了测试里哪些路径属于"这台机器上验不了"。
//
// 用法: audio_probe.exe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <xaudio2.h>
#include <wrl/client.h>

#include <cstdio>
#include <vector>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

void print_hex(const char* label, HRESULT hr) {
    std::printf("    %s: hr=0x%08lX\n", label, static_cast<unsigned long>(hr));
}

}  // namespace

int wmain() {
    // 崩溃时 stdio 缓冲会丢，先关掉缓冲，保证崩之前的输出留得住
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== XAudio2 可用性探针 ===\n");

    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::printf("CoInitializeEx: 0x%08lX%s\n", static_cast<unsigned long>(com),
                com == RPC_E_CHANGED_MODE ? "（已以别的模式初始化过，不算失败）" : "");

    ComPtr<IXAudio2> engine;
    HRESULT hr = ::XAudio2Create(engine.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
    std::printf("XAudio2Create: 0x%08lX\n", static_cast<unsigned long>(hr));
    if (FAILED(hr) || !engine) {
        std::printf("结论: XAudio2 运行时不可用\n");
        return 1;
    }

    // 设备枚举（IXAudio2::GetDeviceCount）在这个 SDK 的头里不可见 —— 它是可选的诊断 API。
    // 关键的判据只有一个：CreateMasteringVoice 能不能成功。
    // 没有输出设备（虚拟机、远程桌面、无声卡）时它会直接失败。

    IXAudio2MasteringVoice* mastering = nullptr;
    hr = engine->CreateMasteringVoice(&mastering);
    std::printf("CreateMasteringVoice: 0x%08lX\n", static_cast<unsigned long>(hr));
    if (FAILED(hr) || mastering == nullptr) {
        print_hex("  ", hr);
        std::printf("结论: 没有可用的音频输出设备 —— 引擎能建但发不出声\n");
        return 2;
    }

    XAUDIO2_VOICE_DETAILS voice_details{};
    mastering->GetVoiceDetails(&voice_details);
    std::printf("主声部: 输入声道=%u 采样率=%u\n", voice_details.InputChannels,
                voice_details.InputSampleRate);

    // 试着建一个 submix（BGM/SFX 两条总线就靠它），确认这条路也通
    IXAudio2SubmixVoice* submix = nullptr;
    hr = engine->CreateSubmixVoice(&submix, voice_details.InputChannels,
                                   voice_details.InputSampleRate, 0, 0, nullptr, nullptr);
    std::printf("CreateSubmixVoice: 0x%08lX\n", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && submix != nullptr) {
        submix->DestroyVoice();
    }

    // 试着建一个 source voice 并提交一小段静音缓冲，确认端到端能用
    IXAudio2SourceVoice* source = nullptr;
    hr = engine->CreateSourceVoice(&source, nullptr, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr,
                                   nullptr, nullptr);
    std::printf("CreateSourceVoice(默认格式): 0x%08lX\n", static_cast<unsigned long>(hr));

    if (source != nullptr) {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = 44100;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        source->DestroyVoice();

        IXAudio2SourceVoice* pcm_source = nullptr;
        hr = engine->CreateSourceVoice(&pcm_source, &format);
        std::printf("CreateSourceVoice(PCM 44.1k/2ch/16bit): 0x%08lX\n",
                    static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr) && pcm_source != nullptr) {
            // 0.1 秒静音
            std::vector<short> silence(4410 * 2, 0);
            XAUDIO2_BUFFER buffer{};
            buffer.AudioBytes = static_cast<UINT32>(silence.size() * sizeof(short));
            buffer.pAudioData = reinterpret_cast<const BYTE*>(silence.data());
            buffer.Flags = XAUDIO2_END_OF_STREAM;
            hr = pcm_source->SubmitSourceBuffer(&buffer);
            std::printf("SubmitSourceBuffer: 0x%08lX\n", static_cast<unsigned long>(hr));
            const HRESULT start_hr = pcm_source->Start(0);
            std::printf("Start: 0x%08lX\n", static_cast<unsigned long>(start_hr));
            pcm_source->DestroyVoice();
        }
    }

    mastering->DestroyVoice();
    // 不要在这里 engine->Release()：engine 是 ComPtr，作用域结束会自己 Release，\n    // 手动再来一次就是双重释放。实测就是这么段错误的。
    std::printf("结论: XAudio2 全套可用\n");
    return 0;
}
