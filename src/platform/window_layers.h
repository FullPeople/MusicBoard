#pragma once

#include <windows.h>
#include "core/failure.h"

namespace musicboard {
[[nodiscard]] bool restore_pinned_window_layers(HWND orb, HWND panel, bool panel_pinned, Failure& failure);
}
