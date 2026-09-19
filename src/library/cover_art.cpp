#include "library/cover_art.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "core/path_utf8.h"

namespace musicboard {
namespace {

// 内嵌标签的区域上限。ID3v2 标签声明多大就读多大，但不允许超过这个 ——
// 一个声称自己 2 GB 的标签是文件损坏或者恶意构造。
constexpr std::size_t kMaximumTagRegion = 24u * 1024u * 1024u;
// Ogg / MP4 这类"要在文件里找一找"的容器，最多看前这么多字节
constexpr std::size_t kMaximumSearchRegion = 2u * 1024u * 1024u;

// ---------------------------------------------------------------------------
// 只读文件的一段。**不把整个文件读进内存** ——
// 为了一张封面把 20 MB 的文件整个读进来，和"扫描阶段就 decode"是同一类错误。
// ---------------------------------------------------------------------------
class FileReader final {
public:
    FileReader() = default;
    ~FileReader() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
    }
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    [[nodiscard]] bool open(const std::filesystem::path& path, Failure& failure) {
        handle_ = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            failure.set(fail_code::file_open_failed,
                        "无法打开 " + path_to_utf8(path) + "（Win32 错误 " +
                            std::to_string(::GetLastError()) + "）");
            return false;
        }
        LARGE_INTEGER size{};
        if (::GetFileSizeEx(handle_, &size) == FALSE) {
            failure.set(fail_code::file_open_failed,
                        "读不到文件大小 " + path_to_utf8(path));
            return false;
        }
        size_ = static_cast<std::uint64_t>(size.QuadPart);
        return true;
    }

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }

    // 读 [offset, offset+size)。越界时返回实际读到的字节数（可能少于请求）。
    [[nodiscard]] bool read_at(std::uint64_t offset, std::size_t size,
                               std::vector<std::uint8_t>& out, Failure& failure) {
        out.clear();
        if (offset >= size_) {
            return true;   // 读不到就是空，不算失败
        }
        const std::uint64_t available = size_ - offset;
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(available, size));
        if (want == 0) {
            return true;
        }

        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (::SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN) == FALSE) {
            failure.set(fail_code::file_read_failed, "定位失败（Win32 错误 " +
                                                        std::to_string(::GetLastError()) + "）");
            return false;
        }

        out.resize(want);
        DWORD read = 0;
        if (::ReadFile(handle_, out.data(), static_cast<DWORD>(want), &read, nullptr) == FALSE) {
            failure.set(fail_code::file_read_failed,
                        "读取失败（Win32 错误 " + std::to_string(::GetLastError()) + "）");
            return false;
        }
        out.resize(read);
        return true;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::uint64_t size_ = 0;
};

[[nodiscard]] std::uint32_t read_be32(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
}

// ID3v2 的"同步安全整数"：每字节只用低 7 位
[[nodiscard]] std::uint32_t read_syncsafe32(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0] & 0x7F) << 21) |
           (static_cast<std::uint32_t>(bytes[1] & 0x7F) << 14) |
           (static_cast<std::uint32_t>(bytes[2] & 0x7F) << 7) |
           static_cast<std::uint32_t>(bytes[3] & 0x7F);
}

