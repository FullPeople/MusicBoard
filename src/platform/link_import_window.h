#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include "core/failure.h"

namespace musicboard {
struct LinkImportInput final {
    std::string url;
    std::string category;
    bool as_sfx = false;
    bool play_when_ready = true;
};
class LinkImportWindow final {
public:
    ~LinkImportWindow();
    [[nodiscard]] bool show(HWND owner, Failure& failure);
    void set_status(const std::string& text, bool running);
    void set_details(const std::string& text);
    std::function<bool(const LinkImportInput&, Failure&)> on_start;
    std::function<void()> on_cancel;
private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    [[nodiscard]] bool create_controls(Failure& failure);
    HWND window_ = nullptr;
    HWND url_ = nullptr;
    HWND category_ = nullptr;
    HWND source_ = nullptr;
    HWND autoplay_ = nullptr;
    HWND status_ = nullptr;
    HWND start_ = nullptr;
    HWND cancel_ = nullptr;
    HWND details_ = nullptr;
    std::string detail_text_;
    HFONT font_ = nullptr;
};
}
