// 容器嗅探测试：纯函数、完全离线
//
// 冲着的核心风险是"扩展名不可信"：一个叫 .mp3 的 ogg 必须被认成 ogg，
// 否则会被交给错误的后端，然后表现为"文件坏了"。

#include <cstdint>
#include <string>
#include <vector>

#include "audio/container_probe.h"
#include "audio/decoder.h"   // container_is_decodable
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;

namespace {

std::vector<std::uint8_t> bytes_from(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

// 把 magic 放在指定偏移，前面补零
std::vector<std::uint8_t> with_magic(std::size_t offset, std::string_view magic, std::size_t total = 64) {
    std::vector<std::uint8_t> data(total, 0);
    for (std::size_t i = 0; i < magic.size() && offset + i < total; ++i) {
        data[offset + i] = static_cast<std::uint8_t>(magic[i]);
    }
    return data;
}

void test_known_containers(Registry& r) {
    // RIFF 后接 4 字节长度再接 WAVE
    auto wav = with_magic(0, "RIFF");
    const char wave[4] = {'W', 'A', 'V', 'E'};
    for (int i = 0; i < 4; ++i) wav[8 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(wave[i]);
    r.check(probe_container(wav).container == AudioContainer::wav, "RIFF/WAVE 识别为 wav");

    r.check(probe_container(with_magic(0, "OggS")).container == AudioContainer::ogg,
            "OggS 识别为 ogg");
    r.check(probe_container(with_magic(0, "fLaC")).container == AudioContainer::flac,
            "fLaC 识别为 flac");
    r.check(probe_container(with_magic(0, "ID3")).container == AudioContainer::mp3,
            "ID3 标签识别为 mp3");
    r.check(probe_container(with_magic(4, "ftyp")).container == AudioContainer::mp4,
            "偏移 4 的 ftyp 识别为 mp4");
    r.check(probe_container(with_magic(0, "FORM")).container != AudioContainer::aiff,
            "只有 FORM 没有 AIFF 时不算 aiff");

    auto aiff = with_magic(0, "FORM");
    aiff[8] = 'A'; aiff[9] = 'I'; aiff[10] = 'F'; aiff[11] = 'F';
    r.check(probe_container(aiff).container == AudioContainer::aiff, "FORM/AIFF 识别为 aiff");

    // ASF/WMA GUID 的前 8 字节
    const char asf[8] = {'\x30', '\x26', '\xB2', '\x75', '\x8E', '\x66', '\xCF', '\x11'};
    r.check(probe_container(with_magic(0, std::string_view(asf, 8))).container == AudioContainer::wma,
            "ASF GUID 识别为 wma");
}

void test_bare_mp3(Registry& r) {
    // 无 ID3 标签的 MP3：帧同步 0xFF 0xFB
    auto mp3 = with_magic(0, std::string_view("\xFF\xFB", 2));
    r.check(probe_container(mp3).container == AudioContainer::mp3, "MPEG 帧同步识别为 mp3");

    // 0xFF 0xF0 之类层字段为 0 的组合不算帧
    auto reserved = with_magic(0, std::string_view("\xFF\xE1", 2));
    r.check(probe_container(reserved).container != AudioContainer::mp3,
            "层字段为保留值的 0xFF 组合不误判为 mp3");
}

void test_unknown(Registry& r) {
    r.check(probe_container(bytes_from("这不是音频文件的内容，只是普通文本。")).container ==
                AudioContainer::unknown,
            "纯文本识别为 unknown");
    r.check(probe_container({}).container == AudioContainer::unknown, "空输入识别为 unknown");
    r.check(probe_container(bytes_from("Ogg")).container == AudioContainer::unknown,
            "字节不足时返回 unknown 而不是猜");
}

void test_extension_is_not_trusted(Registry& r) {
    // 这条是本模块存在的理由：内容说了算。
    // 名字叫 .mp3，内容却是 OggS —— 必须认成 ogg。
    const auto actually_ogg = with_magic(0, "OggS");
    r.check(probe_container(actually_ogg).container == AudioContainer::ogg,
            "内容是 OggS 时按 ogg 处理（不看扩展名）");
}

void test_decodable_coverage(Registry& r) {
    r.check(container_is_decodable(AudioContainer::ogg), "ogg 有可用后端");
    r.check(container_is_decodable(AudioContainer::mp3), "mp3 有可用后端");
    r.check(container_is_decodable(AudioContainer::wav), "wav 有可用后端");
    r.check(!container_is_decodable(AudioContainer::unknown), "unknown 明确报告不可解码");
}

void test_names(Registry& r) {
    r.check(std::string(container_name(AudioContainer::ogg)) == "ogg", "container_name(ogg)");
    r.check(std::string(container_name(AudioContainer::unknown)) == "unknown", "container_name(unknown)");
}

}  // namespace

int main() {
    Registry r;
    test_known_containers(r);
    test_bare_mp3(r);
    test_unknown(r);
    test_extension_is_not_trusted(r);
    test_decodable_coverage(r);
    test_names(r);
    return r.finish("container_probe_tests");
}
