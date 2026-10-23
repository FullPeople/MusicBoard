// 安装事务测试
//
// 这一层是**完整性边界**：包被动过一个字节就不该装进去。所以测试的重点不是
// "能装"，而是"该拒的都拒了"：
//   · 内容与清单不符 → 拒
//   · 清单之外多了一个文件 → 拒（少了这一条，往包里塞个 dll 就能过）
//   · 清单里出现绝对路径或 .. → 拒（路径逃逸）
//   · 包内出现重解析点 → 拒
//   · VERSION.txt 缺失或架构不对 → 拒
//
// 装与卸在一个临时目录里做，**不碰真实安装位置**。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "install/install_transaction.h"
#include "install/transaction_lock.h"
#include "core/path_utf8.h"
#include "core/sha256.h"
#include "tests/test_files.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;
using musicboard::test::TempDir;

namespace {

// 造一个合法的安装包：VERSION.txt + 三个文件 + 清单
struct PackageFixture final {
    std::filesystem::path root;

    [[nodiscard]] bool build(const std::filesystem::path& where, const std::string& version,
                             Registry& registry) {
        root = where;
        std::error_code ec;
        std::filesystem::create_directories(root / L"LICENSES", ec);

        bool ok = true;
        ok &= musicboard::test::write_text(root / L"MusicBoard.exe", "假装是主程序");
        ok &= musicboard::test::write_text(root / L"MusicBoardSetup.exe", "假装是安装器");
        ok &= musicboard::test::write_text(root / L"README.zh-CN.txt", "音乐板说明");
        ok &= musicboard::test::write_text(root / L"LICENSES" / L"stb_vorbis.txt", "公共领域");
        if (!ok) {
            registry.check(false, "造安装包：写文件失败");
            return false;
        }

        std::ostringstream version_text;
        version_text << "product=MusicBoard\n"
                     << "version=" << version << "\n"
                     << "package_version=" << version << "\n"
                     << "architecture=windows-x64\n"
                     << "executable=MusicBoard.exe\n"
                     << "complete=1\n";
        ok &= musicboard::test::write_text(root / L"VERSION.txt", version_text.str());
        if (!ok) {
            registry.check(false, "造安装包：写 VERSION.txt 失败");
            return false;
        }
        return write_manifest(registry);
    }

    // 按当前目录内容重新生成清单
    [[nodiscard]] bool write_manifest(Registry& registry) {
        std::vector<std::string> relative_paths;
        std::error_code ec;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(root, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            std::string name = path_to_utf8(std::filesystem::relative(entry.path(), root, ec));
            std::replace(name.begin(), name.end(), '\\', '/');
            // 清单不能列出自己（列了就对不上）—— 与实现里那条豁免是同一条规则
            if (name == "SHA256SUMS.txt") {
                continue;
            }
            relative_paths.push_back(name);
        }
        std::sort(relative_paths.begin(), relative_paths.end());

        std::string manifest;
        for (const std::string& name : relative_paths) {
            std::filesystem::path wide;
            Failure convert;
            if (!path_from_utf8(name, wide, convert)) {
                return false;
            }
            std::string digest;
            Failure hash_failure;
            if (!sha256_file_hex(root / wide, digest, hash_failure)) {
                registry.check(false, "算清单摘要失败: " + hash_failure.to_string());
                return false;
            }
            manifest += digest + "  " + name + "\n";
        }
        return musicboard::test::write_text(root / L"SHA256SUMS.txt", manifest);
    }
};

[[nodiscard]] bool make_symlink_or_skip(const std::filesystem::path& link,
                                       const std::filesystem::path& target, Registry& registry,
                                       const char* label) {
    std::error_code ec;
    std::filesystem::create_symlink(target, link, ec);
    if (ec) {
        static_cast<void>(::CreateSymbolicLinkW(link.c_str(), target.c_str(), 0x2));
    }
    if (!std::filesystem::exists(link, ec)) {
        registry.skip(label);
        return false;
    }
    return true;
}

}  // namespace

