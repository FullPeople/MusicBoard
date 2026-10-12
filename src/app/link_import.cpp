#include "app/application.h"
#include "app/app_output.h"
#include "app/native_window_access.h"
#include "core/localization.h"
#include "core/path_utf8.h"
#include "library/import_transaction.h"
#include "library/library_ops.h"
#include "library/staging.h"
#include <algorithm>

namespace musicboard::app {
void Application::show_link_import() {
    link_import_window_.on_start = [this](const LinkImportInput& input, Failure& failure) {
        return start_link_import(input, failure);
    };
    link_import_window_.on_cancel = [this] { if (link_import_job_) link_import_job_->cancel(); };
    Failure failure;
    if (!link_import_window_.show(panel_window_handle(), failure))
        set_status(tr("无法打开链接导入: ", "Could not open link importer: ") + failure.to_string());
}
bool Application::start_link_import(const LinkImportInput& input, Failure& failure) {
    if (loading_ || link_import_active_) {
        failure.set("import.busy", tr("请等待当前任务完成", "Wait for the current task to finish"));
        return false;
    }
    const auto classified = classify_url(input.url);
    if (classified.kind == ImportSourceKind::unsupported) {
        failure.set("import.url", classified.rejection_reason);
        return false;
    }
    std::filesystem::path category;
    if (!path_from_utf8(input.category, category, failure)) return false;
    if (category.empty() || category.has_parent_path() || category == L"." || category == L".." ||
            category.native().find_first_of(L"<>:\"|?*\\/") != std::wstring::npos) {
        failure.set("import.category", tr("请输入有效的单层分类名", "Enter a valid category name"));
        return false;
    }
    const auto target = library_root_ / (input.as_sfx ? L"SFX" : L"BGM") / category;
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        failure.set("import.category", tr("分类目录不能是链接目录", "Category cannot be a linked folder"));
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(target, error);
    if (error) { failure.set("import.category", error.message()); return false; }
    ImportJobRequest request;
    request.url = input.url;
    request.kind = classified.kind;
    request.staging_root = default_staging_root();
    request.target_category_dir = target;
    if (request.kind == ImportSourceKind::yt_dlp && !resolve_yt_dlp(request.yt_dlp_path, failure)) return false;
    link_import_job_ = std::make_unique<ImportJob>();
    if (!link_import_job_->start(request, failure)) return false;
    link_import_input_ = input;
    link_import_active_ = true;
    link_import_status_.clear();
    link_import_window_.set_status(tr("正在下载；完成后可离线播放", "Downloading; available offline when complete"), true);
    return true;
}
void Application::pump_link_import() {
    if (!link_import_active_) return;
    link_import_job_->pump();
    const auto& status = link_import_job_->status();
    const bool running = status.state == ImportJobState::downloading;
    std::string message = status.status_text;
    if (running && !status.has_total && status.received_bytes > 0)
        message += " · " + std::to_string(status.received_bytes / 1024) + " KiB";
    if (status.state == ImportJobState::failed) message = status.failure_reason + " " + status.suggestion;
    if (status.state == ImportJobState::cancelled) message = tr("已取消下载", "Download cancelled");
    if (message != link_import_status_) {
        link_import_status_ = message;
        link_import_window_.set_status(message, running);
    }
    if (running) return;
    link_import_active_ = false;
    if (status.state != ImportJobState::done) { set_status(message); return; }
    const auto& result = link_import_job_->result();
    LibraryEntry entry;
    entry.track.name = result.final_stem;
    entry.track.path = result.installed_path;
    entry.track.duration_seconds = result.duration_seconds;
    entry.label = ui::parse_track_label(entry.track.name);
    entry.category = link_import_input_.category;
    entry.is_sfx = link_import_input_.as_sfx;
    entries_.push_back(std::move(entry));
    const std::size_t index = entries_.size() - 1;
    Column& column = link_import_input_.as_sfx ? sfx_ : bgm_;
    if (std::find(column.categories.begin(), column.categories.end(), link_import_input_.category) == column.categories.end())
        column.categories.push_back(link_import_input_.category);
    rebuild_visible(column, link_import_input_.as_sfx);
    message = tr("已下载，可离线播放：", "Downloaded for offline playback: ") + result.final_stem;
    if (link_import_input_.play_when_ready) {
        const bool started = link_import_input_.as_sfx ? trigger_sfx(0, index) : play_bgm(index);
        if (!started) message += tr("；播放失败，请查看状态和日志", "; playback failed; check status and logs");
    }
    set_status(message);
    link_import_window_.set_status(message, false);
}
}
