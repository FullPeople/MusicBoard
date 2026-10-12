#include "library/http_download.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <vector>

#include "core/path_utf8.h"
#include "library/library_ops.h"
#include "library/url_classify.h"

#pragma comment(lib, "winhttp.lib")

namespace musicboard {
namespace {

constexpr const wchar_t* kUserAgent = L"MusicBoard/0.1 (Windows)";

[[nodiscard]] std::string lowercase_ascii(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

[[nodiscard]] std::string trim_ascii(std::string_view text) {
    const auto not_space = [](unsigned char ch) { return std::isspace(ch) == 0; };
    const auto begin = std::find_if(text.begin(), text.end(), not_space);
    const auto end = std::find_if(text.rbegin(), text.rend(), not_space).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

[[nodiscard]] std::wstring utf8_to_wide_or_empty(std::string_view text) {
    std::wstring wide;
    Failure ignored;
    if (!utf8_to_wide(text, wide, ignored)) {
        return {};
    }
    return wide;
}

// WinHTTP 的返回码也是 Win32 风格；统一给一句带数字的说明
[[nodiscard]] std::string winhttp_failure(std::string_view what, DWORD error) {
    return std::string(what) + "（WinHTTP/Win32 错误 " + std::to_string(error) + "）";
}

// 把 URL 拆成 WinHTTP 需要的三段。失败时给出明确原因。
[[nodiscard]] bool crack_url(const std::string& url, std::wstring& host, INTERNET_PORT& port,
                             std::wstring& path, bool& secure, Failure& failure) {
    const std::wstring wide_url = utf8_to_wide_or_empty(url);
    if (wide_url.empty()) {
        failure.set(fail_code::invalid_argument, "URL 不是合法 UTF-8: " + url);
        return false;
    }

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    // 只要这三段，其余留 0 让 WinHTTP 自己管
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (::WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &components) ==
        FALSE) {
        failure.set(fail_code::invalid_argument,
                    winhttp_failure("解析 URL 失败: " + url, ::GetLastError()));
        return false;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTP &&
        components.nScheme != INTERNET_SCHEME_HTTPS) {
        failure.set(fail_code::invalid_argument, "只支持 http/https: " + url);
        return false;
    }
    if (components.dwHostNameLength == 0) {
        failure.set(fail_code::invalid_argument, "URL 里没有主机名: " + url);
        return false;
    }

    secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    port = components.nPort;
    host.assign(components.lpszHostName, components.dwHostNameLength);
    path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }
    return true;
}

// 句柄的小包装：保证任何一条提前返回的路径都会关掉它
class WinHttpHandle final {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) noexcept : handle_(handle) {}
    ~WinHttpHandle() { reset(); }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    void reset(HINTERNET handle = nullptr) noexcept {
        if (handle_ != nullptr) {
            ::WinHttpCloseHandle(handle_);
        }
        handle_ = handle;
    }
    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
};

class FileHandle final {
public:
    ~FileHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle() = default;

