#include "library/import_transaction.h"

#include <system_error>
#include "core/localization.h"
#include "library/url_classify.h"

#include "audio/decoder.h"
#include "library/library_ops.h"
#include "core/path_utf8.h"

namespace musicboard {
namespace {

[[nodiscard]] bool path_is_symlink(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec));
}

// 校验来源：存在、是普通文件、不是符号链接、确实在暂存区里
[[nodiscard]] bool validate_source(const ImportRequest& request, Failure& failure) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(request.staged_file, ec)) {
        failure.set(fail_code::file_not_found,
                    "暂存文件不存在: " + path_to_utf8(request.staged_file));
        return false;
    }
    // 符号链接可以把"暂存区里的一个链接"指向任意位置，
    // 于是"在暂存区之内"这个检查就形同虚设。
    if (path_is_symlink(request.staged_file)) {
        failure.set(fail_code::invalid_argument,
                    "拒绝导入符号链接: " + path_to_utf8(request.staged_file));
        return false;
    }
    if (request.staging_root.empty()) {
        failure.set(fail_code::invalid_argument, "未指定暂存区根目录，无法确认来源");
        return false;
    }
    if (!path_is_inside(request.staging_root, request.staged_file)) {
        failure.set(fail_code::invalid_argument,
                    "暂存文件不在暂存区之内（路径逃逸）: " + path_to_utf8(request.staged_file) +
                        " 不属于 " + path_to_utf8(request.staging_root));
        return false;
    }
    return true;
}

// 校验内容：它真的是一段能解的音频
[[nodiscard]] bool validate_content(const std::filesystem::path& path, double& duration_seconds,
                                    Failure& failure) {
    AudioInfo info;
    Failure probe_failure;
    if (!probe_audio_file(path, info, probe_failure)) {
        // 这一条同时挡住"改名成 .mp3 的可执行文件"——
        // 不需要专门去认 PE 头，解不出来的就不是音频。
        failure.set(fail_code::unsupported_format,
                    "导入的文件不是可解码的音频: " + path_to_utf8(path) + "（" +
                        probe_failure.to_string() + "）");
        return false;
    }
    duration_seconds = info.duration_seconds;
    return true;
}

}  // namespace

bool import_staged_file(const ImportRequest& request, ImportResult& result, Failure& failure) {
    result = ImportResult{};

    // ---- 1. 校验来源 ----
    if (!validate_source(request, failure)) {
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(request.target_category_dir, ec)) {
        failure.set(fail_code::file_not_found,
                    "目标分类目录不存在: " + path_to_utf8(request.target_category_dir));
        return false;
    }
    if (path_is_symlink(request.target_category_dir)) {
        failure.set(fail_code::invalid_argument,
                    "拒绝导入到符号链接目录: " + path_to_utf8(request.target_category_dir));
        return false;
    }

    // ---- 2. 校验内容 ----
    double staged_duration = 0.0;
    if (!validate_content(request.staged_file, staged_duration, failure)) {
        return false;
    }

    // ---- 3. 定名与定址 ----
    std::string stem = request.preferred_stem.empty() ? path_to_utf8(request.staged_file.stem())
                                                      : request.preferred_stem;
    const bool incomplete = downloaded_audio_is_incomplete(request.source_duration_seconds, staged_duration);
    if (incomplete) stem += tr(" (试听片段)", " (preview)");
    stem = sanitize_to_valid_name(stem);
    // 净化之后还要过一遍严格校验：净化只保证"能用"，不保证"符合我们的规则"
    if (!validate_file_stem(stem, failure)) {
        return false;
    }

    const std::string extension = path_to_utf8(request.staged_file.extension());
    const std::string_view ext_without_dot =
        extension.size() > 1 ? std::string_view(extension).substr(1) : std::string_view{};
    if (ext_without_dot.empty()) {
        failure.set(fail_code::invalid_argument,
                    "暂存文件没有扩展名，无法确定目标文件名: " + path_to_utf8(request.staged_file));
        return false;
    }

    const std::filesystem::path target =
        unique_path_in(request.target_category_dir, stem, ext_without_dot);
    if (target.empty()) {
        failure.set(fail_code::file_open_failed,
                    "目标目录里找不到可用的文件名（同名候选已达上限）");
        return false;
    }
    if (!path_is_inside(request.target_category_dir, target)) {
        failure.set(fail_code::invalid_argument, "目标路径越出了分类目录（路径逃逸）");
        return false;
    }
    // "本来想要的名字"必须和 unique_path_in 的第一个候选用同一个函数算出来，
    // 否则"有没有因为重名而改名"这个比较会恒为真（少拼一个扩展名就够了）。
    const std::filesystem::path wanted =
        track_path_for(request.target_category_dir, stem, ext_without_dot);
    result.renamed_to_avoid_collision = wanted.empty() || target != wanted;

    // ---- 4. 原子发布 ----
    std::filesystem::rename(request.staged_file, target, ec);
    if (ec) {
        // 跨盘（暂存区与曲库不在同一个卷）时 rename 会失败，退回复制 + 校验 + 删源
        ec.clear();
        std::filesystem::copy_file(request.staged_file, target, std::filesystem::copy_options::none, ec);
        if (ec) {
            failure.set(fail_code::file_open_failed,
                        "发布失败（同盘重命名与跨盘复制都失败）→ " + path_to_utf8(target) + ": " +
                            ec.message());
            return false;
        }

        const auto source_size = std::filesystem::file_size(request.staged_file, ec);
        const auto published_size = ec ? 0 : std::filesystem::file_size(target, ec);
        if (ec || source_size != published_size) {
            std::error_code cleanup_ec;
            std::filesystem::remove(target, cleanup_ec);
            failure.set(fail_code::file_read_failed,
                        "发布失败：复制后大小不一致（源 " + std::to_string(source_size) + " 目标 " +
                            std::to_string(published_size) + "），已删除半成品");
            return false;
        }
        std::filesystem::remove(request.staged_file, ec);
    }

    // ---- 5. 发布后再校验一次 ----
    // 走到这里如果还解不出来，说明发布过程中出了问题。把刚发布的文件删掉，
    // 而不是把一个坏文件留在用户曲库里。
    double published_duration = 0.0;
    Failure verify_failure;
    if (!validate_content(target, published_duration, verify_failure)) {
        std::error_code cleanup_ec;
        std::filesystem::remove(target, cleanup_ec);
        failure.set(fail_code::decode_failed,
                    "发布后的文件校验失败，已回滚（删除）：" + path_to_utf8(target) + " —— " +
                        verify_failure.to_string());
        return false;
    }

    result.installed_path = target;
    result.final_stem = path_to_utf8(target.stem());
    result.duration_seconds = published_duration;
    result.content_incomplete = incomplete;
    return true;
}

}  // namespace musicboard
