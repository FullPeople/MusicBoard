// 曲库文件操作测试
//
// 这一层的绝大多数逻辑是**纯校验函数**，所以可以完全离线穷举 —— 不碰盘、不碰音频设备。
// 下面按"原版实际出过的问题"分组：
//
//   A. 非法字符 / 保留设备名 / 结尾的点与空格    → 原版只写控制台，界面表现为"点了没反应"
//   B. 分类名里的路径分隔符与 ".."              → 原版会真的建出嵌套目录，文件永远不显示
//   C. 路径包含关系判断                          → 挡住 "C:\abc" 被 "C:\ab" 前缀误判
//   D. 重名                                       → 永不覆盖，自动加 " (2)"
//   E. 删除                                       → 默认回收站；空分类才允许删
//
// 盘上操作全部在一个自己造的临时目录里做，析构时清干净。

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "core/failure.h"
#include "core/path_utf8.h"
#include "library/library_ops.h"
#include "tests/test_files.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;
using musicboard::test::TempDir;

namespace {

// 每个断言组都想要一个干净的 Failure，但又想顺便检查错误码 —— 用这个小工具统一。
struct Probe final {
    Failure failure;

    [[nodiscard]] bool stem_ok(std::string_view stem) {
        failure.clear();
        return validate_file_stem(stem, failure);
    }
    [[nodiscard]] bool stem_bad(std::string_view stem, std::string_view expected_code) {
        failure.clear();
        if (validate_file_stem(stem, failure)) {
            return false;
        }
        return failure.code == expected_code;
    }
    [[nodiscard]] bool category_bad(std::string_view name, std::string_view expected_code) {
        failure.clear();
        if (validate_category_name(name, failure)) {
            return false;
        }
        return failure.code == expected_code;
    }
};

constexpr std::string_view kInvalid = fail_code::invalid_argument;

}  // namespace

