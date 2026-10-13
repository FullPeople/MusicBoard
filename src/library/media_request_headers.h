#pragma once
#include <string_view>

namespace musicboard {
// Use the same request identity while resolving, downloading and streaming.
inline constexpr std::string_view kBrowserUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/120.0.0.0 Safari/537.36";
}
