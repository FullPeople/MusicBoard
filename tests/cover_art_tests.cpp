// 封面提取测试
//
// 分两段：
//   A. 合成素材 —— 在测试里手工拼出带内嵌封面的 ID3v2 / FLAC / Vorbis / MP4 文件，
//      以及同目录封面文件。每一种容器、每一个版本分支都能精确构造，
//      所以能测到"畸形标签""多张图""未同步""超大图片"这些真实文件里会有的情况。
//   B. 真实曲库 —— 由 MUSICBOARD_TEST_LIBRARY 指定。没有就 SKIP。
//      这一段回答的是"这套解析器在真素材上到底能抠出多少封面"，
//      以及"抠一张封面要多久"。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "core/path_utf8.h"
#include "library/cover_art.h"
#include "tests/test_files.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;
using musicboard::test::TempDir;

namespace {

// ---- 一个最小的合法 PNG：8 字节签名 + IHDR + IEND。够让魔数识别认出来。 ----
std::vector<std::uint8_t> png_bytes() {
    return {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
            // 假装是 IHDR 之类的内容，长度不重要 —— 这里只测"抠出来的字节对不对"
            0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R', 1, 2, 3, 4,
            'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
}

// JPEG：FF D8 FF E0 ... FF D9
std::vector<std::uint8_t> jpeg_bytes() {
    return {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01,
            0x11, 0x22, 0x33, 0x44, 0xFF, 0xD9};
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void append_syncsafe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 21) & 0x7F));
    out.push_back(static_cast<std::uint8_t>((value >> 14) & 0x7F));
    out.push_back(static_cast<std::uint8_t>((value >> 7) & 0x7F));
    out.push_back(static_cast<std::uint8_t>(value & 0x7F));
}

void append_ascii(std::vector<std::uint8_t>& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}

std::string base64_encode(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const std::uint8_t byte : bytes) {
        accumulator = (accumulator << 8) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(kAlphabet[(accumulator >> bits) & 0x3F]);
        }
    }
    if (bits > 0) {
        out.push_back(kAlphabet[(accumulator << (6 - bits)) & 0x3F]);
    }
    while (out.size() % 4 != 0) {
        out.push_back('=');
    }
    return out;
}

// ---- ID3v2.3 / 2.4 的 APIC 帧 ----
std::vector<std::uint8_t> make_apic_frame(const std::vector<std::uint8_t>& image,
                                         const std::string& mime) {
    std::vector<std::uint8_t> body;
    body.push_back(0x00);   // 文本编码 = Latin-1
    append_ascii(body, mime);
    body.push_back(0x00);   // MIME 结尾
    body.push_back(0x03);   // 图片类型 = 封面
    body.push_back(0x00);   // 描述为空（一个 \0 就够）
    body.insert(body.end(), image.begin(), image.end());
    return body;
}

// 组装一个完整的 MP3：ID3v2 头 + 若干帧 + 一小段假的音频数据
std::vector<std::uint8_t> make_mp3_with_cover(const std::vector<std::uint8_t>& image,
                                              std::uint8_t version_major = 3,
                                              bool unsynchronised = false) {
    const std::vector<std::uint8_t> body = make_apic_frame(image, "image/png");

    std::vector<std::uint8_t> frames;
    // 先放一个不相关的 TIT2 帧，"跳过非图片帧"这条逻辑才有东西可跳
    {
        std::vector<std::uint8_t> title;
        title.push_back(0x00);
        append_ascii(title, "A Title");
        append_ascii(frames, "TIT2");
        append_be32(frames, static_cast<std::uint32_t>(title.size()));
        frames.push_back(0x00);
        frames.push_back(0x00);
        frames.insert(frames.end(), title.begin(), title.end());
    }
    append_ascii(frames, "APIC");
    if (version_major == 4) {
        append_syncsafe32(frames, static_cast<std::uint32_t>(body.size()));
    } else {
        append_be32(frames, static_cast<std::uint32_t>(body.size()));
    }
    frames.push_back(0x00);
    frames.push_back(0x00);
    frames.insert(frames.end(), body.begin(), body.end());

    std::vector<std::uint8_t> tag;
    append_ascii(tag, "ID3");
    tag.push_back(version_major);
    tag.push_back(0x00);
    tag.push_back(unsynchronised ? 0x80 : 0x00);
    append_syncsafe32(tag, static_cast<std::uint32_t>(frames.size()));
    tag.insert(tag.end(), frames.begin(), frames.end());

    // 一小段假的音频帧（MPEG 帧同步 0xFF 0xFB），让嗅探也认得出这是 mp3
    tag.push_back(0xFF);
    tag.push_back(0xFB);
    tag.insert(tag.end(), 400, 0x00);
    return tag;
}

