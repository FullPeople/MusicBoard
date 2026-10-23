#include "install/install_transaction.h"
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 1;
    const std::filesystem::path root(argv[2]);
    if (!root.is_absolute() || root == root.root_path()) return 1;
    musicboard::Failure failure;
    musicboard::VerifyOptions verify;
    verify.package_root = argv[1];
    musicboard::VerifiedPackage package;
    if (!musicboard::verify_install_package(verify, package, failure)) {
        std::fprintf(stderr, "%s\n", failure.to_string().c_str()); return 2;
    }
    musicboard::InstallOptions options;
    options.package = package;
    options.install_root = root;
    musicboard::InstallResult result;
    if (!musicboard::install_product(options, result, failure)) {
        std::fprintf(stderr, "%s\n", failure.to_string().c_str()); return 3;
    }
    std::printf("Installed transaction baseline %s\n", result.installed_version.c_str());
    return 0;
}
