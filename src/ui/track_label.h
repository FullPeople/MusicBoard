#pragma once
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace musicboard::ui {
struct TrackLabel final { std::string title; std::vector<std::string> tags; };
[[nodiscard]] inline std::string_view trim_label(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}
[[nodiscard]] inline TrackLabel parse_track_label(std::string_view raw) {
    TrackLabel label{std::string(raw), {}};
    auto rest = trim_label(raw);
    constexpr std::array<std::pair<std::string_view, std::string_view>, 5> pairs{{
        {"(", ")"}, {"（", "）"}, {"[", "]"}, {"{", "}"}, {"【", "】"}}};
    for (;;) {
        bool found = false;
        for (const auto& [open, close] : pairs) {
            if (!rest.starts_with(open)) continue;
            const auto end = rest.find(close, open.size());
            if (end == std::string_view::npos) break;
            const auto tag = trim_label(rest.substr(open.size(), end - open.size()));
            if (!tag.empty() && std::find(label.tags.begin(), label.tags.end(), tag) == label.tags.end())
                label.tags.emplace_back(tag);
            rest = trim_label(rest.substr(end + close.size()));
            found = true; break;
        }
        if (!found) break;
    }
    if (rest.empty()) label.tags.clear(); // A bracket-only title must remain visible.
    else label.title = rest;
    return label;
}
[[nodiscard]] inline std::string tag_line(const std::vector<std::string>& tags) {
    std::string text;
    for (const auto& tag : tags) { if (!text.empty()) text += " · "; text += tag; }
    return text;
}
} // namespace musicboard::ui