int main() {
    Registry registry;
    Probe probe;

    // =======================================================================
    // A. 文件名校验
    // =======================================================================

    // 中文名必须能过 —— 这是整个项目的命门，先立住这一条
    registry.check(probe.stem_ok("战斗音乐"), "中文文件名通过");
    registry.check(probe.stem_ok("01 - 主题曲 (Live)"), "带空格括号连字符通过");
    registry.check(probe.stem_ok("ＢＧＭ　全角"), "全角字符与全角空格通过");
    registry.check(probe.stem_ok("Track_With_Underscore-1"), "ASCII 常见写法通过");
    registry.check(probe.stem_ok("emoji 🎵 混排"), "UTF-8 emoji 通过");

    registry.check(probe.stem_bad("", kInvalid), "空文件名被拒");
    registry.check(probe.stem_bad("   ", kInvalid), "全是空格被拒");
    registry.check(probe.stem_bad("\t\t", kInvalid), "全是制表符被拒");
    registry.check(probe.stem_bad("　　", kInvalid), "全是全角空格被拒（不是 ASCII 空格，也判为全空白）");

    // 每个 Windows 非法字符单独来一遍 —— 逐个字符报错才能定位
    for (const char illegal : std::string_view("<>:\"/\\|?*")) {
        const std::string name = std::string("a") + illegal + "b";
        const bool rejected = probe.stem_bad(name, kInvalid);
        registry.check_eq(rejected, std::string("非法字符 '") + illegal + "' 被拒",
                          probe.failure.context);
    }

    // 合法 UTF-8 的多字节序列必须放行，截断/非法的必须拒绝 ——
    // 不先挡这一条，后面构造路径时会走到 MB_ERR_INVALID_CHARS 上去。
    registry.check(probe.stem_ok("\xE6\x88\x98\xE6\x96\x97"), "合法 UTF-8 多字节序列通过");
    registry.check(probe.stem_bad("\xE6\x88", kInvalid), "截断的 UTF-8 序列被拒");
    registry.check(probe.stem_bad("\xFF\xFE", kInvalid), "非 UTF-8 字节（0xFF 0xFE）被拒");
    registry.check(probe.stem_bad("\xE4\xBD\xA0\xC0\x80", kInvalid),
                   "过长的 UTF-8 续字节（overlong）被拒");

    registry.check(probe.stem_bad("含\x01控制符", kInvalid), "0x01 控制字符被拒");
    registry.check(probe.stem_bad("含\x1F控制符", kInvalid), "0x1F 控制字符被拒");
    registry.check(probe.stem_bad("含\x7F控制符", kInvalid), "0x7F DEL 被拒");
    registry.check(probe.stem_bad("结尾空格 ", kInvalid), "结尾空格被拒");
    registry.check(probe.stem_bad("结尾点.", kInvalid), "结尾点被拒");
    registry.check(probe.stem_bad(" 开头空格", kInvalid), "开头空格被拒");

    // 长度：上限是 200 字节，边界两侧各测一次
    registry.check(probe.stem_ok(std::string(maximum_file_stem_length, 'a')), "恰好 200 字节通过");
    registry.check(probe.stem_bad(std::string(maximum_file_stem_length + 1, 'a'), kInvalid),
                   "201 字节被拒");
    // UTF-8 按字节算：67 个中文字符 = 201 字节
    registry.check(probe.stem_bad(std::string(67, 'x') + "中文测试中文测试中文测试中文测试中文测试中文测试"
                                  "中文测试中文测试中文测试中文测试中文测试中文测试中文测试"
                                  "中文测试中文测试中文测试中文测试中文测试中文测试中文测试中文测试",
                                  kInvalid),
                   "中文字符按字节计长（UTF-8 不按字符数放宽）");

    // 保留设备名：22 个全测，且不分大小写
    const char* reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                              "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                              "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    int reserved_exact = 0;
    int reserved_lower = 0;
    for (const char* name : reserved) {
        if (probe.stem_bad(name, kInvalid)) {
            ++reserved_exact;
        }
        std::string lowered(name);
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (probe.stem_bad(lowered, kInvalid)) {
            ++reserved_lower;
        }
    }
    registry.check(reserved_exact == 22, "22 个保留设备名（大写）全部被拒");
    registry.check(reserved_lower == 22, "22 个保留设备名（小写）同样被拒");
    registry.check(probe.stem_bad("CoN", kInvalid), "保留名大小写混排也被拒");

    // 带扩展名的保留名同样是设备 —— Windows 认第一个点之前那一段
    registry.check(probe.stem_bad("CON.txt", kInvalid), "CON.txt 被拒（扩展名不能洗白设备名）");
    registry.check(probe.stem_bad("nul.wav", kInvalid), "nul.wav 被拒");
    // 但只是以保留名开头的普通名字必须放行
    registry.check(probe.stem_ok("CONSOLE"), "CONSOLE 通过（不是保留名的前缀匹配）");
    registry.check(probe.stem_ok("NULL 的曲子"), "NULL 开头的名字通过");
    registry.check(probe.stem_ok("COM10"), "COM10 通过（只保留 COM1..COM9）");

    // 分类名：额外拒绝 "." 与 ".."
    registry.check(probe.category_bad(".", kInvalid), "分类名 \".\" 被拒");
    registry.check(probe.category_bad("..", kInvalid), "分类名 \"..\" 被拒");
    registry.check(probe.stem_bad(".", kInvalid), "文件名 \".\" 被拒（结尾点）");
    registry.check(probe.category_bad("a/b", kInvalid), "分类名 \"a/b\" 被拒 —— 原版会真的建出嵌套目录");
    registry.check(probe.category_bad("a\\b", kInvalid), "分类名 \"a\\\\b\" 被拒");
    registry.check(probe.category_bad("..\\..\\Windows", kInvalid), "分类名里带 .. 逃逸被拒");
    {
        Failure f;
        registry.check(validate_category_name("战斗", f), "中文分类名通过");
        Failure f2;
        registry.check(validate_category_name("城镇 白天", f2), "带空格的分类名通过");
    }

    // =======================================================================
    // 名字净化
    // =======================================================================

    registry.check(sanitize_to_valid_name("a/b:c*d") == "a_b_c_d", "非法字符净化成下划线");
    registry.check(sanitize_to_valid_name("  两边空白  ") == "两边空白", "首尾空白被去掉");
    registry.check(sanitize_to_valid_name("结尾点...") == "结尾点", "结尾的点被去掉");
    registry.check(sanitize_to_valid_name("") == "未命名", "空串净化成\"未命名\"");
    registry.check(sanitize_to_valid_name("   ") == "未命名", "全空白净化成\"未命名\"");
    registry.check(sanitize_to_valid_name(std::string("a\x01") + "b") == "ab",
                   "控制字符被丢弃而不是替换");
    registry.check(sanitize_to_valid_name("CON") == "CON_", "保留名净化后补后缀");
    registry.check(sanitize_to_valid_name("con.txt") == "con.txt_", "con.txt 净化后补后缀");
    registry.check(sanitize_to_valid_name("正常名字") == "正常名字", "正常名字原样返回");

    {
        // 截断不能切在多字节序列中间。100 个中文 = 300 字节，上限 200：
        // 正确结果是 66 个字符（198 字节），第 67 个字符整体丢掉。
        const std::string long_chinese = [] {
            std::string s;
            for (int i = 0; i < 100; ++i) {
                s += "中";
            }
            return s;
        }();
        const std::string cut = sanitize_to_valid_name(long_chinese, 200);
        registry.check_eq(cut.size() == 198, "超长中文截断到 198 字节（不切断字符）",
                          std::to_string(cut.size()));
        // 逐字节验证 UTF-8 合法：不会出现孤立的续字节
        bool valid_utf8 = true;
        for (std::size_t i = 0; i < cut.size();) {
            const auto byte = static_cast<unsigned char>(cut[i]);
            std::size_t length = 0;
            if (byte < 0x80) {
                length = 1;
            } else if ((byte & 0xE0) == 0xC0) {
                length = 2;
            } else if ((byte & 0xF0) == 0xE0) {
                length = 3;
            } else if ((byte & 0xF8) == 0xF0) {
                length = 4;
            } else {
                valid_utf8 = false;
                break;
            }
            if (i + length > cut.size()) {
                valid_utf8 = false;
                break;
            }
            for (std::size_t k = 1; k < length; ++k) {
                if ((static_cast<unsigned char>(cut[i + k]) & 0xC0) != 0x80) {
                    valid_utf8 = false;
                    break;
                }
            }
            if (!valid_utf8) {
                break;
            }
            i += length;
        }
        registry.check(valid_utf8, "截断后的字节序列仍是合法 UTF-8");
    }

    // =======================================================================
    // C. 路径包含关系
    // =======================================================================

    {
        TempDir temp;
        const auto root = temp.path();
        const auto bgm = root / L"BGM";
        const auto inside = bgm / L"战斗";
        const auto sibling = root / L"BGMX";
        std::error_code ec;
        std::filesystem::create_directories(inside, ec);
        std::filesystem::create_directories(sibling, ec);

        registry.check(path_is_inside(root, inside), "子目录判为在内");
        registry.check(path_is_inside(bgm, inside), "分类目录判为在栏目之内");
        registry.check(!path_is_inside(root, root), "自己不算在自己之内");
        registry.check(!path_is_inside(bgm, sibling),
                       "\"BGMX\" 不被 \"BGM\" 前缀误判（逐组件比，不做字符串前缀）");
        registry.check(!path_is_inside(inside, root), "父目录不算在子目录之内");
        registry.check(path_is_inside(root, root / L"BGM" / L".." / L"BGM" / L"战斗"),
                       "带 .. 的相对路径规范化后仍在内部");
        registry.check(path_is_inside(root, root / L"新文件.ogg"), "尚不存在的子项也算在内");
    }

    // =======================================================================
    // D/E. 真实的盘上操作
    // =======================================================================

    {
        TempDir temp;
        const auto root = temp.path();
        const auto bgm = root / L"BGM";
        const auto sfx = root / L"SFX";
        std::error_code ec;
        std::filesystem::create_directories(bgm, ec);
        std::filesystem::create_directories(sfx, ec);

        // ---- create_category：原版那个 a/b bug 的正面验证 ----
        Failure failure;
        registry.check(!create_category(bgm, "a/b", failure) &&
                           failure.code == kInvalid,
                       "create_category(\"a/b\") 被拒（这是原版 bug 的防线）");
        registry.check(!std::filesystem::exists(bgm / L"a", ec), "被拒之后磁盘上没有多出 \"a\" 目录");

        failure.clear();
        registry.check(create_category(bgm, "战斗", failure), "创建中文分类成功");
        registry.check(std::filesystem::is_directory(bgm / L"战斗", ec), "分类目录真的在盘上");

        failure.clear();
        registry.check(!create_category(bgm, "战斗", failure) && failure.code == kInvalid,
                       "重复创建同名分类被拒");

        failure.clear();
        registry.check(!create_category(bgm / L"不存在", "新分类", failure) &&
                           failure.code == fail_code::file_not_found,
                       "在不存在的位置建分类被拒");

        // ---- rename_category ----
        failure.clear();
        registry.check(rename_category(bgm, "战斗", "激战", failure), "重命名分类成功");
        registry.check(std::filesystem::is_directory(bgm / L"激战", ec), "新分类名在盘上");
        registry.check(!std::filesystem::exists(bgm / L"战斗", ec), "旧分类名已不在");

        failure.clear();
        registry.check(create_category(bgm, "城镇", failure), "再建一个分类用于重名测试");
        failure.clear();
        registry.check(!rename_category(bgm, "激战", "城镇", failure) && failure.code == kInvalid,
                       "重命名到已存在的分类名被拒");
        failure.clear();
        registry.check(!rename_category(bgm, "激战", "激战", failure) && failure.code == kInvalid,
                       "重命名成同名被拒");

        // ---- 曲目：重命名 / 重名 / 移动 / 删除 ----
        const auto track = bgm / L"激战" / L"原曲.wav";
        registry.check(musicboard::test::write_text(track, "fake"), "准备一个测试文件");

        LibraryOpResult result;
        failure.clear();
        registry.check(rename_track(track, "新名字", result, failure), "重命名曲目成功");
        registry.check(result.resulting_path == bgm / L"激战" / L"新名字.wav", "扩展名保持 .wav");
        registry.check(!result.renamed_to_avoid_collision, "没有发生避让重名");

        failure.clear();
        registry.check(!rename_track(result.resulting_path, "不合法/名字", result, failure) &&
                           failure.code == kInvalid,
                       "重命名成非法名字被拒");
        registry.check(std::filesystem::exists(bgm / L"激战" / L"新名字.wav", ec),
                       "被拒之后原文件还在（不会改一半）");

        // 重名：先造一个同名文件，再重命名过去
        registry.check(musicboard::test::write_text(bgm / L"激战" / L"撞车.wav", "a"),
                       "准备撞车目标 1");
        registry.check(musicboard::test::write_text(bgm / L"激战" / L"撞车(2).wav", "b"),
                       "准备撞车目标 2");

        const auto mover = bgm / L"激战" / L"待改名.wav";
        registry.check(musicboard::test::write_text(mover, "c"), "准备待改名文件");
        failure.clear();
        registry.check(rename_track(mover, "撞车", result, failure), "重命名到已被占用的名字仍然成功");
        registry.check(result.renamed_to_avoid_collision, "并且报告了避让重名");
        registry.check(result.resulting_path == bgm / L"激战" / L"撞车(3).wav",
                       "避让结果是 撞车(3).wav（不是覆盖，也不是只加一个 (2)）");
        registry.check(musicboard::test::read_text(bgm / L"激战" / L"撞车.wav") == "a",
                       "原有文件内容未被覆盖");

        // ---- move_track ----
        const auto source = bgm / L"激战" / L"要搬走.wav";
        registry.check(musicboard::test::write_text(source, "d"), "准备待移动文件");

        failure.clear();
        registry.check(!move_track(source, bgm / L"激战", result, failure) &&
                           failure.code == kInvalid,
                       "移动到同一个目录被拒（不必移动）");

        failure.clear();
        registry.check(move_track(source, bgm / L"城镇", result, failure), "移动到另一个分类成功");
        registry.check(std::filesystem::exists(bgm / L"城镇" / L"要搬走.wav", ec), "新位置有文件");
        registry.check(!std::filesystem::exists(source, ec), "旧位置没有残留");

        failure.clear();
        registry.check(!move_track(source, bgm / L"城镇", result, failure) &&
                           failure.code == fail_code::file_not_found,
                       "移动一个不存在的文件被拒");

        failure.clear();
        registry.check(!move_track(bgm / L"城镇" / L"要搬走.wav", bgm / L"没有这个分类", result,
                                   failure) &&
                           failure.code == fail_code::file_not_found,
                       "移动到不存在的目标目录被拒");

        // ---- delete_empty_category ----
        failure.clear();
        registry.check(create_category(sfx, "空分类", failure), "建一个空分类");
        failure.clear();
        registry.check(delete_empty_category(sfx, "空分类", failure), "删除空分类成功");
        registry.check(!std::filesystem::exists(sfx / L"空分类", ec), "空分类真的没了");

        failure.clear();
        registry.check(!delete_empty_category(bgm, "城镇", failure) && failure.code == kInvalid,
                       "非空分类拒绝删除（不递归）");
        registry.check(std::filesystem::is_directory(bgm / L"城镇", ec), "被拒之后非空分类仍在");

        // ---- delete_track ----
        const auto doomed = bgm / L"城镇" / L"删除我.wav";
        registry.check(musicboard::test::write_text(doomed, "e"), "准备待删除文件");
        failure.clear();
        registry.check(delete_track(doomed, DeleteMode::permanent, result, failure),
                       "永久删除成功");
        registry.check(!std::filesystem::exists(doomed, ec), "永久删除后文件不在了");
        registry.check(result.delete_mode == DeleteMode::permanent, "结果里记录了删除方式");

        // 回收站：文件离开原位置即可（不去查回收站内部，那依赖系统实现）
        const auto recycled = bgm / L"城镇" / L"进回收站.wav";
        registry.check(musicboard::test::write_text(recycled, "f"), "准备进回收站的文件");
        failure.clear();
        registry.check(delete_track(recycled, DeleteMode::recycle_bin, result, failure),
                       "移入回收站成功");
        registry.check(!std::filesystem::exists(recycled, ec), "回收站删除后原位置不在了");
        registry.check(result.delete_mode == DeleteMode::recycle_bin, "默认删除方式是回收站");

        failure.clear();
        registry.check(!delete_track(recycled, DeleteMode::recycle_bin, result, failure) &&
                           failure.code == fail_code::file_not_found,
                       "删除已不存在的文件被拒（并说明原因）");
    }

    return registry.finish("library_ops_tests");
}
