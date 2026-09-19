// core 层测试：路径 UTF-8 边界、错误契约、文件读取、日志
//
// 这一层的价值全在"边界"上，所以测试直接冲着边界去：
// 中文路径、非法 UTF-8、不存在的文件、超限文件、JSON 转义。
//
// 本文件里凡是需要反斜杠或引号的地方，一律用字符码常量而不是字面量转义 ——
// 这个项目在中文 Windows 上开发，源码里的转义序列太容易在传递过程中被吃掉，
// 用 static_cast<char>(92) 这类写法读起来也更明确。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/failure.h"
#include "core/file_bytes.h"
#include "core/logger.h"
#include "core/path_utf8.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;

namespace {

constexpr char kBackslash = static_cast<char>(92);
constexpr char kQuote = static_cast<char>(34);
constexpr char kNewline = static_cast<char>(10);

// JSON 里一个被转义的引号
std::string escaped_quote() { return std::string{kBackslash, kQuote}; }
// JSON 里一个被转义的反斜杠
std::string escaped_backslash() { return std::string{kBackslash, kBackslash}; }

std::filesystem::path temp_dir() {
    static const std::filesystem::path dir = [] {
        std::filesystem::path d = std::filesystem::temp_directory_path() / L"musicboard_core_tests";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return dir;
}

// 用宽字符路径写文件，绕开窄字符 API 的编码问题
bool write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = ::WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                nullptr) != 0 &&
                    written == bytes.size();
    ::CloseHandle(handle);
    return ok;
}

void test_path_utf8(Registry& r) {
    // 中文 + 全角符号 + 方括号 + 空格：直接取自真实曲库里的文件名
    const std::filesystem::path path(L"[低][休整] 和平 水边 轻松｜Fast Food.mp3");
    const std::string utf8 = path_to_utf8(path);

    r.check(!utf8.empty(), "path_to_utf8: 中文路径能转出 UTF-8");
    r.check(utf8.find("休整") != std::string::npos, "path_to_utf8: 中文内容完整保留");
    r.check(utf8.find("\xEF\xBD\x9C") != std::string::npos, "path_to_utf8: 全角竖线按 UTF-8 编码");

    // 这一条是"为什么必须有单一边界"的现场证据：
    // path.string() 走本地代码页(936)，字节数与 UTF-8 不同，混用必坏。
    const std::size_t narrow_size = path.string().size();
    const std::size_t utf8_size = utf8.size();
    r.check_eq(narrow_size != utf8_size,
               "path.string() 与 path_to_utf8() 字节数不同（编码差异实证）",
               "narrow=" + std::to_string(narrow_size) + " utf8=" + std::to_string(utf8_size));

    std::filesystem::path round_tripped;
    Failure failure;
    r.check(path_from_utf8(utf8, round_tripped, failure), "path_from_utf8: 合法 UTF-8 能转回路径");
    r.check(round_tripped == path, "UTF-8 往返后路径完全一致");

    // 非法 UTF-8 必须明确失败，而不是替换成 U+FFFD 蒙混过去
    Failure bad_failure;
    std::filesystem::path dummy;
    const std::string invalid_utf8 = "\xC3\x28";   // 非法的两字节序列
    r.check(!path_from_utf8(invalid_utf8, dummy, bad_failure), "path_from_utf8: 非法 UTF-8 被拒绝");
    r.check(bad_failure.code == fail_code::invalid_argument, "非法 UTF-8 给出 invalid_argument 错误码");

    r.check(wide_to_utf8(L"音乐板") == "音乐板", "wide_to_utf8: 宽字符转 UTF-8 正确");
}

void test_failure_contract(Registry& r) {
    Failure failure;
    r.check(failure.empty(), "新建 Failure 是空的");

    failure.set("first.code", "第一层原因");
    failure.set("second.code", "第二层原因");
    r.check(failure.code == "first.code", "Failure::set 不覆盖已有原因（最内层优先）");
    r.check(failure.context == "第一层原因", "Failure::context 同样保持最内层");

    failure.clear();
    r.check(failure.empty() && failure.code.empty(), "Failure::clear 能清空");
    r.check(failure.to_string() == "ok", "空 Failure 的 to_string 是 ok");
}

