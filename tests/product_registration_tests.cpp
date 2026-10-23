// 产品注册测试
//
// 这一层真的会写系统状态（注册表 + 开始菜单），所以三条纪律：
//   · 测试用**自己的注册表子键**（MusicBoard.SelfTest-<pid>），不碰真产品的那一条
//   · 快捷方式写在临时目录里，不写进真的开始菜单
//   · 每个用例结束都清理干净，包括失败路径
//
// 除了"写进去了"，还要验"读回来对不对"与"删掉之后真的没了" ——
// 卸载残留一个注册表项，用户在"设置 → 应用"里会看到一个卸不掉的空壳。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>   // CoInitializeEx

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "core/path_utf8.h"
#include "install/product_registration.h"
#include "tests/test_files.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;
using musicboard::test::TempDir;

namespace {

// 每个用例一个独立子键，跑完删掉
std::wstring test_subkey() {
    return L"MusicBoard.SelfTest-" + std::to_wstring(::GetCurrentProcessId());
}

ProductRegistrationOptions make_options(const std::filesystem::path& temp,
                                       const std::wstring& subkey) {
    ProductRegistrationOptions options;
    options.install_root = temp / L"Programs" / L"MusicBoard";
    options.uninstaller_path = options.install_root / L"current" / L"MusicBoardSetup.exe";
    options.executable_path = options.install_root / L"current" / L"MusicBoard.exe";
    options.start_menu_shortcut = temp / L"开始菜单" / L"音乐板.lnk";
    options.desktop_shortcut = temp / L"桌面" / L"音乐板.lnk";
    options.uninstall_registry_subkey = subkey;
    options.product_name = "音乐板";
    options.publisher = "MusicBoard";
    options.product_version = "0.1.0";
    options.estimated_size_kib = 4096;
    return options;
}

}  // namespace

