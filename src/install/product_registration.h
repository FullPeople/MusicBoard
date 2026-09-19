#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 产品注册：让"安装"这件事在系统里看得见
//
// 两样东西：
//   1. **卸载入口** —— HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\<产品名>
//      有了它，"设置 → 应用"里才看得到音乐板，也才有卸载按钮。
//   2. **开始菜单快捷方式** —— 用户找不到开始菜单里的程序时会以为没装上。
//
// 三条刻意的选择：
//
//   · **只写 HKCU，不写 HKLM。** 装到用户目录、不需要管理员、也就不该要求
//     提权才能卸载。代价是"所有用户"看不到它 —— 对一个跑团用的音乐板，
//     这个取舍是对的，而它换来了"双击即装、不弹 UAC"。
//   · **UninstallString 指向安装目录里的那份 setup。** 那样卸载器与被装的
//     版本天然一致；如果指向包外的东西，用户升级之后就会出现"旧卸载器卸新版本"。
//   · **卸载只删自己写的东西**：一个注册表键 + 一个快捷方式 + 安装目录。
//     用户的曲库、设置、日志都不动。
// ============================================================================

struct ProductRegistrationOptions final {
    // 安装根目录（里面有 current\）
    std::filesystem::path install_root;
    // 用来卸载的可执行文件（一般是安装目录里的 MusicBoardSetup.exe）
    std::filesystem::path uninstaller_path;
    // 主程序（用于 DisplayIcon 与快捷方式）
    std::filesystem::path executable_path;
    // 开始菜单快捷方式的位置（默认 %APPDATA%\Microsoft\Windows\Start Menu\Programs\音乐板.lnk）
    std::filesystem::path start_menu_shortcut;
    // 注册表子键名。默认 "MusicBoard"；测试会传别的名字，免得动到真产品。
    std::wstring uninstall_registry_subkey;
    std::string product_name = "音乐板";
    std::string publisher = "MusicBoard";
    std::string product_version;
    std::uint32_t estimated_size_kib = 0;
    // 安装目录里主程序的相对路径（"current\\MusicBoard.exe"）
    std::wstring executable_relative;
};

// 默认的开始菜单快捷方式路径
[[nodiscard]] std::filesystem::path default_start_menu_shortcut();

// 默认的注册表子键名
[[nodiscard]] std::wstring default_uninstall_registry_subkey();

// 写注册表项 + 建开始菜单快捷方式。任一步失败都会**把已经做的回滚掉**，
// 不留半个注册。
[[nodiscard]] bool apply_product_registration(const ProductRegistrationOptions& options,
                                             Failure& failure);

// 删掉注册表项与快捷方式。已经没了也算成功（幂等）。
[[nodiscard]] bool remove_product_registration(const ProductRegistrationOptions& options,
                                              Failure& failure);

// 读回系统里记的东西，判断与期望是否一致。
// 打包后的冒烟测试用它验证"装完之后系统里确实有这一条"。
[[nodiscard]] bool product_registration_matches(const ProductRegistrationOptions& options,
                                               bool& matches, Failure& failure);

// 只读：当前注册表里记的版本（没注册过返回空）
[[nodiscard]] std::string registered_version(const std::wstring& subkey);

}  // namespace musicboard