// ---------------------------------------------------------------------------
// base64（Vorbis 注释里的封面是 base64 的）
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<std::uint8_t> base64_decode(std::string_view text, std::size_t limit) {
    static constexpr std::array<std::int8_t, 256> kTable = [] {
        std::array<std::int8_t, 256> table{};
        table.fill(-1);
        const std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (std::size_t i = 0; i < alphabet.size(); ++i) {
            table[static_cast<unsigned char>(alphabet[i])] = static_cast<std::int8_t>(i);
        }
        return table;
    }();

    std::vector<std::uint8_t> out;
    out.reserve(std::min<std::size_t>(text.size() * 3 / 4, limit));

    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char ch : text) {
        if (ch == '=') {
            break;
        }
        const auto value = kTable[static_cast<unsigned char>(ch)];
        if (value < 0) {
            continue;   // 跳过换行、空白与其他杂字符
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
            if (out.size() >= limit) {
                // 超过上限就停下并**如实报告**（调用方会看到 bytes 非空但被截断），
                // 后面的解析会因此失败 —— 这正是我们要的：不静默接受一个半张图。
                return out;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// ID3v2（MP3）
// ---------------------------------------------------------------------------

// 把整个标签区域读进来，并把"未同步"（unsynchronisation）还原。
// 那个变换是 0xFF 0x00 → 0xFF，写标签的编码器为了避开假同步字会做它。
[[nodiscard]] bool read_id3v2_tag(FileReader& reader, std::vector<std::uint8_t>& tag,
                                 bool& has_unsync, Failure& failure) {
    std::vector<std::uint8_t> header;
    if (!reader.read_at(0, 10, header, failure)) {
        return false;
    }
    if (header.size() < 10 || header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        return true;   // 不是 ID3v2，tag 保持空
    }
    const std::uint32_t declared = read_syncsafe32(&header[6]);
    const std::size_t total = 10 + declared;
    if (total > kMaximumTagRegion) {
        failure.set(fail_code::file_too_large,
                    "ID3v2 标签声称有 " + std::to_string(total) + " 字节，超过上限 " +
                        std::to_string(kMaximumTagRegion) + " —— 当作文件损坏处理");
        return false;
    }
    has_unsync = (header[5] & 0x80) != 0;

    if (!reader.read_at(0, total, tag, failure)) {
        return false;
    }
    if (has_unsync) {
        std::vector<std::uint8_t> fixed;
        fixed.reserve(tag.size());
        for (std::size_t i = 0; i < tag.size(); ++i) {
            fixed.push_back(tag[i]);
            if (tag[i] == 0xFF && i + 1 < tag.size() && tag[i + 1] == 0x00) {
                ++i;   // 吃掉补进来的那个 0x00
            }
        }
        tag = std::move(fixed);
    }
    return true;
}

// 在 APIC（v2.3/2.4）或 PIC（v2.2）帧里取出图片
[[nodiscard]] bool parse_apic_frame(const std::uint8_t* body, std::size_t size, bool version_22,
                                   CoverArt& cover) {
    if (size < 4) {
        return false;
    }
    std::size_t offset = 1;   // 第一个字节是文本编码

    if (version_22) {
        if (offset + 3 > size) {
            return false;
        }
        // v2.2 的 PIC 帧写的是三字母格式名（"JPG"/"PNG"）
        const std::string_view image_format(reinterpret_cast<const char*>(body + offset), 3);
        cover.mime = image_format == "PNG" ? "image/png" : "image/jpeg";
        offset += 3;
    } else {
        // MIME，以 \0 结尾
        const std::size_t mime_end = [&] {
            for (std::size_t i = offset; i < size; ++i) {
                if (body[i] == 0x00) {
                    return i;
                }
            }
            return size;
        }();
        if (mime_end >= size) {
            return false;
        }
        cover.mime.assign(reinterpret_cast<const char*>(body + offset), mime_end - offset);
        offset = mime_end + 1;
    }

    if (offset >= size) {
        return false;
    }
    ++offset;   // 图片类型（0x03 = 封面）

    // 描述字段：按编码决定用 \0 还是 \0\0 结尾。UTF-16 要按两字节找。
    const std::uint8_t encoding = body[0];
    if (encoding == 0x01 || encoding == 0x02) {
        bool terminated = false;
        while (offset + 1 < size) {
            if (body[offset] == 0x00 && body[offset + 1] == 0x00) {
                offset += 2;
                terminated = true;
                break;
            }
            offset += 2;
        }
        if (!terminated) {
            return false;
        }
    } else {
        while (offset < size && body[offset] != 0x00) {
            ++offset;
        }
        if (offset < size) {
            ++offset;   // 跳过结尾的 \0
        }
    }

    if (offset >= size) {
        return false;
    }
    const std::size_t image_size = size - offset;
    if (image_size == 0 || image_size > kMaximumCoverBytes) {
        return false;
    }
    cover.bytes.assign(body + offset, body + size);
    cover.format = sniff_image_format(cover.bytes.data(), cover.bytes.size());
    cover.source = version_22 ? "id3v2.2" : "id3v2";
    return true;
}

[[nodiscard]] bool extract_from_id3v2(FileReader& reader, CoverArt& cover, Failure& failure) {
    std::vector<std::uint8_t> tag;
    bool has_unsync = false;
    if (!read_id3v2_tag(reader, tag, has_unsync, failure)) {
        return false;
    }
    if (tag.size() < 10) {
        return false;
    }

    const std::uint8_t version_major = tag[3];
    const bool version_22 = version_major == 2;
    const std::size_t id_size = version_22 ? 3 : 4;
    const std::size_t frame_header_size = version_22 ? 6 : 10;

    std::size_t offset = 10;
    // 扩展头：跳过它声明的长度
    if ((tag[5] & 0x40) != 0 && version_major >= 3 && offset + 4 <= tag.size()) {
        const std::size_t extended =
            version_major == 4 ? read_syncsafe32(&tag[offset]) : read_be32(&tag[offset]);
        offset += extended;
    }

    while (offset + frame_header_size <= tag.size()) {
        const std::uint8_t* header = tag.data() + offset;
        // 帧 ID 全 0 表示填充区，到此为止
        if (header[0] == 0x00) {
            break;
        }
        const std::string_view frame_id(reinterpret_cast<const char*>(header), id_size);
        const std::uint32_t frame_size =
            version_22 ? ((static_cast<std::uint32_t>(header[3]) << 16) |
                          (static_cast<std::uint32_t>(header[4]) << 8) |
                          static_cast<std::uint32_t>(header[5]))
                       : (version_major == 4 ? read_syncsafe32(header + 4) : read_be32(header + 4));
        offset += frame_header_size;

        if (frame_size == 0 || offset + frame_size > tag.size()) {
            break;   // 尺寸不合理，别再往前走了
        }
        const bool is_picture = version_22 ? (frame_id == "PIC") : (frame_id == "APIC");
        if (is_picture) {
            if (parse_apic_frame(tag.data() + offset, frame_size, version_22, cover)) {
                return true;
            }
            // 这一帧读不出来就继续找 —— 有的文件里有多张图
        }
        offset += frame_size;
    }
    return false;
}

// ---------------------------------------------------------------------------
// FLAC：PICTURE 元数据块（类型 6）
// ---------------------------------------------------------------------------

// FLAC 的 PICTURE 块体与 Vorbis 注释里 METADATA_BLOCK_PICTURE 的内容是同一个结构
[[nodiscard]] bool parse_flac_picture_block(const std::uint8_t* body, std::size_t size,
                                           const std::string& source, CoverArt& cover) {
    std::size_t offset = 0;
    const auto read_u32 = [&](std::uint32_t& out) {
        if (offset + 4 > size) {
            return false;
        }
        out = read_be32(body + offset);
        offset += 4;
        return true;
    };
    const auto skip = [&](std::uint32_t count) {
        if (offset + count > size) {
            return false;
        }
        offset += count;
        return true;
    };

    std::uint32_t picture_type = 0;
    if (!read_u32(picture_type)) {
        return false;
    }
    std::uint32_t mime_length = 0;
    if (!read_u32(mime_length)) {
        return false;
    }
    if (mime_length > 256) {
        return false;
    }
    if (offset + mime_length > size) {
        return false;
    }
    cover.mime.assign(reinterpret_cast<const char*>(body + offset), mime_length);
    offset += mime_length;

    std::uint32_t description_length = 0;
    if (!read_u32(description_length) || !skip(description_length)) {
        return false;
    }
    // 宽 / 高 / 色深 / 索引色数
    for (int i = 0; i < 4; ++i) {
        std::uint32_t ignored = 0;
        if (!read_u32(ignored)) {
            return false;
        }
    }
    std::uint32_t data_length = 0;
    if (!read_u32(data_length)) {
        return false;
    }
    if (data_length == 0 || data_length > kMaximumCoverBytes || offset + data_length > size) {
        return false;
    }
    cover.bytes.assign(body + offset, body + offset + data_length);
    cover.format = sniff_image_format(cover.bytes.data(), cover.bytes.size());
    cover.source = source;
    return true;
}

[[nodiscard]] bool extract_from_flac(FileReader& reader, CoverArt& cover, Failure& failure) {
    std::uint64_t offset = 4;   // 跳过 "fLaC"
    for (;;) {
        std::vector<std::uint8_t> block_header;
        if (!reader.read_at(offset, 4, block_header, failure)) {
            return false;
        }
        if (block_header.size() < 4) {
            return false;   // 文件到此为止，没有 PICTURE 块
        }
        const bool is_last = (block_header[0] & 0x80) != 0;
        const std::uint8_t type = block_header[0] & 0x7F;
        const std::uint32_t length = (static_cast<std::uint32_t>(block_header[1]) << 16) |
                                     (static_cast<std::uint32_t>(block_header[2]) << 8) |
                                     static_cast<std::uint32_t>(block_header[3]);
        offset += 4;

        if (type == 6) {   // PICTURE
            if (length > kMaximumCoverBytes + 1024) {
                failure.set(fail_code::file_too_large,
                            "FLAC 的 PICTURE 块声明 " + std::to_string(length) +
                                " 字节，超过封面大小上限");
                return false;
            }
            std::vector<std::uint8_t> body;
            if (!reader.read_at(offset, length, body, failure)) {
                return false;
            }
            if (parse_flac_picture_block(body.data(), body.size(), "flac", cover)) {
                return true;
            }
            // 这一块不合格就继续找下一块
        }
        if (is_last) {
            return false;
        }
        offset += length;
    }
}

// ---------------------------------------------------------------------------
// Ogg Vorbis：注释块里的 METADATA_BLOCK_PICTURE（base64）
//
// **刻意不做完整的 Ogg 页重组。** 那个字段名只可能出现在注释头里，
// 而注释头总在文件最前面。在开头一段里直接找字段名、再解它后面的 base64，
// 比实现一个页重组器（27 字节页头 + 段表 + 跨页拼包）简单得多，
// 而且不会因为一个畸形页而整条路失败。
// 代价：如果注释头大到超出搜索窗口，就找不到封面 —— 这是有界的退化，不是错误。
// ---------------------------------------------------------------------------
[[nodiscard]] bool extract_from_vorbis(FileReader& reader, CoverArt& cover, Failure& failure) {
    const std::uint64_t region =
        std::min<std::uint64_t>(reader.size(), kMaximumSearchRegion);
    std::vector<std::uint8_t> head;
    if (!reader.read_at(0, static_cast<std::size_t>(region), head, failure)) {
        return false;
    }

    static constexpr std::string_view kField = "METADATA_BLOCK_PICTURE=";
    const auto* begin = head.data();
    const auto* end = head.data() + head.size();
    const auto* found = std::search(begin, end, kField.begin(), kField.end());
    if (found == end) {
        return false;
    }
    const char* base64_begin = reinterpret_cast<const char*>(found + kField.size());
    const std::size_t remaining = static_cast<std::size_t>(end - found) - kField.size();

    // base64 一直延伸到第一个非 base64 字符（换行、\0、其他字段名）
    std::size_t length = 0;
    while (length < remaining) {
        const char ch = base64_begin[length];
        const bool is_base64 = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                               (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' || ch == '=';
        if (!is_base64) {
            break;
        }
        ++length;
    }
    if (length == 0) {
        return false;
    }

    std::vector<std::uint8_t> picture =
        base64_decode(std::string_view(base64_begin, length), kMaximumCoverBytes + 1024);
    if (picture.size() <= 8) {
        return false;
    }
    return parse_flac_picture_block(picture.data(), picture.size(), "vorbis", cover);
}

// ---------------------------------------------------------------------------
// MP4 / M4A：moov.udta.meta.ilst.covr
//
// 递归找 atom。meta 是个特例：它有 4 字节的版本/标志，子 atom 从 +4 开始。
// ---------------------------------------------------------------------------
constexpr int kMaximumAtomDepth = 8;

[[nodiscard]] bool find_mp4_cover(FileReader& reader, std::uint64_t begin, std::uint64_t end,
                                 int depth, CoverArt& cover, Failure& failure) {
    if (depth > kMaximumAtomDepth || end <= begin) {
        return false;
    }
    std::uint64_t offset = begin;
    while (offset + 8 <= end) {
        std::vector<std::uint8_t> header;
        if (!reader.read_at(offset, 8, header, failure)) {
            return false;
        }
        if (header.size() < 8) {
            return false;
        }
        std::uint64_t atom_size = read_be32(header.data());
        const std::string_view type(reinterpret_cast<const char*>(header.data() + 4), 4);
        std::uint64_t header_size = 8;
        if (atom_size == 1) {
            // 64 位长度
            std::vector<std::uint8_t> extended;
            if (!reader.read_at(offset + 8, 8, extended, failure) || extended.size() < 8) {
                return false;
            }
            atom_size = (static_cast<std::uint64_t>(read_be32(extended.data())) << 32) |
                        read_be32(extended.data() + 4);
            header_size = 16;
        } else if (atom_size == 0) {
            atom_size = end - offset;   // 一直到容器末尾
        }
        if (atom_size < header_size || offset + atom_size > end) {
            return false;   // 尺寸不合理
        }

        const std::uint64_t body_begin = offset + header_size;
        const std::uint64_t body_end = offset + atom_size;

        if (type == "covr") {
            // 子 atom 是 (size, type)，type 是 13 = JPEG / 14 = PNG
            std::uint64_t inner = body_begin;
            while (inner + 8 <= body_end) {
                std::vector<std::uint8_t> item;
                if (!reader.read_at(inner, 8, item, failure) || item.size() < 8) {
                    return false;
                }
                const std::uint64_t item_size = read_be32(item.data());
                if (item_size < 8 || inner + item_size > body_end) {
                    return false;
                }
                const std::uint32_t image_type = read_be32(item.data() + 4);
                const std::size_t data_size = static_cast<std::size_t>(item_size - 8);
                if (data_size > 0 && data_size <= kMaximumCoverBytes) {
                    std::vector<std::uint8_t> data;
                    if (!reader.read_at(inner + 8, data_size, data, failure)) {
                        return false;
                    }
                    cover.bytes = std::move(data);
                    cover.format = sniff_image_format(cover.bytes.data(), cover.bytes.size());
                    if (cover.format == CoverImageFormat::unknown) {
                        // 有些文件在 type 里写格式而不是靠魔数
                        cover.format = image_type == 14 ? CoverImageFormat::png
                                                        : CoverImageFormat::jpeg;
                    }
                    cover.source = "mp4";
                    return true;
                }
                inner += item_size;
            }
            return false;
        }

        // 这几个容器要继续往里走
        if (type == "moov" || type == "udta" || type == "ilst" || type == "trak" ||
            type == "mdia") {
            if (find_mp4_cover(reader, body_begin, body_end, depth + 1, cover, failure)) {
                return true;
            }
        } else if (type == "meta") {
            // meta 有 4 字节版本/标志，子 atom 从 +4 开始
            if (find_mp4_cover(reader, body_begin + 4, body_end, depth + 1, cover, failure)) {
                return true;
            }
        } else if (type == "stbl" || type == "minf") {
            if (find_mp4_cover(reader, body_begin, body_end, depth + 1, cover, failure)) {
                return true;
            }
        }

        offset = body_end;
    }
    return false;
}

[[nodiscard]] bool extract_from_mp4(FileReader& reader, CoverArt& cover, Failure& failure) {
    return find_mp4_cover(reader, 0, reader.size(), 0, cover, failure);
}

// ---------------------------------------------------------------------------
// 同目录封面文件
// ---------------------------------------------------------------------------
[[nodiscard]] bool read_whole_file_bounded(const std::filesystem::path& path,
                                          std::vector<std::uint8_t>& out, Failure& failure) {
    FileReader reader;
    if (!reader.open(path, failure)) {
        return false;
    }
    if (reader.size() == 0 || reader.size() > kMaximumCoverBytes) {
        failure.set(fail_code::file_too_large,
                    path_to_utf8(path) + " 的大小是 " + std::to_string(reader.size()) +
                        " 字节，不在封面文件的合理范围内");
        return false;
    }
    return reader.read_at(0, static_cast<std::size_t>(reader.size()), out, failure);
}

}  // namespace

const char* cover_format_name(CoverImageFormat format) noexcept {
    switch (format) {
        case CoverImageFormat::unknown:
            return "unknown";
        case CoverImageFormat::png:
            return "png";
        case CoverImageFormat::jpeg:
            return "jpeg";
        case CoverImageFormat::bmp:
            return "bmp";
        case CoverImageFormat::gif:
            return "gif";
        case CoverImageFormat::webp:
            return "webp";
    }
    return "unknown";
}

const char* cover_format_extension(CoverImageFormat format) noexcept {
    switch (format) {
        case CoverImageFormat::png:
            return ".png";
        case CoverImageFormat::jpeg:
            return ".jpg";
        case CoverImageFormat::bmp:
            return ".bmp";
        case CoverImageFormat::gif:
            return ".gif";
        case CoverImageFormat::webp:
            return ".webp";
        case CoverImageFormat::unknown:
            break;
    }
    return "";
}

CoverImageFormat sniff_image_format(const std::uint8_t* bytes, std::size_t size) noexcept {
    if (bytes == nullptr || size < 4) {
        return CoverImageFormat::unknown;
    }
    if (size >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') {
        return CoverImageFormat::png;
    }
    if (bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return CoverImageFormat::jpeg;
    }
    if (bytes[0] == 'B' && bytes[1] == 'M') {
        return CoverImageFormat::bmp;
    }
    if (bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F') {
        return CoverImageFormat::gif;
    }
    if (size >= 12 && std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "WEBP", 4) == 0) {
        return CoverImageFormat::webp;
    }
    return CoverImageFormat::unknown;
}

bool extract_embedded_cover(const std::filesystem::path& path, CoverArt& cover, Failure& failure) {
    cover = CoverArt{};

    // 容器按**内容**判断，不按扩展名（AGENTS.md 第 5 条边界）。
    std::vector<std::uint8_t> head;
    FileReader reader;
    if (!reader.open(path, failure)) {
        return false;
    }
    if (!reader.read_at(0, 64, head, failure)) {
        return false;
    }
    const ContainerProbe probe = probe_container(head);

    bool found = false;
    switch (probe.container) {
        case AudioContainer::mp3:
            found = extract_from_id3v2(reader, cover, failure);
            break;
        case AudioContainer::flac:
            found = extract_from_flac(reader, cover, failure);
            break;
        case AudioContainer::ogg:
            found = extract_from_vorbis(reader, cover, failure);
            break;
        case AudioContainer::mp4:
            found = extract_from_mp4(reader, cover, failure);
            break;
        case AudioContainer::wav: {
            // WAV 里可能挂一个 id3 chunk；按 ID3v2 的解析走，有就有、没有就算了
            found = extract_from_id3v2(reader, cover, failure);
            break;
        }
        default:
            // 其他容器（wma / aiff / unknown）目前不取内嵌封面。
            // 这不是静默失败：调用方会看到"没有封面"，界面回退黑胶。
            break;
    }

    if (!found || cover.bytes.empty()) {
        cover = CoverArt{};
        return false;
    }
    if (cover.format == CoverImageFormat::unknown) {
        // 抠出来的字节不是我们认识的图片格式 —— 与其把一段垃圾交给 WIC 去失败，
        // 不如在这里就说清楚。
        failure.set(fail_code::unsupported_format,
                    path_to_utf8(path) + " 的内嵌封面不是可识别的图片格式（来源 " +
                        cover.source + "，首字节 0x" +
                        [&] {
                            char buffer[8]{};
                            if (cover.bytes.size() >= 2) {
                                std::snprintf(buffer, sizeof(buffer), "%02X%02X", cover.bytes[0],
                                              cover.bytes[1]);
                            }
                            return std::string(buffer);
                        }() +
                        "）");
        cover = CoverArt{};
        return false;
    }
    return true;
}

std::vector<std::string> sidecar_cover_names(const std::string& audio_stem) {
    // 顺序即优先级。前几个是业界惯例（很多播放器与整理工具都认），
    // 最后两个是"和这首歌同名"的图。
    std::vector<std::string> names;
    static constexpr std::array<std::string_view, 6> kBases = {"cover", "folder", "front",
                                                              "album", "albumart", "thumb"};
    static constexpr std::array<std::string_view, 3> kExtensions = {".jpg", ".jpeg", ".png"};
    for (const auto& base : kBases) {
        for (const auto& extension : kExtensions) {
            names.push_back(std::string(base) + std::string(extension));
        }
    }
    if (!audio_stem.empty()) {
        for (const auto& extension : kExtensions) {
            names.push_back(audio_stem + std::string(extension));
        }
    }
    return names;
}

bool find_cover_art(const std::filesystem::path& audio_path, CoverArt& cover, Failure& failure) {
    // 1. 内嵌图
    {
        Failure embedded_failure;
        if (extract_embedded_cover(audio_path, cover, embedded_failure)) {
            return true;
        }
        if (!embedded_failure.empty()) {
            // "读的时候出了问题"要报出去；"没有内嵌封面"继续往下找同目录文件。
            // 两者必须分得开 —— 这是本模块契约里写明的。
            failure = embedded_failure;
            return false;
        }
    }

    // 2. 同目录封面文件
    const std::filesystem::path directory = audio_path.parent_path();
    const std::string stem = path_to_utf8(audio_path.stem());
    for (const std::string& name : sidecar_cover_names(stem)) {
        Failure name_failure;
        std::filesystem::path candidate;
        if (!path_from_utf8(name, candidate, name_failure)) {
            continue;
        }
        const std::filesystem::path full = directory / candidate;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(full, ec)) {
            continue;
        }
        Failure read_failure;
        std::vector<std::uint8_t> bytes;
        if (!read_whole_file_bounded(full, bytes, read_failure)) {
            failure = read_failure;
            return false;
        }
        const CoverImageFormat format = sniff_image_format(bytes.data(), bytes.size());
        if (format == CoverImageFormat::unknown) {
            // 名字像封面但内容不是图片 —— 说清楚，不静默跳过
            failure.set(fail_code::unsupported_format,
                        path_to_utf8(full) + " 的名字像封面文件，但内容不是可识别的图片格式");
            return false;
        }
        cover = CoverArt{};
        cover.bytes = std::move(bytes);
        cover.format = format;
        cover.source = "sidecar:" + name;
        return true;
    }

    // 3. 都没有 —— 正常结果，failure 保持为空
    cover = CoverArt{};
    return false;
}

}  // namespace musicboard
