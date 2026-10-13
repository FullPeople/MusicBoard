#include "platform/link_import_window.h"
#include "core/localization.h"
#include "core/path_utf8.h"
#include <vector>

namespace musicboard {
namespace {
constexpr wchar_t kind[] = L"MusicBoard.LinkImport";
std::string text_of(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1);
    GetWindowTextW(edit, buffer.data(), length + 1);
    return wide_to_utf8(std::wstring_view(buffer.data(), static_cast<std::size_t>(length)));
}
}
LinkImportWindow::~LinkImportWindow() {
    if (window_) { RemovePropW(window_, L"MusicBoard.ModelessDialog"); DestroyWindow(window_); }
    if (font_) DeleteObject(font_);
}
bool LinkImportWindow::show(HWND owner, Failure& failure) {
    if (!window_) {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpfnWndProc = window_proc;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(cls.hInstance, MAKEINTRESOURCEW(1));
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        cls.lpszClassName = kind;
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            failure.set("ui.link_import", "RegisterClassEx Win32=" + std::to_string(GetLastError()));
            return false;
        }
        RECT parent{};
        if (!GetWindowRect(owner, &parent)) {
            failure.set("ui.link_import", "Owner bounds Win32=" + std::to_string(GetLastError()));
            return false;
        }
        const UINT dpi = GetDpiForWindow(owner);
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kind, tr(L"导入音乐链接", L"Import music link"),
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            parent.left + MulDiv(30, dpi, 96), parent.top + MulDiv(30, dpi, 96),
            MulDiv(640, dpi, 96), MulDiv(410, dpi, 96), owner, nullptr, cls.hInstance, this);
        if (!window_ || !create_controls(failure)) return false;
        if (!SetPropW(window_, L"MusicBoard.ModelessDialog", reinterpret_cast<HANDLE>(1))) {
            failure.set("ui.link_import", "SetProp Win32=" + std::to_string(GetLastError()));
            return false;
        }
    }
    ShowWindow(window_, SW_SHOW);
    SetFocus(url_);
    return true;
}
bool LinkImportWindow::create_controls(Failure& failure) {
    const UINT dpi = GetDpiForWindow(window_);
    font_ = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    const auto control = [&](DWORD ex, const wchar_t* type, const wchar_t* text, DWORD style,
            int x, int y, int width, int height, int id) {
        HWND child = CreateWindowExW(ex, type, text, WS_CHILD | WS_VISIBLE | style,
            MulDiv(x, dpi, 96), MulDiv(y, dpi, 96), MulDiv(width, dpi, 96), MulDiv(height, dpi, 96),
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return child;
    };
    bool labels = true;
    labels = control(0, L"STATIC", tr(L"音乐或视频链接", L"Music or video URL"), 0, 22, 18, 570, 22, 0) && labels;
    url_ = control(WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 22, 46, 575, 28, 101);
    if (url_) SendMessageW(url_, EM_SETLIMITTEXT, 8192, 0);
    labels = control(0, L"STATIC", tr(L"分类", L"Category"), 0, 22, 94, 100, 22, 0) && labels;
    category_ = control(WS_EX_CLIENTEDGE, L"EDIT", tr(L"链接导入", L"Link imports"), ES_AUTOHSCROLL | WS_TABSTOP,
        22, 120, 365, 28, 102);
    if (category_) SendMessageW(category_, EM_SETLIMITTEXT, 120, 0);
    source_ = control(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 405, 120, 190, 120, 103);
    if (source_) {
        SendMessageW(source_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(L"背景音乐", L"Background music")));
        SendMessageW(source_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(L"音效", L"Sound effects")));
        SendMessageW(source_, CB_SETCURSEL, 0, 0);
    }
    autoplay_ = control(0, L"BUTTON", tr(L"下载完成后播放；文件保留在曲库中，可离线使用", L"Play when downloaded; keep in library for offline use"),
        BS_AUTOCHECKBOX | WS_TABSTOP, 22, 169, 575, 26, 104);
    if (autoplay_) SendMessageW(autoplay_, BM_SETCHECK, BST_CHECKED, 0);
    online_ = control(0, L"BUTTON", tr(L"直接在线播放，不保存音频文件", L"Stream online without saving audio files"),
        BS_AUTOCHECKBOX | WS_TABSTOP, 22, 197, 575, 26, 109);
    status_ = control(0, L"STATIC", tr(L"支持常见音乐平台、视频平台和音频直链", L"Common music/video platforms and direct audio links"),
        0, 22, 230, 575, 48, 105);
    start_ = control(0, L"BUTTON", tr(L"下载到曲库", L"Download to library"), BS_DEFPUSHBUTTON | WS_TABSTOP, 367, 307, 138, 31, 106);
    cancel_ = control(0, L"BUTTON", tr(L"取消任务", L"Cancel task"), WS_TABSTOP, 517, 307, 80, 31, 107);
    details_ = control(0, L"BUTTON", tr(L"查看详情", L"View details"), WS_TABSTOP, 22, 307, 105, 31, 108);
    if (!font_ || !labels || !url_ || !category_ || !source_ || !autoplay_ || !online_ || !status_ || !start_ || !cancel_ || !details_) {
        failure.set("ui.link_import", "Control creation Win32=" + std::to_string(GetLastError()));
        return false;
    }
    EnableWindow(cancel_, FALSE);
    EnableWindow(details_, FALSE);
    return true;
}
void LinkImportWindow::set_details(const std::string& text) {
    detail_text_ = text;
    if (details_) EnableWindow(details_, !text.empty());
}
void LinkImportWindow::set_status(const std::string& text, bool running) {
    if (!window_) return;
    std::wstring wide;
    Failure failure;
    if (utf8_to_wide(text, wide, failure)) SetWindowTextW(status_, wide.c_str());
    EnableWindow(start_, !running);
    EnableWindow(cancel_, running);
    EnableWindow(url_, !running);
    EnableWindow(category_, !running && SendMessageW(online_, BM_GETCHECK, 0, 0) != BST_CHECKED);
    EnableWindow(source_, !running);
    EnableWindow(autoplay_, !running && SendMessageW(online_, BM_GETCHECK, 0, 0) != BST_CHECKED);
    EnableWindow(online_, !running);
}
LRESULT CALLBACK LinkImportWindow::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<LinkImportWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<LinkImportWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle_message(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}
LRESULT LinkImportWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE) { ShowWindow(window_, SW_HIDE); return 0; }
    if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED) {
        if (LOWORD(wparam) == 109) {
            const bool online = SendMessageW(online_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SetWindowTextW(start_, online ? tr(L"在线播放", L"Play online") : tr(L"下载到曲库", L"Download to library"));
            EnableWindow(autoplay_, !online);EnableWindow(category_, !online);return 0;
        }
        if (LOWORD(wparam) == 108 && !detail_text_.empty()) {
            std::wstring wide;
            Failure failure;
            if (utf8_to_wide(detail_text_, wide, failure))
                MessageBoxW(window_, wide.c_str(), tr(L"下载详情", L"Download details"), MB_OK | MB_ICONINFORMATION);
            else set_status(failure.context, false);
            return 0;
        }
        if (LOWORD(wparam) == 107 && on_cancel) { on_cancel(); return 0; }
        if (LOWORD(wparam) == 106 && on_start) {
            LinkImportInput input{text_of(url_), text_of(category_),
                SendMessageW(source_, CB_GETCURSEL, 0, 0) == 1,
                SendMessageW(autoplay_, BM_GETCHECK, 0, 0) == BST_CHECKED,
                SendMessageW(online_, BM_GETCHECK, 0, 0) == BST_CHECKED};
            Failure failure;
            set_details({});
            if (!on_start(input, failure)) {
                set_details(failure.to_string());
                set_status(failure.context, false);
            }
            return 0;
        }
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}
}