int main() {
    Registry registry;

    // =======================================================================
    // 合法包：校验通过
    // =======================================================================
    {
        TempDir temp;
        PackageFixture fixture;
        registry.check(fixture.build(temp.path() / L"pkg", "0.1.0", registry), "造一个合法安装包");

        VerifiedPackage package;
        Failure failure;
        const bool verified = verify_install_package({temp.path() / L"pkg"}, package, failure);
        registry.check_eq(verified, "合法安装包校验通过", failure.to_string());
        registry.check(package.version == "0.1.0", "读出了版本号");
        registry.check(package.architecture == "windows-x64", "读出了架构");
        registry.check(package.executable_name == "MusicBoard.exe", "读出了主程序名");
        registry.check(package.entries.size() == 5, "清单里有 5 条（4 个文件 + 自己）");
        registry.check(is_hex_digest(package.manifest_sha256), "记下了清单自身的摘要");
        registry.check(package.total_bytes > 0, "统计了总字节数");
    }

    // =======================================================================
    // 该拒的都要拒
    // =======================================================================
    {
        // 内容被改过一个字节
        TempDir temp;
        PackageFixture fixture;
        static_cast<void>(fixture.build(temp.path() / L"pkg", "0.1.0", registry));
        registry.check(musicboard::test::write_text(fixture.root / L"MusicBoard.exe",
                                                    "假装是主程序，但被改过"),
                       "篡改主程序内容（不更新清单）");

        VerifiedPackage package;
        Failure failure;
        const bool verified = verify_install_package({fixture.root}, package, failure);
        registry.check(!verified, "内容与清单不符 → 拒绝安装");
        registry.check(failure.code == fail_code::file_read_failed, "错误码是 file_read_failed");
        registry.check(failure.context.find("MusicBoard.exe") != std::string::npos,
                       "原因里点名了是哪个文件");
        registry.check(failure.context.find("清单") != std::string::npos,
                       "原因里说清了是与清单不符");
    }
    {
        // 包里多了一个**没写进清单**的文件
        TempDir temp;
        PackageFixture fixture;
        static_cast<void>(fixture.build(temp.path() / L"pkg", "0.1.0", registry));
        registry.check(musicboard::test::write_text(fixture.root / L"偷偷加进来的.dll",
                                                    "恶意 payload"),
                       "往包里塞一个没写进清单的文件");

        VerifiedPackage package;
        Failure failure;
        const bool verified = verify_install_package({fixture.root}, package, failure);
        registry.check(!verified, "清单之外的文件 → 拒绝（否则可以往包里塞 dll）");
        registry.check(failure.context.find("偷偷加进来的.dll") != std::string::npos,
                       "原因里点名了那个文件");
    }
    {
        // 清单里少了某个文件（文件被删了）
        TempDir temp;
        PackageFixture fixture;
        static_cast<void>(fixture.build(temp.path() / L"pkg", "0.1.0", registry));
        std::error_code ec;
        std::filesystem::remove(fixture.root / L"README.zh-CN.txt", ec);
        registry.check(!ec, "删掉一个文件（不更新清单）");

        VerifiedPackage package;
        Failure failure;
        registry.check(!verify_install_package({fixture.root}, package, failure),
                       "清单里的文件不存在 → 拒绝");
        registry.check(failure.code == fail_code::file_not_found, "错误码是 file_not_found");
    }
    {
        // 路径逃逸：清单里写 ../ 出去
        TempDir temp;
        PackageFixture fixture;
        static_cast<void>(fixture.build(temp.path() / L"pkg", "0.1.0", registry));
        std::string manifest;
        {
            std::ifstream stream(fixture.root / L"SHA256SUMS.txt", std::ios::binary);
            manifest.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        }
        manifest += std::string(64, 'a') + "  ../../../Windows/System32/evil.dll\n";
        registry.check(musicboard::test::write_text(fixture.root / L"SHA256SUMS.txt", manifest),
                       "清单里加一条 ../ 逃逸的路径");

        VerifiedPackage package;
        Failure failure;
        registry.check(!verify_install_package({fixture.root}, package, failure),
                       "含 .. 的路径 → 拒绝（路径逃逸）");
        registry.check(failure.context.find("逃逸") != std::string::npos,
                       "原因说清了是路径逃逸");
    }
    {
        // 绝对路径
        TempDir temp;
        PackageFixture fixture;
        static_cast<void>(fixture.build(temp.path() / L"pkg", "0.1.0", registry));
        std::string manifest;
        {
            std::ifstream stream(fixture.root / L"SHA256SUMS.txt", std::ios::binary);
            manifest.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        }
        manifest += std::string(64, 'b') + "  C:/Windows/System32/evil.dll\n";
        registry.check(musicboard::test::write_text(fixture.root / L"SHA256SUMS.txt", manifest),
                       "清单里加一条绝对路径");

        VerifiedPackage package;
        Failure failure;
        registry.check(!verify_install_package({fixture.root}, package, failure),
                       "绝对路径 → 拒绝");
    }
    {
        // 清单格式错
        TempDir temp;
        PackageFixture fixture;
        static_cast<void>(fixture.build(temp.path() / L"pkg", "0.1.0", registry));
        registry.check(musicboard::test::write_text(fixture.root / L"SHA256SUMS.txt",
                                                    "这不是一行合法的清单\n"),
                       "写一份格式错的清单");

        VerifiedPackage package;
        Failure failure;
        registry.check(!verify_install_package({fixture.root}, package, failure),
                       "清单格式错 → 拒绝");
        registry.check(failure.context.find("第 1 行") != std::string::npos,
                       "原因里带了行号");
    }
    {
        // 缺 VERSION.txt
        TempDir temp;
        PackageFixture fixture;
        static_cast<void>(fixture.build(temp.path() / L"pkg", "0.1.0", registry));
        std::error_code ec;
        std::filesystem::remove(fixture.root / L"VERSION.txt", ec);
        registry.check(!ec, "删掉 VERSION.txt");

        VerifiedPackage package;
        Failure failure;
        registry.check(!verify_install_package({fixture.root}, package, failure),
                       "缺 VERSION.txt → 拒绝");
    }
    {
        // 架构不对
        TempDir temp;
        PackageFixture fixture;
        static_cast<void>(fixture.build(temp.path() / L"pkg", "0.1.0", registry));
        registry.check(musicboard::test::write_text(
                           fixture.root / L"VERSION.txt",
                           "product=MusicBoard\nversion=0.1.0\npackage_version=0.1.0\n"
                           "architecture=windows-arm64\nexecutable=MusicBoard.exe\ncomplete=1\n"),
                       "把架构改成 arm64");

        VerifiedPackage package;
        Failure failure;
        registry.check(!verify_install_package({fixture.root}, package, failure),
                       "架构不对 → 拒绝");
        registry.check(failure.context.find("windows-arm64") != std::string::npos,
                       "原因里说明了实际架构");
    }
    {
        // 产品名不对（拿错了包）
        TempDir temp;
        PackageFixture fixture;
        static_cast<void>(fixture.build(temp.path() / L"pkg", "0.1.0", registry));
        registry.check(musicboard::test::write_text(
                           fixture.root / L"VERSION.txt",
                           "product=别的产品\nversion=0.1.0\npackage_version=0.1.0\n"
                           "architecture=windows-x64\nexecutable=MusicBoard.exe\ncomplete=1\n"),
                       "把产品名改成别的");

        VerifiedPackage package;
        Failure failure;
        registry.check(!verify_install_package({fixture.root}, package, failure),
                       "产品名不对 → 拒绝（拿错了包）");
    }
    {
        // 包目录不存在
        TempDir temp;
        VerifiedPackage package;
        Failure failure;
        registry.check(!verify_install_package({temp.path() / L"没有这个包"}, package, failure),
                       "包目录不存在 → 拒绝");
    }
    {
        // 包内出现重解析点
        TempDir temp;
        PackageFixture fixture;
        static_cast<void>(fixture.build(temp.path() / L"pkg", "0.1.0", registry));
        const std::filesystem::path outside = temp.path() / L"包外的文件.txt";
        static_cast<void>(musicboard::test::write_text(outside, "外部内容"));
        if (make_symlink_or_skip(fixture.root / L"链接.txt", outside, registry,
                                 "包内重解析点 → 拒绝（本机没有创建符号链接的权限）")) {
            std::error_code ec;
            std::filesystem::create_directories(fixture.root, ec);
            VerifiedPackage package;
            Failure failure;
            registry.check(!verify_install_package({fixture.root}, package, failure),
                           "包内有符号链接 → 拒绝（校验过的和装下去的可能是两个东西）");
        }
    }

    // =======================================================================
    // 安装
    // =======================================================================
    {
        TempDir temp;
        PackageFixture fixture;
        static_cast<void>(fixture.build(temp.path() / L"pkg", "0.1.0", registry));

        VerifiedPackage package;
        Failure failure;
        registry.check(verify_install_package({fixture.root}, package, failure), "校验通过");

        const std::filesystem::path install_root = temp.path() / L"Programs" / L"MusicBoard";
        InstallOptions options;
        options.package = package;
        options.install_root = install_root;

        InstallResult result;
        registry.check_invocation([&] { return install_product(options, result, failure); },
                                  "全新安装成功", failure);
        registry.check(!result.upgraded, "这是全新安装（不是升级）");
        registry.check(result.installed_version == "0.1.0", "结果里记了版本");
        registry.check(std::filesystem::is_regular_file(
                           install_root / L"current" / L"MusicBoard.exe"),
                       "主程序在 current/ 下");
        registry.check(std::filesystem::is_regular_file(
                           install_root / L"current" / L"LICENSES" / L"stb_vorbis.txt"),
                       "子目录也一起装过去了");
        registry.check(!std::filesystem::exists(install_root / L"previous"),
                       "全新安装没有 previous 目录");
        registry.check(installed_version(install_root) == "0.1.0",
                       "从安装日志读回版本");
        registry.check(result.installed_executable == install_root / L"current" / L"MusicBoard.exe",
                       "结果里的可执行文件路径正确");

        // 幂等：再装一次同一个版本，不该报错
        InstallResult again;
        registry.check(install_product(options, again, failure), "重复安装同一个版本成功（幂等）");
        registry.check(installed_version(install_root) == "0.1.0", "版本仍是 0.1.0");
    }

    // =======================================================================
    // 升级
    // =======================================================================
    {
        TempDir temp;
        const std::filesystem::path install_root = temp.path() / L"Programs" / L"MusicBoard";

        // 先装 0.1.0
        {
            PackageFixture fixture;
            static_cast<void>(fixture.build(temp.path() / L"pkg1", "0.1.0", registry));
            VerifiedPackage package;
            Failure failure;
            registry.check(verify_install_package({fixture.root}, package, failure), "校验 0.1.0");
            InstallOptions options;
            options.package = package;
            options.install_root = install_root;
            InstallResult result;
            registry.check(install_product(options, result, failure), "装 0.1.0");
        }
        // 再装 0.2.0
        {
            PackageFixture fixture;
            static_cast<void>(fixture.build(temp.path() / L"pkg2", "0.2.0", registry));
            registry.check(musicboard::test::write_text(fixture.root / L"新增文件.txt", "0.2.0 新增"),
                           "0.2.0 里多一个文件");
            registry.check(fixture.write_manifest(registry), "重新生成清单");

            VerifiedPackage package;
            Failure failure;
            registry.check(verify_install_package({fixture.root}, package, failure), "校验 0.2.0");
            InstallOptions options;
            options.package = package;
            options.install_root = install_root;
            InstallResult result;
            registry.check_invocation([&] { return install_product(options, result, failure); },
                                      "升级到 0.2.0 成功", failure);
            registry.check(result.upgraded, "结果里标明了这是升级");
            registry.check(result.previous_version == "0.1.0", "记下了被替换的版本");
            registry.check(installed_version(install_root) == "0.2.0", "现在装的是 0.2.0");
            registry.check(std::filesystem::is_regular_file(
                               install_root / L"current" / L"新增文件.txt"),
                           "新版本的文件在 current 下");
            registry.check(!std::filesystem::exists(install_root / L"previous"),
                           "升级成功后 previous 被清掉了");
        }
    }

    // =======================================================================
    // 中断恢复：模拟"旧版本让位之后断电"
    // =======================================================================
    {
        TempDir temp;
        const std::filesystem::path install_root = temp.path() / L"Programs" / L"MusicBoard";

        // 装 0.1.0
        {
            PackageFixture fixture;
            static_cast<void>(fixture.build(temp.path() / L"pkg1", "0.1.0", registry));
            VerifiedPackage package;
            Failure failure;
            registry.check(verify_install_package({fixture.root}, package, failure), "校验 0.1.0");
            InstallOptions options;
            options.package = package;
            options.install_root = install_root;
            InstallResult result;
            registry.check(install_product(options, result, failure), "装 0.1.0");
        }

        // 手工造出"旧版本已经让位、候选还没就位"的现场：
        // current → previous，并写一份 phase=previous_moved 的日志
        std::error_code ec;
        std::filesystem::rename(install_root / L"current", install_root / L"previous", ec);
        registry.check(!ec, "手工制造中断现场：current 已经让位成 previous");
        registry.check(musicboard::test::write_text(
                           install_root / L"install-journal.txt",
                           "phase=3\nversion=0.2.0\ncomplete=1\n"),
                       "写一份 phase=previous_moved 的日志");

        // 再装 0.2.0：应当先把 previous 放回去（恢复），再正常安装
        {
            PackageFixture fixture;
            static_cast<void>(fixture.build(temp.path() / L"pkg2", "0.2.0", registry));
            VerifiedPackage package;
            Failure failure;
            registry.check(verify_install_package({fixture.root}, package, failure), "校验 0.2.0");
            InstallOptions options;
            options.package = package;
            options.install_root = install_root;
            InstallResult result;
            registry.check_invocation([&] { return install_product(options, result, failure); },
                                      "上一次中断之后能接着装", failure);
            registry.check(result.recovered_interrupted_transaction, "结果里标明了做过恢复");
            registry.check(installed_version(install_root) == "0.2.0", "最终装的是 0.2.0");
            registry.check(std::filesystem::is_regular_file(
                               install_root / L"current" / L"MusicBoard.exe"),
                           "current 下有主程序");
            registry.check(!std::filesystem::exists(install_root / L"previous"),
                           "恢复过程没有留下 previous");
        }
    }

    // =======================================================================
    // 卸载
    // =======================================================================
    {
        TempDir temp;
        const std::filesystem::path install_root = temp.path() / L"Programs" / L"MusicBoard";
        PackageFixture fixture;
        static_cast<void>(fixture.build(temp.path() / L"pkg", "0.1.0", registry));
        VerifiedPackage package;
        Failure failure;
        registry.check(verify_install_package({fixture.root}, package, failure), "校验");
        InstallOptions options;
        options.package = package;
        options.install_root = install_root;
        InstallResult installed;
        registry.check(install_product(options, installed, failure), "装好");

        // 在安装根**外面**放一个文件：卸载不许动它
        const std::filesystem::path outside = temp.path() / L"用户的曲库" / L"别删我.mp3";
        std::error_code ec;
        std::filesystem::create_directories(outside.parent_path(), ec);
        registry.check(musicboard::test::write_text(outside, "用户的数据"), "在安装目录外放一份用户数据");

        UninstallOptions uninstall;
        uninstall.install_root = install_root;
        UninstallResult result;
        registry.check_invocation([&] { return uninstall_product(uninstall, result, failure); },
                                  "卸载成功", failure);
        registry.check(!std::filesystem::exists(install_root, ec), "安装根目录被删掉了");
        registry.check(result.removed_version == "0.1.0", "记下了卸载的版本");
        registry.check(std::filesystem::is_regular_file(outside, ec),
                       "**安装目录之外的用户数据没有被碰**");

        // 再卸载一次：已经没了也算成功（幂等）
        UninstallResult again;
        registry.check(uninstall_product(uninstall, again, failure), "重复卸载不报错");
    }

    // =======================================================================
    // 同版本修订、清单信任边界与真实进度
    // =======================================================================
    {
        TempDir temp;
        PackageFixture fixture;
        static_cast<void>(fixture.build(temp.path() / L"pkg", "0.3.5", registry));
        VerifyOptions verify;
        verify.package_root = fixture.root;
        Failure failure;
        registry.check(sha256_file_hex(fixture.root / L"SHA256SUMS.txt", verify.expected_manifest_sha256, failure), "读取可信清单摘要");
        std::vector<std::pair<std::uint64_t, std::uint64_t>> verified;
        verify.progress = [&](std::uint64_t done, std::uint64_t total) { verified.emplace_back(done, total); };
        VerifiedPackage package;
        registry.check(verify_install_package(verify, package, failure), "带预期清单摘要校验通过");
        registry.check(!verified.empty() && verified.front().first == 0 &&
                           verified.back().first == package.entries.size() &&
                           verified.back().second == package.entries.size(), "校验进度对应实际文件总数");
        verify.expected_manifest_sha256.assign(64, '0');
        VerifiedPackage rejected;
        Failure mismatch;
        registry.check(!verify_install_package(verify, rejected, mismatch), "拒绝与可信摘要不一致的清单");
        InstallOptions install;
        install.package = package;
        install.install_root = temp.path() / L"安装位置";
        std::vector<std::pair<std::uint64_t, std::uint64_t>> copied;
        install.progress = [&](std::uint64_t done, std::uint64_t total) { copied.emplace_back(done, total); };
        InstallResult result;
        registry.check(install_product(install, result, failure), "旧修订安装成功");
        const auto expected_bytes = package.total_bytes + std::filesystem::file_size(fixture.root / L"SHA256SUMS.txt");
        registry.check(!copied.empty() && copied.front().first == 0 && copied.back().first == expected_bytes &&
                           copied.back().second == expected_bytes, "复制进度对应真实完成字节数");
        bool monotonic = true;
        for (std::size_t index = 1; index < copied.size(); ++index) monotonic &= copied[index].first >= copied[index - 1].first && copied[index].first <= copied[index].second;
        registry.check(monotonic, "复制进度单调且不超出总量");
        copied.clear();
        registry.check(install_product(install, result, failure) && copied.empty(), "相同版本及清单重复安装保持幂等");
        registry.check(musicboard::test::write_text(fixture.root / L"MusicBoard.exe", "相同版本的新安装修订"), "准备同版本新文件");
        registry.check(fixture.write_manifest(registry), "为新修订生成新清单");
        registry.check(verify_install_package({fixture.root}, install.package, failure), "校验同版本新修订");
        copied.clear();
        registry.check(install_product(install, result, failure) && result.upgraded && !copied.empty(), "同版本不同清单实际覆盖安装");
        std::string actual;
        registry.check(sha256_file_hex(install.install_root / L"current" / L"MusicBoard.exe", actual, failure) &&
                           sha256_file_hex(fixture.root / L"MusicBoard.exe", verify.expected_manifest_sha256, failure) &&
                           actual == verify.expected_manifest_sha256, "覆盖后主程序与新修订逐字节一致");
    }

    {
        TempDir temp;
        const auto unrelated = temp.path() / L"用户曲库";
        std::filesystem::create_directories(unrelated);
        registry.check(musicboard::test::write_text(unrelated / L"保留.mp3", "保留用户文件"), "准备无关目录");
        UninstallResult removed;
        Failure failure;
        registry.check(!uninstall_product({unrelated}, removed, failure) &&
                           std::filesystem::is_regular_file(unrelated / L"保留.mp3"), "拒绝卸载未登记的无关目录");
    }

    // =======================================================================
    // 安装与卸载使用同一个按目录命名的跨进程锁
    // =======================================================================
    {
        TempDir temp;
        const auto root = temp.path() / L"事务目录";
        std::wstring first, second, other;
        Failure failure;
        registry.check(transaction_mutex_name(root, first, failure) &&
                           transaction_mutex_name(root / L".", second, failure) && first == second,
                       "同目录的点路径与末尾分隔符使用同一事务锁");
        auto uppercase = root.native();
        ::CharUpperBuffW(uppercase.data(), static_cast<DWORD>(uppercase.size()));
        registry.check(transaction_mutex_name(uppercase, second, failure) && first == second,
                       "大小写不同的同一安装位置使用同一事务锁");
        registry.check(transaction_mutex_name(temp.path() / L"其他目录", other, failure) && first != other,
                       "不同安装位置不会互相阻塞");
        registry.check(first.starts_with(L"Local\\MusicBoard.Setup.Transaction.") && first.size() == 99,
                       "事务锁使用固定命名空间与 SHA-256 目录摘要");
    }

    // =======================================================================
    // 默认安装位置
    // =======================================================================
    {
        const std::filesystem::path root = default_install_root();
        registry.check(!root.empty(), "默认安装位置取得到（LOCALAPPDATA 在）");
        registry.check(root.filename() == L"MusicBoard", "最后一级是 MusicBoard");
        registry.check(root.parent_path().filename() == L"Programs",
                       "装在 Programs 下（用户目录，不需要管理员）");
    }

    return registry.finish("install_transaction_tests");
}
