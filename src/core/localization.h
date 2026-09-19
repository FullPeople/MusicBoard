#pragma once

#include <atomic>

namespace musicboard {
enum class Language { chinese, english };
inline std::atomic<Language> selected_language{Language::chinese};
inline void set_language(Language language) noexcept { selected_language.store(language); }
[[nodiscard]] inline Language language() noexcept { return selected_language.load(); }
// Pairs are explicit at authored UI sites. User filenames and tags never pass through translation.
[[nodiscard]] inline const char* tr(const char* cn, const char* en) noexcept {
    return language() == Language::english ? en : cn;
}
[[nodiscard]] inline const wchar_t* tr(const wchar_t* cn, const wchar_t* en) noexcept {
    return language() == Language::english ? en : cn;
}
}  // namespace musicboard