// ---- FLAC 的 PICTURE 元数据块 ----
std::vector<std::uint8_t> make_flac_picture_body(const std::vector<std::uint8_t>& image,
                                                const std::string& mime) {
    std::vector<std::uint8_t> body;
    append_be32(body, 3);   // 图片类型 = 封面
    append_be32(body, static_cast<std::uint32_t>(mime.size()));
    append_ascii(body, mime);
    append_be32(body, 0);   // 描述长度
    append_be32(body, 512);   // 宽
    append_be32(body, 512);   // 高
    append_be32(body, 24);    // 色深
    append_be32(body, 0);     // 索引色数
    append_be32(body, static_cast<std::uint32_t>(image.size()));
    body.insert(body.end(), image.begin(), image.end());
    return body;
}

std::vector<std::uint8_t> make_flac_with_cover(const std::vector<std::uint8_t>& image) {
    const std::vector<std::uint8_t> picture = make_flac_picture_body(image, "image/png");

    std::vector<std::uint8_t> file;
    append_ascii(file, "fLaC");

    // STREAMINFO（类型 0，非最后一块），内容随便填 34 字节
    file.push_back(0x00);
    file.push_back(0x00);
    file.push_back(0x00);
    file.push_back(34);
    file.insert(file.end(), 34, 0x00);

    // PICTURE（类型 6，最后一块 —— 高位标志置 1）
    file.push_back(static_cast<std::uint8_t>(0x80 | 6));
    const std::uint32_t length = static_cast<std::uint32_t>(picture.size());
    file.push_back(static_cast<std::uint8_t>((length >> 16) & 0xFF));
    file.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFF));
    file.push_back(static_cast<std::uint8_t>(length & 0xFF));
    file.insert(file.end(), picture.begin(), picture.end());
    return file;
}

// ---- Ogg Vorbis：注释头里放 METADATA_BLOCK_PICTURE=base64 ----
std::vector<std::uint8_t> make_ogg_with_cover(const std::vector<std::uint8_t>& image) {
    const std::vector<std::uint8_t> picture = make_flac_picture_body(image, "image/png");
    const std::string base64 = base64_encode(picture);

    std::vector<std::uint8_t> file;
    // 一个最小的 Ogg 页头（27 字节）让魔数嗅探认出来
    append_ascii(file, "OggS");
    file.push_back(0x00);
    file.push_back(0x02);
    for (int i = 0; i < 8; ++i) {
        file.push_back(0x00);   // granule
    }
    append_ascii(file, "\x01\x1E\x00\x00\x00\x00\x00\x00");
    file.push_back(0x01);   // 段数
    file.push_back(0x01);   // 段表：1 个段
    append_ascii(file, "\x01vorbis");   // 假装是识别包头

    // 注释包（这里是"不合法但足够真实"的一段）：字段就在里面
    append_ascii(file, "\x03vorbis");
    append_ascii(file, "\x00\x00\x00\x00");
    append_ascii(file, "METADATA_BLOCK_PICTURE=");
    append_ascii(file, base64);
    append_ascii(file, "\x01\x00\x00\x00\x00");
    return file;
}