    [[nodiscard]] bool open(const std::filesystem::path& path) {
        // CREATE_ALWAYS：暂存目录是每个任务独有的空目录，覆盖不会伤到别人
        handle_ = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

}  // namespace

const char* http_download_state_name(HttpDownloadState state) noexcept {
    switch (state) {
        case HttpDownloadState::idle:
            return "idle";
        case HttpDownloadState::running:
            return "running";
        case HttpDownloadState::done:
            return "done";
        case HttpDownloadState::failed:
            return "failed";
        case HttpDownloadState::cancelled:
            return "cancelled";
    }
    return "unknown";
}

std::string filename_from_content_disposition(std::string_view header) {
    const std::string lowered = lowercase_ascii(header);

    // 先找 RFC 5987 的 filename*=UTF-8''%E4%B8%AD...（它是给非 ASCII 名用的）
    const std::size_t extended = lowered.find("filename*=");
    if (extended != std::string::npos) {
        std::string value = trim_ascii(header.substr(extended + 10));
        const std::size_t quote = value.find('\'');
        if (quote != std::string::npos) {
            const std::size_t second = value.find('\'', quote + 1);
            if (second != std::string::npos) {
                value = value.substr(second + 1);
            }
        }
        value = percent_decode(value);
        const std::size_t semicolon = value.find(';');
        if (semicolon != std::string::npos) {
            value.resize(semicolon);
        }
        value = trim_ascii(value);
        // 去掉两侧的引号
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (!value.empty()) {
            return value;
        }
    }

    const std::size_t plain = lowered.find("filename=");
    if (plain == std::string::npos) {
        return {};
    }
    std::string value = trim_ascii(header.substr(plain + 9));
    const std::size_t semicolon = value.find(';');
    if (semicolon != std::string::npos) {
        value.resize(semicolon);
    }
    value = trim_ascii(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

HttpDownload::~HttpDownload() {
    if (state_.load() == HttpDownloadState::running) {
        cancel();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool HttpDownload::start(const HttpDownloadRequest& request, Failure& failure) {
    if (state_.load() != HttpDownloadState::idle) {
        failure.set(fail_code::invalid_argument, "这个下载器已经用过一次了，请新建一个");
        return false;
    }
    if (!url_is_well_formed(request.url)) {
        failure.set(fail_code::invalid_argument, "链接格式不对: " + request.url);
        return false;
    }
    if (request.destination.empty()) {
        failure.set(fail_code::invalid_argument, "没有指定目标文件");
        return false;
    }

    // 先解析一遍 URL：能在这里报的错不要推到工作线程里变成一句"下载失败"
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
    if (!crack_url(request.url, host, port, path, secure, failure)) {
        return false;
    }

    request_ = request;
    received_.store(0);
    total_.store(0);
    http_status_.store(0);
    cancel_requested_.store(false);
    {
        std::lock_guard<std::mutex> lock(text_mutex_);
        failure_reason_.clear();
        content_type_.clear();
        suggested_name_.clear();
    }
    has_total_.store(false);
    status_ = HttpDownloadStatus{};
    state_.store(HttpDownloadState::running);
    worker_ = std::thread([this] { run(); });
    return true;
}

void HttpDownload::run() {
    const auto fail = [this](std::string reason) {
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            failure_reason_ = std::move(reason);
        }
        state_.store(HttpDownloadState::failed);
    };

    WinHttpHandle session(::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.valid()) {
        fail(winhttp_failure("创建 WinHTTP 会话失败", ::GetLastError()));
        return;
    }
    ::WinHttpSetTimeouts(session.get(),
                         static_cast<int>(request_.resolve_timeout_seconds * 1000.0),
                         static_cast<int>(request_.connect_timeout_seconds * 1000.0),
                         static_cast<int>(request_.send_timeout_seconds * 1000.0),
                         static_cast<int>(request_.receive_timeout_seconds * 1000.0));

    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
    Failure crack_failure;
    if (!crack_url(request_.url, host, port, path, secure, crack_failure)) {
        fail(crack_failure.context);
        return;
    }

    WinHttpHandle connection(::WinHttpConnect(session.get(), host.c_str(), port, 0));
    if (!connection.valid()) {
        fail(winhttp_failure("连接主机失败（" + path_to_utf8(std::filesystem::path(host)) + "）",
                             ::GetLastError()));
        return;
    }

    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(::WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              flags));
    if (!request.valid()) {
        fail(winhttp_failure("创建请求失败", ::GetLastError()));
        return;
    }

    if (::WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE) {
        fail(winhttp_failure("发送请求失败", ::GetLastError()));
        return;
    }
    if (::WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
        fail(winhttp_failure("等待响应失败", ::GetLastError()));
        return;
    }

    // ---- 状态码 ----
    {
        DWORD status = 0;
        DWORD size = sizeof(status);
        if (::WinHttpQueryHeaders(request.get(),
                                  WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                  WINHTTP_NO_HEADER_INDEX) == FALSE) {
            fail(winhttp_failure("读取 HTTP 状态码失败", ::GetLastError()));
            return;
        }
        http_status_.store(static_cast<int>(status));
        if (status < 200 || status >= 300) {
            fail("服务器返回 HTTP " + std::to_string(status) + "（期望 2xx）");
            return;
        }
    }

    // ---- Content-Length（可能没有）----
    {
        unsigned long long length = 0;
        DWORD size = sizeof(length);
        if (::WinHttpQueryHeaders(request.get(),
                                  WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &length, &size,
                                  WINHTTP_NO_HEADER_INDEX) != FALSE &&
            length > 0) {
            if (length > request_.maximum_bytes) {
                fail("文件太大（" + std::to_string(length) + " 字节，上限 " +
                     std::to_string(request_.maximum_bytes) + " 字节）");
                return;
            }
            total_.store(static_cast<std::uint64_t>(length));
            // 主线程会读它，所以必须是原子的
            has_total_.store(true);
        }
    }

    // ---- Content-Type / Content-Disposition（只用于显示与命名）----
    //
    // 一次把**整段原始响应头**拉回来，再自己按行找。
    // 不用 WINHTTP_QUERY_CUSTOM 单个查的理由是踩过一次：自定义头的名字匹配
    // 规则比文档看起来微妙（尾冒号、大小写），查不到时它只是返回 FALSE，
    // 于是"服务器明明给了文件名，界面却用了 URL 里的哈希串"，
    // 而且没有任何地方报错。整段拉回来自己找，行为完全在本地。
    {
        std::array<wchar_t, 8192> buffer{};
        DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        if (::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                  WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size,
                                  WINHTTP_NO_HEADER_INDEX) != FALSE) {
            const std::string raw_headers = path_to_utf8(std::filesystem::path(buffer.data()));
            // 按行找两个头。头名字大小写不敏感，值里的内容原样交给各自的解析器。
            const auto header_line = [&raw_headers](std::string_view name) -> std::string {
                const std::string lowered = lowercase_ascii(raw_headers);
                std::size_t offset = 0;
                while (offset <= lowered.size()) {
                    const std::size_t end = lowered.find('\n', offset);
                    const std::string_view line =
                        std::string_view(lowered).substr(offset, end == std::string::npos
                                                                 ? std::string::npos
                                                                 : end - offset);
                    if (line.starts_with(name)) {
                        // 用原文的下标切，不要用 lowered 的 —— 值里的非 ASCII
                        // 字节在 lowered 里已经被改过了（高字节被 tolower 动过）
                        return raw_headers.substr(offset + name.size(),
                                                  line.size() - name.size());
                    }
                    if (end == std::string::npos) {
                        break;
                    }
                    offset = end + 1;
                }
                return {};
            };

            const std::string disposition = header_line("content-disposition:");
            if (!disposition.empty()) {
                std::string name = filename_from_content_disposition(disposition);
                if (!name.empty()) {
                    name = sanitize_to_valid_name(name);
                    std::lock_guard<std::mutex> lock(text_mutex_);
                    suggested_name_ = name;
                }
            }
            const std::string content_type_value = trim_ascii(header_line("content-type:"));
            if (!content_type_value.empty()) {
                std::lock_guard<std::mutex> lock(text_mutex_);
                content_type_ = content_type_value;
            }
        }
    }

    // ---- 落盘 ----
    FileHandle file;
    if (!file.open(request_.destination)) {
        fail("无法创建文件 " + path_to_utf8(request_.destination) +
             "（Win32 错误 " + std::to_string(::GetLastError()) + "）");
        return;
    }

    std::vector<char> chunk(64 * 1024);
    std::uint64_t written = 0;
    for (;;) {
        if (cancel_requested_.load()) {
            state_.store(HttpDownloadState::cancelled);
            return;
        }

        DWORD available = 0;
        if (::WinHttpQueryDataAvailable(request.get(), &available) == FALSE) {
            fail(winhttp_failure("查询可用数据失败", ::GetLastError()));
            return;
        }
        if (available == 0) {
            break;   // 响应结束
        }

        const DWORD want = std::min(available, static_cast<DWORD>(chunk.size()));
        DWORD read = 0;
        if (::WinHttpReadData(request.get(), chunk.data(), want, &read) == FALSE) {
            fail(winhttp_failure("读取数据失败", ::GetLastError()));
            return;
        }
        if (read == 0) {
            break;
        }

        if (written + read > request_.maximum_bytes) {
            fail("文件超过上限 " + std::to_string(request_.maximum_bytes) + " 字节，已中止");
            return;
        }

        DWORD wrote = 0;
        if (::WriteFile(file.get(), chunk.data(), read, &wrote, nullptr) == FALSE ||
            wrote != read) {
            fail("写文件失败 " + path_to_utf8(request_.destination) +
                 "（Win32 错误 " + std::to_string(::GetLastError()) + "）");
            return;
        }
        written += read;
        received_.store(written);
    }

    if (written == 0) {
        // 200 但是零字节：不是音频，明确说出来。
        // 有的服务器会用"空体 + 200"表示"你需要登录"。
        fail("服务器返回成功但没有任何数据（0 字节）。这个链接可能不是文件直链。");
        return;
    }
    // 有 Content-Length 却没下满 → 传输被截断。不能当成成功。
    const std::uint64_t declared = total_.load();
    if (declared > 0 && written != declared) {
        fail("传输不完整：声明 " + std::to_string(declared) + " 字节，实际收到 " +
             std::to_string(written) + " 字节");
        return;
    }

    state_.store(HttpDownloadState::done);
}

void HttpDownload::pump() {
    status_.state = state_.load();
    status_.received_bytes = received_.load();
    status_.total_bytes = total_.load();
    status_.has_total = has_total_.load() && status_.total_bytes > 0;
    // 只有当分母真的存在时才给百分比。没有 Content-Length 就老实显示字节数 ——
    // 编一个百分比正是"进度条是假的"的来源。
    status_.fraction =
        status_.has_total
            ? std::min(1.0, static_cast<double>(status_.received_bytes) /
                                static_cast<double>(status_.total_bytes))
            : 0.0;
    status_.http_status = http_status_.load();

    std::lock_guard<std::mutex> lock(text_mutex_);
    status_.content_type = content_type_;
    status_.suggested_name = suggested_name_;
}

void HttpDownload::cancel() {
    if (state_.load() != HttpDownloadState::running) {
        // 已经结束了。仍然搬一次计数 —— 不然调用方看到的是上一次 pump 的旧值。
        pump();
        return;
    }
    cancel_requested_.store(true);
    // 等它自己收场。工作线程的接收超时是 0.25 秒，所以最多等这么久。
    if (worker_.joinable()) {
        worker_.join();
    }
    if (state_.load() == HttpDownloadState::running) {
        // 走到这里说明工作线程没能收场 —— 说清楚，不假装取消了
        std::lock_guard<std::mutex> lock(text_mutex_);
        failure_reason_ = "已经请求取消，但下载线程没有结束";
    }
    // 把最终状态与计数搬进 status_。不搬的话调用方看到的还是上一次 pump 的旧值 ——
    // "已经取消了，状态却显示还在下"正是要避免的那种不一致。
    pump();
}

bool HttpDownload::finished() const noexcept {
    const HttpDownloadState state = state_.load();
    return state == HttpDownloadState::done || state == HttpDownloadState::failed ||
           state == HttpDownloadState::cancelled;
}

}  // namespace musicboard
