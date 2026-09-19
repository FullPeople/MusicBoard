// SHA-256 测试
//
// 对着**公开的已知向量**测，而不是对着自己的实现测 —— 后者只能证明"前后一致"，
// 证明不了"算对了"。NIST 的向量是这类函数唯一有意义的验收标准。
//
// 向量来源：FIPS 180-4 的示例与 NIST 的 SHA-256 测试向量（"abc" 等经典值）。
// 另外测一件容易被忽略的事：**流式读文件与一次性算内存必须得到同一个值**，
// 因为 sha256_file_hex 是分块读的，分块边界错了短输入根本看不出来。

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/path_utf8.h"
#include "core/sha256.h"
#include "tests/test_files.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;
using musicboard::test::TempDir;

namespace {

std::string hex_of(std::string_view text) {
    std::string out;
    Failure failure;
    if (!sha256_hex(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), out,
                    failure)) {
        return "ERROR: " + failure.to_string();
    }
    return out;
}

}  // namespace

int main() {
    Registry registry;

    // =======================================================================
    // 已知向量
    // =======================================================================
    registry.check_eq(hex_of("") ==
                          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                      "空串的 SHA-256（众所周知的向量）", hex_of(""));
    registry.check_eq(hex_of("abc") ==
                          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                      "「abc」的 SHA-256", hex_of("abc"));
    registry.check_eq(
        hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "448 比特的经典向量", hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"));
    registry.check_eq(hex_of("The quick brown fox jumps over the lazy dog") ==
                          "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592",
                      "fox 向量", hex_of("The quick brown fox jumps over the lazy dog"));

    // 一百万个 'a'：分组边界与长度溢出的经典测例
    {
        std::string million(1000000, 'a');
        registry.check_eq(hex_of(million) ==
                              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                          "一百万个 'a' 的 SHA-256（多分组路径）",
                          hex_of(million).substr(0, 16) + "...");
    }

    // 长度前缀：55/56/64 字节是 SHA-256 分组填充的三个关键边界
    {
        Failure failure;
        const auto hash_of_length = [&failure](std::size_t length) {
            std::vector<std::uint8_t> data(length, 0x61);   // 全 'a'
            std::string out;
            Failure local;
            static_cast<void>(sha256_hex(data.data(), data.size(), out, local));
            failure = local;
            return out;
        };
        const std::string h55 = hash_of_length(55);
        const std::string h56 = hash_of_length(56);
        const std::string h64 = hash_of_length(64);
        registry.check(h55 != h56 && h56 != h64 && h55 != h64,
                       "55/56/64 字节给出三个不同的摘要（填充边界没写错）");
        registry.check(failure.empty(), "边界长度不报错");
        // 55 个 'a' 的已知值
        registry.check_eq(
            h55 == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",
            "55 个 'a' 的 SHA-256（单分组填满）", h55);
    }

    // =======================================================================
    // 摘要格式校验
    // =======================================================================
    registry.check(is_hex_digest(std::string(64, 'a')), "64 个十六进制字符是合法摘要");
    registry.check(is_hex_digest(std::string(64, 'F')), "大写也算合法");
    registry.check(!is_hex_digest(std::string(63, 'a')), "63 个字符不合法");
    registry.check(!is_hex_digest(std::string(65, 'a')), "65 个字符不合法");
    registry.check(!is_hex_digest(std::string(64, 'z')), "非十六进制字符不合法");
    registry.check(!is_hex_digest(""), "空串不合法");

    registry.check(digest_equals(std::string(64, 'a'), std::string(64, 'A')),
                   "摘要比较大小写不敏感");
    registry.check(!digest_equals("abc", "abd"), "不同的摘要不相等");
    registry.check(!digest_equals("abc", "ab"), "长度不同不相等");

    // =======================================================================
    // 文件哈希：与内存哈希必须一致
    // =======================================================================
    {
        TempDir temp;
        // 故意用比 256 KB 分块更大的数据，这样才真的走到多块路径
        const std::size_t size = 700u * 1024u + 123u;
        std::vector<std::uint8_t> data(size);
        for (std::size_t i = 0; i < size; ++i) {
            data[i] = static_cast<std::uint8_t>(i * 31u + 7u);
        }

        const auto path = temp.path() / L"大文件.bin";
        registry.check(musicboard::test::write_bytes(path, data), "写一个大文件（>256KB）");

        std::string memory_digest;
        Failure memory_failure;
        registry.check(sha256_hex(data.data(), data.size(), memory_digest, memory_failure),
                       "内存哈希算出来了");

        std::string file_digest;
        Failure file_failure;
        registry.check_invocation([&] { return sha256_file_hex(path, file_digest, file_failure); },
                                  "文件哈希算出来了", file_failure);
        registry.check_eq(digest_equals(memory_digest, file_digest),
                          "**流式读文件与一次性算内存得到同一个摘要**（分块边界没写错）",
                          memory_digest.substr(0, 16) + " vs " + file_digest.substr(0, 16));

        // 中文文件名也要能算（走的是宽字符路径）
        const auto chinese = temp.path() / L"带中文名的文件.bin";
        registry.check(musicboard::test::write_bytes(chinese, data), "写一个中文名的文件");
        std::string chinese_digest;
        Failure chinese_failure;
        registry.check_invocation(
            [&] { return sha256_file_hex(chinese, chinese_digest, chinese_failure); },
            "中文名文件的哈希算出来了", chinese_failure);
        registry.check(digest_equals(chinese_digest, memory_digest), "同一个内容，同一摘要");
    }
    {
        // 空文件
        TempDir temp;
        const auto path = temp.path() / L"空.bin";
        registry.check(musicboard::test::write_bytes(path, {}), "写一个空文件");
        std::string digest;
        Failure failure;
        registry.check_invocation([&] { return sha256_file_hex(path, digest, failure); },
                                  "空文件的哈希算出来了", failure);
        registry.check(digest == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                       "空文件的哈希与空串一致");
    }
    {
        // 文件不存在 → 明确失败
        TempDir temp;
        std::string digest;
        Failure failure;
        registry.check(!sha256_file_hex(temp.path() / L"没有这个文件.bin", digest, failure),
                       "文件不存在 → 失败");
        registry.check(failure.code == fail_code::file_open_failed, "错误码是 file_open_failed");
        registry.check(digest.empty(), "失败时不留下半个摘要");
    }
    {
        // 内容变一个字节，摘要就要变 —— 这是完整性校验的全部意义
        TempDir temp;
        const auto first = temp.path() / L"a.bin";
        const auto second = temp.path() / L"b.bin";
        registry.check(musicboard::test::write_text(first, "hello world"), "写第一个文件");
        registry.check(musicboard::test::write_text(second, "hello worle"), "写只差一个字节的第二个文件");

        std::string digest_a;
        std::string digest_b;
        Failure failure;
        static_cast<void>(sha256_file_hex(first, digest_a, failure));
        static_cast<void>(sha256_file_hex(second, digest_b, failure));
        registry.check(!digest_equals(digest_a, digest_b), "差一个字节 → 摘要完全不同");
    }

    // =======================================================================
    // 失败路径不留下半成品
    // =======================================================================
    {
        std::string out = "旧内容";
        Failure failure;
        // 传一个不存在的算法是测不到的（bcrypt 不会失败），所以这里只测
        // "失败时 out 被清空" 这条契约在文件路径上成立
        TempDir temp;
        out = "旧内容";
        Failure open_failure;
        static_cast<void>(sha256_file_hex(temp.path() / L"不存在.bin", out, open_failure));
        registry.check(out.empty(), "文件哈希失败时把输出清空（不留下上一次的值）");
        registry.check(!open_failure.empty(), "失败时给出原因");
    }

    return registry.finish("sha256_tests");
}