int main() {
    Registry registry;

    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    registry.check(SUCCEEDED(com) || com == RPC_E_CHANGED_MODE, "COM 可用");

    const std::wstring subkey = test_subkey();

    // =======================================================================
    // 注册 → 匹配 → 卸载
    // =======================================================================
    {
        TempDir temp;
        const ProductRegistrationOptions options = make_options(temp.path(), subkey);

        // 先确保干净（上一次跑挂了可能留下东西）
        {
            Failure cleanup;
            static_cast<void>(remove_product_registration(options, cleanup));
        }

        Failure failure;
        registry.check_invocation([&] { return apply_product_registration(options, failure); },
                                  "写注册表 + 建开始菜单快捷方式", failure);

        // 快捷方式真的建出来了
        std::error_code ec;
        registry.check(std::filesystem::is_regular_file(options.start_menu_shortcut, ec),
                       "开始菜单快捷方式文件存在");
        registry.check(std::filesystem::file_size(options.start_menu_shortcut, ec) > 0,
                       "快捷方式不是空文件");

        // 读回来对得上
        bool matches = false;
        Failure match_failure;
        registry.check_invocation(
            [&] { return product_registration_matches(options, matches, match_failure); },
            "能读回注册信息", match_failure);
        registry.check(matches, "读回来的信息与期望一致");

        // 版本能从注册表读出来
        registry.check(registered_version(subkey) == "0.1.0", "从注册表读出版本号");

        // **独立构造**期望值：直接用一个宽字符串字面量去读注册表比对。
        // 如果拿实现自己的 UTF-8 转换结果当期望，两边会一样地错而断言照样通过。
        // 这条曾经真的抓到过：DisplayName 写进去的是 9 个 Latin-1 字符而不是"音乐板"。
        {
            HKEY key = nullptr;
            const std::wstring key_path =
                std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") +
                subkey;
            const LSTATUS opened =
                ::RegOpenKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, KEY_READ, &key);
            registry.check(opened == ERROR_SUCCESS, "能直接打开注册表键（绕过本模块的封装）");
            if (opened == ERROR_SUCCESS) {
                wchar_t buffer[512]{};
                DWORD size = sizeof(buffer);
                DWORD type = 0;
                const LSTATUS queried = ::RegQueryValueExW(
                    key, L"DisplayName", nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size);
                registry.check(queried == ERROR_SUCCESS, "读得到 DisplayName");
                registry.check_eq(std::wstring(buffer) == L"音乐板",
                                  "DisplayName 就是「音乐板」这三个字（不是逐字节加宽的乱码）",
                                  path_to_utf8(std::filesystem::path(buffer)));
                wchar_t uninstall[4096]{};
                size = sizeof(uninstall);
                registry.check(::RegQueryValueExW(key, L"UninstallString", nullptr, &type,
                               reinterpret_cast<BYTE*>(uninstall), &size) == ERROR_SUCCESS,
                               "读取完整卸载命令");
                const std::wstring command = uninstall;
                registry.check(command.find(L"--install-root \"" + options.install_root.wstring() + L"\"") != std::wstring::npos &&
                                   command.find(L"--registry-subkey \"" + subkey + L"\"") != std::wstring::npos &&
                                   command.find(L"--shortcut-path \"" + options.start_menu_shortcut.wstring() + L"\"") != std::wstring::npos &&
                                   command.find(L"--desktop-shortcut-path \"" + options.desktop_shortcut.wstring() + L"\"") != std::wstring::npos,
                               "卸载命令保留自选安装位置与全部隔离参数");
                ::RegCloseKey(key);
            }
        }

        // 版本对不上时 matches 必须是 false（这是冒烟测试要用的判断）
        {
            ProductRegistrationOptions other = options;
            other.product_version = "9.9.9";
            bool other_matches = true;
            Failure other_failure;
            static_cast<void>(
                product_registration_matches(other, other_matches, other_failure));
            registry.check(!other_matches, "版本对不上时 matches 为 false");
        }

        // ---- 卸载 ----
        Failure remove_failure;
        registry.check_invocation(
            [&] { return remove_product_registration(options, remove_failure); },
            "删除注册与快捷方式", remove_failure);
        registry.check(!std::filesystem::exists(options.start_menu_shortcut, ec),
                       "快捷方式被删掉了");
        registry.check(registered_version(subkey).empty(), "注册表里读不到版本了");

        bool after = true;
        Failure after_failure;
        static_cast<void>(product_registration_matches(options, after, after_failure));
        registry.check(!after, "卸载之后 matches 为 false");

        // 幂等
        Failure again_failure;
        registry.check(remove_product_registration(options, again_failure),
                       "重复卸载不报错（幂等）");
    }

    // =======================================================================
    // 参数校验
    // =======================================================================
    {
        TempDir temp;
        ProductRegistrationOptions options = make_options(temp.path(), subkey);
        options.install_root.clear();
        Failure failure;
        registry.check(!apply_product_registration(options, failure) &&
                           failure.code == fail_code::invalid_argument,
                       "没有安装目录 → 明确拒绝");
    }

    {
        TempDir temp;
        auto options = make_options(temp.path(), subkey);
        Failure failure;
        registry.check(apply_product_registration(options, failure), "启用私有桌面快捷方式");
        registry.check(std::filesystem::is_regular_file(options.desktop_shortcut) &&
                           registered_desktop_shortcut_enabled(subkey, false), "桌面选项及链接均已保存");
        options.desktop_shortcut_enabled = false;
        registry.check(apply_product_registration(options, failure) && !std::filesystem::exists(options.desktop_shortcut) &&
                           !registered_desktop_shortcut_enabled(subkey, true), "关闭选项移除自己的桌面链接并持久保留选择");
        auto broken = options;
        broken.desktop_shortcut_enabled = true;
        broken.product_version = "9.9.9";
        const auto blocked = temp.path() / L"不允许建目录";
        registry.check(musicboard::test::write_text(blocked, "这里已经是一个文件"), "准备可重复的快捷方式写入失败");
        broken.desktop_shortcut = blocked / L"音乐板.lnk";
        Failure write_failure;
        registry.check(!apply_product_registration(broken, write_failure) && registered_version(subkey) == "0.1.0" &&
                           !registered_desktop_shortcut_enabled(subkey, true), "注册失败恢复原版本和桌面选择，不清掉旧安装登记");
        registry.check(musicboard::test::write_text(options.desktop_shortcut, "用户自己的同名文件"), "准备不属于音乐板的同名桌面文件");
        registry.check(apply_product_registration(options, failure) && std::filesystem::is_regular_file(options.desktop_shortcut), "选项关闭时保留外来同名文件");
        options.desktop_shortcut_enabled = true;
        Failure collision;
        registry.check(!apply_product_registration(options, collision) &&
                           std::filesystem::file_size(options.desktop_shortcut) == std::string("用户自己的同名文件").size(), "拒绝覆盖外来桌面文件");
        registry.check(remove_product_registration(options, failure) && std::filesystem::is_regular_file(options.desktop_shortcut), "卸载保留外来桌面文件");
    }

    // =======================================================================
    // 没注册过时：matches 为 false，但不算失败
    // =======================================================================
    {
        TempDir temp;
        const ProductRegistrationOptions options = make_options(temp.path(), subkey);
        bool matches = true;
        Failure failure;
        registry.check_invocation(
            [&] { return product_registration_matches(options, matches, failure); },
            "没注册过也能查询（不报错）", failure);
        registry.check(failure.empty(), "没注册过不算错误");
        registry.check(!matches, "没注册过时 matches 为 false");
        registry.check(registered_version(subkey).empty(), "没注册过时读不到版本");
    }

    // =======================================================================
    // 默认值
    // =======================================================================
    {
        const std::filesystem::path shortcut = default_start_menu_shortcut();
        registry.check(!shortcut.empty(), "取得到默认开始菜单路径");
        registry.check(shortcut.extension() == L".lnk", "默认路径是 .lnk");
        registry.check(shortcut.filename() == L"音乐板.lnk", "默认名字是中文的「音乐板.lnk」");
        registry.check(shortcut.parent_path().filename() == L"Programs",
                       "默认落在 ...\\Start Menu\\Programs 下");
        registry.check(default_uninstall_registry_subkey() == L"MusicBoard",
                       "默认注册表子键是 MusicBoard");
    }

    // 收尾：确保测试子键不在系统里留下东西
    {
        TempDir temp;
        ProductRegistrationOptions options = make_options(temp.path(), subkey);
        Failure cleanup;
        static_cast<void>(remove_product_registration(options, cleanup));
    }

    if (SUCCEEDED(com)) {
        ::CoUninitialize();
    }
    return registry.finish("product_registration_tests");
}