// ---- MP4 / M4A：moov.udta.meta.ilst.covr ----
std::vector<std::uint8_t> make_mp4_with_cover(const std::vector<std::uint8_t>& image) {
    const auto atom = [](std::string_view type, const std::vector<std::uint8_t>& body) {
        std::vector<std::uint8_t> out;
        append_be32(out, static_cast<std::uint32_t>(body.size() + 8));
        append_ascii(out, type);
        out.insert(out.end(), body.begin(), body.end());
        return out;
    };

    std::vector<std::uint8_t> covr_body;
    append_be32(covr_body, static_cast<std::uint32_t>(image.size() + 8));
    append_ascii(covr_body, "jpeg");   // 有些文件这里写格式名
    covr_body.insert(covr_body.end(), image.begin(), image.end());

    std::vector<std::uint8_t> ilst_body = atom("covr", covr_body);

    std::vector<std::uint8_t> meta_body;
    append_be32(meta_body, 0);   // meta 的版本/标志
    const std::vector<std::uint8_t> ilst = atom("ilst", ilst_body);
    meta_body.insert(meta_body.end(), ilst.begin(), ilst.end());

    std::vector<std::uint8_t> udta_body = atom("meta", meta_body);
    std::vector<std::uint8_t> moov_body = atom("udta", udta_body);
    std::vector<std::uint8_t> file = atom("moov", moov_body);
    // 注意：atom 的 type 字段只有 4 字节。"ftyp" 之后是 body，
    // body 的头 4 字节才是主品牌（"M4A "）。写成 atom("ftypM4A ", ...) 会让
    // 声明长度比实际长度小 4 —— 解析器只能判定"尺寸不合理"然后放弃。
    const std::vector<std::uint8_t> ftyp =
        atom("ftyp", {'M', '4', 'A', ' ', 0x00, 0x00, 0x02, 0x00});
    file.insert(file.begin(), ftyp.begin(), ftyp.end());
    return file;
}

std::uint32_t read_syncsafe(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0] & 0x7F) << 21) |
           (static_cast<std::uint32_t>(bytes[1] & 0x7F) << 14) |
           (static_cast<std::uint32_t>(bytes[2] & 0x7F) << 7) |
           static_cast<std::uint32_t>(bytes[3] & 0x7F);
}

void append_syncsafe32_at(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
    out[offset + 0] = static_cast<std::uint8_t>((value >> 21) & 0x7F);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 14) & 0x7F);
    out[offset + 2] = static_cast<std::uint8_t>((value >> 7) & 0x7F);
    out[offset + 3] = static_cast<std::uint8_t>(value & 0x7F);
}

std::size_t count_covers_in(const std::filesystem::path& root, std::size_t& with_cover,
                            std::size_t& looked_at) {
    with_cover = 0;
    looked_at = 0;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(root, ec);
    if (ec) {
        return 0;
    }
    for (const auto& entry : it) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::string extension = path_to_utf8(entry.path().extension());
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (extension != ".mp3" && extension != ".ogg" && extension != ".wav" &&
            extension != ".flac" && extension != ".m4a") {
            continue;
        }
        ++looked_at;
        CoverArt cover;
        Failure failure;
        if (find_cover_art(entry.path(), cover, failure) && !cover.empty()) {
            ++with_cover;
        }
    }
    return with_cover;
}

}  // namespace

