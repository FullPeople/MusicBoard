#pragma once

#include <filesystem>

#include "core/failure.h"
#include "ui/panel_layout.h"
#include "ui/panel_view.h"

namespace musicboard::app {

// 扫真实曲库，填出一个能直接画出来的面板状态。
// 只给预览用（spike 的 --panel 模式），不参与正式程序。
[[nodiscard]] bool build_preview_state(const std::filesystem::path& library_root,
                                       ui::PanelState& state, Failure& failure);

}  // namespace musicboard::app
