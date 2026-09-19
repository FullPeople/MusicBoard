#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 安装事务
//
// 安装包长这样（由 tools/package_release.ps1 产出）：
//
//   <包根>/MusicBoard.exe          主程序
//   <包根>/MusicBoardSetup.exe     安装/卸载程序（它自己也装在包里）
//   <包根>/VERSION.txt             机器可读的版本契约（key=value，与设置文件同一套写法）
//   <包根>/SHA256SUMS.txt          每行 "<64 位十六进制>  <相对路径>"
//   <包根>/README.zh-CN.txt        给人看的说明
//   <包根>/LICENSES/…              第三方许可
//
// 安装分两步，**顺序不能反**：
//
//   1. **校验**（verify_install_package）—— 这是完整性边界，不是可选的礼节：
//      · 清单里每一行的路径必须相对、不许有 `..`、不许是绝对路径（挡住路径逃逸）
//      · 每个文件都要哈希对得上
//      · **包里不许有清单之外的文件**（否则可以往包里塞一个 dll 让它被加载）
//      · 包内不许出现重解析点（符号链接/联结点），否则校验过的和装下去的可能是两个东西
//      · VERSION.txt 必须存在且架构对得上
//   2. **安装**（install_product）—— 带日志的分阶段搬运，中途断电可以回滚或续做。
//
// 为什么不用 MSI：这个产品是"解压即用"的单目录程序，没有注册组件、
// 没有共享 DLL、不需要管理员。用一个自带的安装器反而更可预测，
// 而且安装器本身也在包里，卸载时它和被装的版本天然一致。
// ============================================================================

// 包里的一个文件
struct InstallPackageEntry final {
    std::string relative_path;
    std::string sha256;
};

struct VerifiedPackage final {
    std::filesystem::path root;
    std::string product;
    std::string version;
    std::string package_version;
    std::string architecture;
    std::string executable_name;
    // 清单文件自身的 SHA256（写进安装日志，便于事后核对装的是哪一份包）
    std::string manifest_sha256;
    std::vector<InstallPackageEntry> entries;
    std::uintmax_t total_bytes = 0;
};

struct VerifyOptions final {
    std::filesystem::path package_root;
    std::string expected_architecture = "windows-x64";
};

// 只校验，不安装。失败时 failure.context 会指出**是哪个文件、哪一条规则**。
[[nodiscard]] bool verify_install_package(const VerifyOptions& options, VerifiedPackage& package,
                                        Failure& failure);

// ---------------------------------------------------------------------------
// 安装
// ---------------------------------------------------------------------------

enum class InstallPhase : std::uint32_t {
    // 还没开始（日志里不会出现这个值）
    none = 0,
    // 安装包已经校验过，准备开始
    verified = 1,
    // 候选目录已经写满（这一步之后断电，current 还是旧的，安全）
    staged = 2,
    // 旧版本被让位到 previous（这一步之后断电，需要恢复）
    previous_moved = 3,
    // 候选已经就位成 current（这一步之后断电，只需收尾）
    candidate_published = 4,
    // 收尾完成
    committed = 5,
};

[[nodiscard]] const char* install_phase_name(InstallPhase phase) noexcept;

struct InstallOptions final {
    VerifiedPackage package;
    std::filesystem::path install_root;
};

struct InstallResult final {
    bool upgraded = false;
    // 上次被中断的事务这次被接着做完了（或回滚了）
    bool recovered_interrupted_transaction = false;
    std::string previous_version;
    std::string installed_version;
    std::filesystem::path installed_executable;
};

// 安装或升级。已安装的版本相同则直接成功（幂等）。
[[nodiscard]] bool install_product(const InstallOptions& options, InstallResult& result,
                                 Failure& failure);

// ---------------------------------------------------------------------------
// 卸载
// ---------------------------------------------------------------------------

struct UninstallOptions final {
    std::filesystem::path install_root;
};

struct UninstallResult final {
    bool recovered_interrupted_transaction = false;
    std::uintmax_t removed_bytes = 0;
    std::string removed_version;
};

// 卸载。**只删自己装下去的东西**：安装根目录之外的任何文件都不动。
[[nodiscard]] bool uninstall_product(const UninstallOptions& options, UninstallResult& result,
                                   Failure& failure);

// 读安装日志里记的版本（没装过时返回空）
[[nodiscard]] std::string installed_version(const std::filesystem::path& install_root);

// 默认安装位置：%LOCALAPPDATA%\Programs\MusicBoard
// 放用户目录而不是 Program Files：不需要管理员，也不需要 UAC。
[[nodiscard]] std::filesystem::path default_install_root();

}  // namespace musicboard