void test_read_file(Registry& r) {
    const std::filesystem::path good = temp_dir() / L"正常文件.bin";
    const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
    r.check(write_bytes(good, payload), "测试用文件写入成功");

    std::vector<std::uint8_t> read_back;
    Failure failure;
    r.check(read_file_bytes(good, read_back, failure), "read_file_bytes: 读回成功");
    r.check(read_back == payload, "read_file_bytes: 内容一致");

    std::vector<std::uint8_t> head;
    Failure head_failure;
    r.check(read_file_head(good, 3, head, head_failure), "read_file_head: 读前 3 字节成功");
    r.check(head.size() == 3 && head[0] == 1 && head[2] == 3, "read_file_head: 内容正确");

    std::vector<std::uint8_t> over;
    Failure over_failure;
    r.check(read_file_head(good, 100, over, over_failure), "read_file_head: 请求长度超过文件时不失败");
    r.check(over.size() == payload.size(), "read_file_head: 返回实际长度");

    std::vector<std::uint8_t> missing;
    Failure missing_failure;
    r.check(!read_file_bytes(temp_dir() / L"不存在.mp3", missing, missing_failure),
            "read_file_bytes: 不存在的文件返回失败");
    r.check(missing_failure.code == fail_code::file_not_found, "不存在的文件给出 file_not_found");

    std::vector<std::uint8_t> limited;
    Failure limited_failure;
    r.check(!read_file_bytes(good, limited, limited_failure, 2), "超过上限的读取被拒绝");
    r.check(limited_failure.code == fail_code::file_too_large, "超限给出 file_too_large");
}

void test_logger(Registry& r) {
    const std::filesystem::path dir = temp_dir() / L"日志目录";
    std::filesystem::path log_path;

    // 必须先让 Logger 析构关掉写句柄，再去读它写出的文件。
    // Windows 的共享检查会挡住"自己还在写就自己读" —— 这是保护，不是缺陷。
    {
        Logger logger;
        Failure failure;
        r.check(logger.initialize_in_directory(dir, failure), "日志初始化成功");
        r.check(logger.is_open(), "日志文件已打开");
        if (!logger.is_open()) {
            return;
        }
        log_path = logger.log_path();

        // 中文、引号、反斜杠全塞进去，验证 JSONL 仍然可解析
        const std::string tricky = std::string("路径=") + kQuote + "C:" + kBackslash + "音乐" +
                                   kBackslash + "曲目.ogg" + kQuote + " 时长=12.5";
        logger.write(LogLevel::info, "audio.decoded", tricky);
        logger.write(LogLevel::error, "audio.failed", "容器=ogg 原因=解码失败");
    }

    std::vector<std::uint8_t> bytes;
    Failure read_failure;
    r.check_invocation([&] { return read_file_bytes(log_path, bytes, read_failure); }, "能读回日志文件", read_failure);
    if (bytes.empty()) {
        return;
    }
    const std::string text(bytes.begin(), bytes.end());

    r.check(text.find("\"level\":\"info\"") != std::string::npos, "日志含 level 字段");
    r.check(text.find("音乐") != std::string::npos, "日志里的中文是 UTF-8 原文，没有乱码");
    r.check(text.find(escaped_quote()) != std::string::npos, "日志里的引号被正确转义");
    r.check(text.find(escaped_backslash()) != std::string::npos, "日志里的反斜杠被正确转义");

    // 每行都以 } 收尾：粗验证 JSONL 结构没被换行破坏
    std::size_t line_count = 0;
    std::size_t closed = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == kNewline) {
            ++line_count;
            if (i > 0 && text[i - 1] == '}') {
                ++closed;
            }
        }
    }
    r.check_eq(line_count >= 2 && line_count == closed, "每行都是一个完整的 JSON 对象",
               "行数=" + std::to_string(line_count) + " 完整=" + std::to_string(closed));
}

}  // namespace

int main() {
    Registry r;
    test_path_utf8(r);
    test_failure_contract(r);
    test_read_file(r);
    test_logger(r);
    return r.finish("core_tests");
}
