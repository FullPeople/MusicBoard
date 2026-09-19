#pragma once

// ============================================================================
// 回环 HTTP 服务器（只给测试用）
//
// 为什么值得写这么一段：直链下载的每一条分支 —— 有 Content-Length、
// 没有 Content-Length、HTTP 404、中途把连接掐断、慢慢吐字节（好让测试
// 观察到中间进度）—— 都只能由服务器那侧造出来。
// 对着真实的公网 URL 测等于把测试的成败交给别人家的服务器。
//
// 只跑在 127.0.0.1 上，只支持 GET，只按预设脚本应答。不是一个通用服务器。
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace musicboard::test {

struct LoopbackResponse final {
    int status = 200;
    std::string reason = "OK";
    // 空则不发 Content-Length（模拟分块传输 / 未知长度）
    bool send_content_length = true;
    // 额外的响应头，每行一个，要含结尾的 \r\n
    std::string extra_headers;
    std::vector<std::uint8_t> body;
    // 每次写之间歇这么久，并且分片发送 —— 测试因此能在下载中途观察进度
    int chunk_delay_ms = 0;
    std::size_t chunk_size = 0;   // 0 = 一次发完
    // 发到一半就掐断连接（模拟传输中断）
    std::size_t truncate_after_bytes = 0;   // 0 = 不断
};

class LoopbackHttpServer final {
public:
    LoopbackHttpServer() = default;
    ~LoopbackHttpServer() { stop(); }

    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    [[nodiscard]] bool start(LoopbackResponse response, const std::string& path = "/track.wav") {
        response_ = std::move(response);
        path_ = path;

        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return false;
        }
        wsa_started_ = true;

        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) {
            return false;
        }
        // 端口交给系统选，避免并行跑测试时抢端口
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
            SOCKET_ERROR) {
            return false;
        }
        int length = sizeof(address);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) ==
            SOCKET_ERROR) {
            return false;
        }
        port_ = ::ntohs(address.sin_port);
        if (::listen(listener_, 4) == SOCKET_ERROR) {
            return false;
        }

        running_.store(true);
        worker_ = std::thread([this] { serve(); });
        return true;
    }

    void stop() {
        running_.store(false);
        if (listener_ != INVALID_SOCKET) {
            ::closesocket(listener_);
            listener_ = INVALID_SOCKET;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        if (wsa_started_) {
            ::WSACleanup();
            wsa_started_ = false;
        }
    }

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + path_;
    }
    // 一个一定连不上的端口（用一个已经关掉的监听器的端口）
    [[nodiscard]] std::string dead_port_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/closed.wav";
    }
    [[nodiscard]] unsigned short port() const { return port_; }
    [[nodiscard]] int served_requests() const { return served_.load(); }

private:
    void send_all(SOCKET client, const void* data, std::size_t size) const {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        std::size_t sent = 0;
        while (sent < size) {
            const int written =
                ::send(client, reinterpret_cast<const char*>(bytes + sent),
                       static_cast<int>(size - sent), 0);
            if (written <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(written);
        }
    }

    void serve() {
        while (running_.load()) {
            SOCKET client = ::accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                return;   // 监听器被关掉了
            }
            ++served_;

            // 读完请求头就够了，我们不解析它
            char buffer[4096]{};
            static_cast<void>(::recv(client, buffer, sizeof(buffer), 0));

            const std::string head = "HTTP/1.1 " + std::to_string(response_.status) + " " +
                                     response_.reason + "\r\n" +
                                     "Content-Type: audio/wav\r\n" +
                                     (response_.send_content_length
                                          ? "Content-Length: " +
                                                std::to_string(response_.body.size()) + "\r\n"
                                          : std::string{}) +
                                     response_.extra_headers + "Connection: close\r\n\r\n";
            send_all(client, head.data(), head.size());

            const std::size_t limit =
                response_.truncate_after_bytes > 0
                    ? std::min(response_.truncate_after_bytes, response_.body.size())
                    : response_.body.size();
            if (response_.chunk_size == 0) {
                send_all(client, response_.body.data(), limit);
            } else {
                std::size_t offset = 0;
                while (offset < limit && running_.load()) {
                    const std::size_t size =
                        std::min(response_.chunk_size, limit - offset);
                    send_all(client, response_.body.data() + offset, size);
                    offset += size;
                    if (response_.chunk_delay_ms > 0) {
                        ::Sleep(static_cast<DWORD>(response_.chunk_delay_ms));
                    }
                }
            }

            ::shutdown(client, SD_SEND);
            ::closesocket(client);
        }
    }

    LoopbackResponse response_;
    std::string path_ = "/track.wav";
    SOCKET listener_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> served_{0};
    std::thread worker_;
    bool wsa_started_ = false;
};

}  // namespace musicboard::test