int main() {
    Registry registry;

    // =======================================================================
    // 图片魔数
    // =======================================================================
    {
        const auto png = png_bytes();
        const auto jpeg = jpeg_bytes();
        registry.check(sniff_image_format(png.data(), png.size()) == CoverImageFormat::png,
                       "PNG 魔数");
        registry.check(sniff_image_format(jpeg.data(), jpeg.size()) == CoverImageFormat::jpeg,
                       "JPEG 魔数");
        const std::vector<std::uint8_t> bmp = {'B', 'M', 0, 0};
        registry.check(sniff_image_format(bmp.data(), bmp.size()) == CoverImageFormat::bmp,
                       "BMP 魔数");
        const std::vector<std::uint8_t> gif = {'G', 'I', 'F', '8'};
        registry.check(sniff_image_format(gif.data(), gif.size()) == CoverImageFormat::gif,
                       "GIF 魔数");
        const std::vector<std::uint8_t> webp = {'R', 'I', 'F', 'F', 0, 0, 0, 0,
                                                'W', 'E', 'B', 'P'};
        registry.check(sniff_image_format(webp.data(), webp.size()) == CoverImageFormat::webp,
                       "WebP 魔数");
        const std::vector<std::uint8_t> junk = {0x00, 0x01, 0x02, 0x03};
        registry.check(sniff_image_format(junk.data(), junk.size()) == CoverImageFormat::unknown,
                       "垃圾字节 → unknown");
        registry.check(sniff_image_format(nullptr, 0) == CoverImageFormat::unknown,
                       "空指针 → unknown（不崩）");
        registry.check(std::string(cover_format_extension(CoverImageFormat::jpeg)) == ".jpg",
                       "JPEG 的扩展名是 .jpg");
        registry.check(std::string(cover_format_name(CoverImageFormat::png)) == "png",
                       "PNG 的名字是 png");
    }

    // =======================================================================
    // 同目录封面文件名
    // =======================================================================
    {
        const std::vector<std::string> names = sidecar_cover_names("月亮河");
        registry.check(!names.empty(), "有候选名");
        registry.check(names.front() == "cover.jpg", "第一优先是 cover.jpg");
        registry.check(std::find(names.begin(), names.end(), "folder.jpg") != names.end(),
                       "包含 folder.jpg（业界惯例）");
        registry.check(std::find(names.begin(), names.end(), "月亮河.jpg") != names.end(),
                       "包含与歌曲同名的图");
        registry.check(std::find(names.begin(), names.end(), "cover.png") != names.end(),
                       "包含 PNG");
        registry.check(sidecar_cover_names("").size() < names.size(),
                       "没有主干名时不生成同名候选");
    }

    // =======================================================================
    // ID3v2（MP3）
    // =======================================================================
    {
        TempDir temp;
        const auto image = png_bytes();
        const auto file = temp.path() / L"带封面.mp3";
        registry.check(musicboard::test::write_bytes(file, make_mp3_with_cover(image)),
                       "写一个带封面的 mp3");

        CoverArt cover;
        Failure failure;
        const bool found = extract_embedded_cover(file, cover, failure);
        registry.check_eq(found, "抠出了 ID3v2.3 的 APIC 封面", failure.to_string());
        registry.check(cover.bytes == image, "抠出来的字节与原始图片完全一致");
        registry.check(cover.format == CoverImageFormat::png, "识别出是 PNG");
        registry.check(cover.source == "id3v2", "来源标为 id3v2");
        registry.check(cover.mime == "image/png", "MIME 也读出来了");
    }
    {
        // v2.4 的帧长度是同步安全整数
        TempDir temp;
        const auto image = jpeg_bytes();
        const auto file = temp.path() / L"v24.mp3";
        registry.check(musicboard::test::write_bytes(file, make_mp3_with_cover(image, 4)),
                       "写一个 ID3v2.4 的 mp3");

        CoverArt cover;
        Failure failure;
        registry.check_invocation([&] { return extract_embedded_cover(file, cover, failure); }, "抠出了 ID3v2.4 的 APIC 封面（同步安全长度）", failure);
        registry.check(cover.bytes == image && cover.format == CoverImageFormat::jpeg,
                       "v2.4 的内容与格式都对");
    }
    {
        // 未同步（unsynchronisation）：整段标签里每个 0xFF 后面补一个 0x00，
        // 读的时候再删掉。JPEG 里天然有 FF，正好用来验证。
        //
        // 构造顺序很重要：先按正常尺寸建好帧，**再**对整段做未同步，
        // 最后用未同步之后的长度写标签头。顺序反了会得到一个自称长度
        // 与实际不符的标签 —— 那样测的就不是未同步，而是"截断的标签"。
        TempDir temp;
        const auto image = jpeg_bytes();
        std::vector<std::uint8_t> file_bytes = make_mp3_with_cover(image, 3, true);

        // 找到帧区（标签头之后到假音频帧之前）做未同步
        const std::size_t tag_total = 10 + read_syncsafe(file_bytes.data() + 6);
        std::vector<std::uint8_t> frames(file_bytes.begin() + 10, file_bytes.begin() + tag_total);
        std::vector<std::uint8_t> escaped;
        escaped.reserve(frames.size() + 8);
        for (const std::uint8_t byte : frames) {
            escaped.push_back(byte);
            if (byte == 0xFF) {
                escaped.push_back(0x00);
            }
        }

        std::vector<std::uint8_t> fixed;
        fixed.insert(fixed.end(), file_bytes.begin(), file_bytes.begin() + 10);
        append_syncsafe32_at(fixed, 6, static_cast<std::uint32_t>(escaped.size()));
        fixed.insert(fixed.end(), escaped.begin(), escaped.end());
        fixed.insert(fixed.end(), file_bytes.begin() + tag_total, file_bytes.end());

        const auto file = temp.path() / L"unsync.mp3";
        registry.check(musicboard::test::write_bytes(file, fixed), "写一个未同步的 mp3");

        CoverArt cover;
        Failure failure;
        registry.check_invocation([&] { return extract_embedded_cover(file, cover, failure); }, "未同步的标签也能读出来", failure);
        registry.check(cover.bytes == image, "未同步还原之后字节与原始图片一致");
        registry.check(cover.format == CoverImageFormat::jpeg, "还原之后仍认得出是 JPEG");
    }
    {
        // 没有 ID3 标签的 mp3：不是错误，只是"没有封面"
        TempDir temp;
        std::vector<std::uint8_t> plain;
        plain.push_back(0xFF);
        plain.push_back(0xFB);
        plain.insert(plain.end(), 500, 0x00);
        const auto file = temp.path() / L"无标签.mp3";
        registry.check(musicboard::test::write_bytes(file, plain), "写一个没有标签的 mp3");

        CoverArt cover;
        Failure failure;
        registry.check(!extract_embedded_cover(file, cover, failure),
                       "没有标签 → 没有封面");
        registry.check(failure.empty(),
                       "而且 failure 是空的（'没有封面'不是错误）");
    }
    {
        // 标签声明了一个荒谬的长度 → 明确报错，不当成"没有封面"
        TempDir temp;
        std::vector<std::uint8_t> broken;
        append_ascii(broken, "ID3");
        broken.push_back(3);
        broken.push_back(0);
        broken.push_back(0);
        // 声称有 200 MB
        broken.push_back(0x7F);
        broken.push_back(0x7F);
        broken.push_back(0x7F);
        broken.push_back(0x7F);
        broken.insert(broken.end(), 1000, 0x00);
        const auto file = temp.path() / L"坏标签.mp3";
        registry.check(musicboard::test::write_bytes(file, broken), "写一个标签长度荒谬的 mp3");

        CoverArt cover;
        Failure failure;
        registry.check(!extract_embedded_cover(file, cover, failure), "拒绝荒谬的标签长度");
        registry.check(failure.code == fail_code::file_too_large, "错误码是 file_too_large");
    }
    {
        // 内嵌的不是图片（说自己是 image/png，内容是文本）→ 明确报错
        TempDir temp;
        const std::string text = "这不是图片";
        const std::vector<std::uint8_t> not_an_image(text.begin(), text.end());
        const auto file = temp.path() / L"假封面.mp3";
        registry.check(musicboard::test::write_bytes(file, make_mp3_with_cover(not_an_image)),
                       "写一个内嵌内容不是图片的 mp3");

        CoverArt cover;
        Failure failure;
        registry.check(!extract_embedded_cover(file, cover, failure),
                       "内嵌内容不是图片 → 拒绝");
        registry.check(failure.code == fail_code::unsupported_format,
                       "错误码是 unsupported_format（而不是含糊的失败）");
        registry.check(failure.context.find("不是可识别的图片格式") != std::string::npos,
                       "原因说清了是图片格式的问题");
    }

    // =======================================================================
    // FLAC
    // =======================================================================
    {
        TempDir temp;
        const auto image = png_bytes();
        const auto file = temp.path() / L"封面.flac";
        registry.check(musicboard::test::write_bytes(file, make_flac_with_cover(image)),
                       "写一个带 PICTURE 块的 flac");

        CoverArt cover;
        Failure failure;
        registry.check_invocation([&] { return extract_embedded_cover(file, cover, failure); }, "抠出了 FLAC 的 PICTURE 封面", failure);
        registry.check(cover.bytes == image, "内容一致");
        registry.check(cover.source == "flac", "来源标为 flac");
        registry.check(cover.mime == "image/png", "MIME 读出来了");
    }
    {
        // 只有 STREAMINFO、没有 PICTURE 块
        TempDir temp;
        std::vector<std::uint8_t> file_bytes;
        append_ascii(file_bytes, "fLaC");
        file_bytes.push_back(0x80);   // 最后一块，类型 0
        file_bytes.push_back(0x00);
        file_bytes.push_back(0x00);
        file_bytes.push_back(34);
        file_bytes.insert(file_bytes.end(), 34, 0x00);
        const auto file = temp.path() / L"无封面.flac";
        registry.check(musicboard::test::write_bytes(file, file_bytes), "写一个没有 PICTURE 的 flac");

        CoverArt cover;
        Failure failure;
        registry.check(!extract_embedded_cover(file, cover, failure), "没有 PICTURE → 没有封面");
        registry.check(failure.empty(), "failure 仍然是空的");
    }

    // =======================================================================
    // Ogg Vorbis（base64）
    // =======================================================================
    {
        TempDir temp;
        const auto image = png_bytes();
        const auto file = temp.path() / L"封面.ogg";
        registry.check(musicboard::test::write_bytes(file, make_ogg_with_cover(image)),
                       "写一个带 METADATA_BLOCK_PICTURE 的 ogg");

        CoverArt cover;
        Failure failure;
        registry.check_invocation([&] { return extract_embedded_cover(file, cover, failure); }, "抠出了 Vorbis 注释里的封面（含 base64 解码）", failure);
        registry.check(cover.bytes == image, "base64 解码后的字节与原始图片一致");
        registry.check(cover.source == "vorbis", "来源标为 vorbis");
    }

    // =======================================================================
    // MP4 / M4A
    // =======================================================================
    {
        TempDir temp;
        const auto image = jpeg_bytes();
        const auto file = temp.path() / L"封面.m4a";
        registry.check(musicboard::test::write_bytes(file, make_mp4_with_cover(image)),
                       "写一个带 covr 的 m4a");

        CoverArt cover;
        Failure failure;
        registry.check_invocation([&] { return extract_embedded_cover(file, cover, failure); }, "抠出了 MP4 的 covr 封面（递归找 atom）", failure);
        registry.check(cover.bytes == image, "内容一致");
        registry.check(cover.format == CoverImageFormat::jpeg, "格式是 JPEG");
        registry.check(cover.source == "mp4", "来源标为 mp4");
    }
    {
        // 畸形 atom 尺寸：不能死循环、不能越界读
        TempDir temp;
        std::vector<std::uint8_t> file_bytes;
        append_be32(file_bytes, 4);   // 长度 4 < 头长度 8，非法
        append_ascii(file_bytes, "moov");
        file_bytes.insert(file_bytes.end(), 100, 0x00);
        const auto file = temp.path() / L"畸形.m4a";
        registry.check(musicboard::test::write_bytes(file, file_bytes), "写一个畸形 atom 的 m4a");

        CoverArt cover;
        Failure failure;
        registry.check(!extract_embedded_cover(file, cover, failure), "畸形 atom → 没有封面而不是死循环");
    }

    // =======================================================================
    // 同目录封面文件
    // =======================================================================
    {
        TempDir temp;
        const auto image = png_bytes();
        const auto audio = temp.path() / L"一首歌.mp3";
        registry.check(musicboard::test::write_bytes(audio, make_mp3_with_cover({})),
                       "写一个没有内嵌封面的 mp3（空的图片数据）");

        // 先只有 folder.png
        registry.check(musicboard::test::write_bytes(temp.path() / L"folder.png", image),
                       "放一个 folder.png");

        CoverArt cover;
        Failure failure;
        registry.check_invocation([&] { return find_cover_art(audio, cover, failure); }, "从同目录找到封面", failure);
        registry.check(cover.bytes == image, "内容一致");
        registry.check(cover.source == "sidecar:folder.png", "来源标明了是哪个文件");
    }
    {
        // cover.jpg 优先于 folder.png（顺序即优先级）
        TempDir temp;
        const auto png = png_bytes();
        const auto jpeg = jpeg_bytes();
        const auto audio = temp.path() / L"优先级.mp3";
        registry.check(musicboard::test::write_bytes(audio, make_mp3_with_cover({})), "写 mp3");
        registry.check(musicboard::test::write_bytes(temp.path() / L"cover.jpg", jpeg), "放 cover.jpg");
        registry.check(musicboard::test::write_bytes(temp.path() / L"folder.png", png),
                       "放 folder.png");

        CoverArt cover;
        Failure failure;
        registry.check(find_cover_art(audio, cover, failure), "找到封面");
        registry.check(cover.format == CoverImageFormat::jpeg &&
                           cover.source == "sidecar:cover.jpg",
                       "cover.jpg 赢了（候选顺序就是优先级）");
    }
    {
        // 与歌曲同名的图
        TempDir temp;
        const auto image = png_bytes();
        const auto audio = temp.path() / L"月亮河.ogg";
        registry.check(musicboard::test::write_bytes(audio, make_ogg_with_cover({})), "写 ogg");
        registry.check(musicboard::test::write_bytes(temp.path() / L"月亮河.png", image),
                       "放一张与歌同名的图");

        CoverArt cover;
        Failure failure;
        registry.check(find_cover_art(audio, cover, failure), "找到同名封面");
        registry.check(cover.source == "sidecar:月亮河.png", "来源正确（中文名也没问题）");
    }
    {
        // 名字像封面但内容不是图片 → 明确报错，不静默跳过
        TempDir temp;
        const auto audio = temp.path() / L"假图.mp3";
        registry.check(musicboard::test::write_bytes(audio, make_mp3_with_cover({})), "写 mp3");
        registry.check(musicboard::test::write_text(temp.path() / L"cover.jpg", "这不是图片"),
                       "放一个名叫 cover.jpg 的文本文件");

        CoverArt cover;
        Failure failure;
        registry.check(!find_cover_art(audio, cover, failure), "拒绝内容不是图片的封面文件");
        registry.check(failure.code == fail_code::unsupported_format,
                       "错误码是 unsupported_format");
    }
    {
        // 内嵌优先于同目录文件
        TempDir temp;
        const auto png = png_bytes();
        const auto jpeg = jpeg_bytes();
        const auto audio = temp.path() / L"内嵌优先.mp3";
        registry.check(musicboard::test::write_bytes(audio, make_mp3_with_cover(jpeg)), "写带封面的 mp3");
        registry.check(musicboard::test::write_bytes(temp.path() / L"cover.png", png),
                       "同时放一个 cover.png");

        CoverArt cover;
        Failure failure;
        registry.check(find_cover_art(audio, cover, failure), "找到封面");
        registry.check(cover.source == "id3v2" && cover.format == CoverImageFormat::jpeg,
                       "内嵌图赢过同目录文件（优先级正确）");
    }
    {
        // 什么都没有
        TempDir temp;
        const auto audio = temp.path() / L"光秃秃.mp3";
        std::vector<std::uint8_t> plain;
        plain.push_back(0xFF);
        plain.push_back(0xFB);
        plain.insert(plain.end(), 500, 0x00);
        registry.check(musicboard::test::write_bytes(audio, plain), "写一个没有任何封面的 mp3");

        CoverArt cover;
        Failure failure;
        registry.check(!find_cover_art(audio, cover, failure), "两条路都没有 → 没有封面");
        registry.check(failure.empty(), "failure 为空（界面该回退黑胶，不是报错）");
        registry.check(cover.empty(), "cover 是空的");
    }
    {
        // 文件不存在 → 这是真错误
        TempDir temp;
        CoverArt cover;
        Failure failure;
        registry.check(!find_cover_art(temp.path() / L"没有这个文件.mp3", cover, failure),
                       "文件不存在 → 失败");
        registry.check(!failure.empty(), "failure 非空（与'没有封面'分得开）");
    }

    // =======================================================================
    // 真实曲库
    // =======================================================================
    {
        wchar_t buffer[32768]{};
        const DWORD written = ::GetEnvironmentVariableW(L"MUSICBOARD_TEST_LIBRARY", buffer,
                                                       static_cast<DWORD>(std::size(buffer)));
        if (written == 0 || written >= std::size(buffer)) {
            registry.skip("真实曲库封面统计（没有设置 MUSICBOARD_TEST_LIBRARY）");
        } else {
            const std::filesystem::path root(buffer);
            std::error_code ec;
            if (!std::filesystem::is_directory(root, ec)) {
                registry.skip("真实曲库封面统计（目录不存在）");
            } else {
                std::size_t with_cover = 0;
                std::size_t looked_at = 0;
                const ULONGLONG started = ::GetTickCount64();
                count_covers_in(root, with_cover, looked_at);
                const double seconds =
                    static_cast<double>(::GetTickCount64() - started) / 1000.0;

                std::printf("     真实曲库封面: %zu / %zu 个文件有封面, 耗时 %.2f 秒\n", with_cover,
                            looked_at, seconds);

                registry.check(looked_at > 0, "真实曲库里找到了音频文件");
                registry.check(with_cover > 0,
                               "真实曲库里至少有一部分文件带封面（否则整条路就是白写的）");
                // 给一张封面要比对整库做一遍 probe 快得多 ——
                // 这是"只读标签区域"那条设计的直接体现。
                registry.check_eq(seconds < 30.0, "全库取封面在 30 秒以内（只读标签，不读整文件）",
                                  std::to_string(seconds) + " 秒");
            }
        }
    }

    return registry.finish("cover_art_tests");
}
