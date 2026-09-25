#pragma once

/** @file test_http_server.hpp
 *  @brief An in-process, single-purpose HTTP/1.1 range server used only by this package's own
 *         tests (docs/integration-plan.md "Independent test strategy": "Remote HTTP is optional and
 *         tested with a local range server first"). Not part of the public library -- lives under
 *         tests/, not include/. Serves a fixed, deterministic byte buffer and can be told, per
 *         upcoming request, to misbehave in one of the specific ways the HTTP client must detect:
 *         wrong status, bad Content-Range, a truncated body, a connection reset mid-body, transient
 *         5xx, or a wrong total size / changed ETag.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using test_socket_t = SOCKET;
#define TEST_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using test_socket_t = int;
#define TEST_INVALID_SOCKET (-1)
#endif

namespace sub0tieredcache::test {

class TestRangeServer {
public:
    enum class Behavior {
        normal,           ///< Correct 206 response.
        full_body_200,    ///< 200 with the whole resource -- must be rejected, not silently read.
        bad_content_range, ///< 206 whose Content-Range doesn't match the request.
        truncated_body,   ///< 206 with correct headers, connection closed partway through the body.
        reset_mid_body,   ///< Like truncated_body, but via a forced RST rather than a clean close.
        server_error_503, ///< Transient failure -- client is expected to retry.
        wrong_total_size, ///< 206 whose Content-Range total doesn't match the registered size.
    };

    explicit TestRangeServer(std::vector<std::byte> content, std::string etag)
        : content_(std::move(content)), etag_(std::move(etag)) {
#if defined(_WIN32)
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
        listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // ephemeral
        bind(listen_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        getsockname(listen_socket_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        listen(listen_socket_, 16);
        acceptor_ = std::thread([this] { accept_loop(); });
    }

    ~TestRangeServer() {
        stop_.store(true, std::memory_order_relaxed);
#if defined(_WIN32)
        closesocket(listen_socket_);
        acceptor_.join();
        for (auto& t : handlers_) t.join();
        WSACleanup();
#else
        ::shutdown(listen_socket_, SHUT_RDWR);
        ::close(listen_socket_);
        acceptor_.join();
        for (auto& t : handlers_) t.join();
#endif
    }

    TestRangeServer(const TestRangeServer&) = delete;
    TestRangeServer& operator=(const TestRangeServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/data"; }
    [[nodiscard]] std::uint64_t total_size() const noexcept { return content_.size(); }

    /// Queues one behavior for the next incoming request; requests beyond the queue use `normal`.
    void push_behavior(Behavior b) {
        std::scoped_lock lock(mutex_);
        behaviors_.push_back(b);
    }

    void set_etag(std::string etag) {
        std::scoped_lock lock(mutex_);
        etag_ = std::move(etag);
    }

    [[nodiscard]] std::size_t request_count() const noexcept { return request_count_.load(std::memory_order_relaxed); }

private:
    void accept_loop() {
        while (!stop_.load(std::memory_order_relaxed)) {
            sockaddr_in peer{};
#if defined(_WIN32)
            int peer_len = sizeof(peer);
#else
            socklen_t peer_len = sizeof(peer);
#endif
            test_socket_t client = accept(listen_socket_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (client == TEST_INVALID_SOCKET) {
                break; // listen socket closed (shutdown) -> destructor is tearing us down.
            }
            handlers_.emplace_back([this, client] { handle_connection(client); });
        }
    }

    void handle_connection(test_socket_t client) {
        std::string request;
        char buf[4096];
        for (;;) {
#if defined(_WIN32)
            const int n = recv(client, buf, sizeof(buf), 0);
#else
            const auto n = ::recv(client, buf, sizeof(buf), 0);
#endif
            if (n <= 0) {
                close_socket(client);
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
            if (request.find("\r\n\r\n") != std::string::npos) {
                break;
            }
        }

        std::uint64_t range_start = 0, range_end = content_.size() - 1;
        parse_range(request, range_start, range_end);
        ++request_count_;

        Behavior behavior = Behavior::normal;
        {
            std::scoped_lock lock(mutex_);
            if (!behaviors_.empty()) {
                behavior = behaviors_.front();
                behaviors_.pop_front();
            }
        }

        serve(client, behavior, range_start, range_end);
        close_socket(client);
    }

    static void close_socket(test_socket_t s) {
#if defined(_WIN32)
        closesocket(s);
#else
        ::close(s);
#endif
    }

    static void parse_range(const std::string& request, std::uint64_t& start, std::uint64_t& end) {
        const auto pos = request.find("Range: bytes=");
        if (pos == std::string::npos) {
            return;
        }
        const auto dash = request.find('-', pos);
        const auto line_end = request.find("\r\n", pos);
        if (dash == std::string::npos || line_end == std::string::npos) {
            return;
        }
        start = std::stoull(request.substr(pos + 13, dash - (pos + 13)));
        end = std::stoull(request.substr(dash + 1, line_end - dash - 1));
    }

    void send_all(test_socket_t s, const void* data, std::size_t len) {
        const char* p = static_cast<const char*>(data);
        std::size_t sent = 0;
        while (sent < len) {
#if defined(_WIN32)
            const int n = send(s, p + sent, static_cast<int>(len - sent), 0);
#else
            const auto n = ::send(s, p + sent, len - sent, 0);
#endif
            if (n <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(n);
        }
    }

    void serve(test_socket_t client, Behavior behavior, std::uint64_t range_start, std::uint64_t range_end) {
        const std::uint64_t total = content_.size();
        const std::uint64_t requested_len = range_end >= range_start ? (range_end - range_start + 1) : 0;
        std::string etag_copy;
        {
            std::scoped_lock lock(mutex_);
            etag_copy = etag_;
        }

        if (behavior == Behavior::server_error_503) {
            std::string resp = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(client, resp.data(), resp.size());
            return;
        }

        if (behavior == Behavior::full_body_200) {
            std::string headers = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(total) +
                                   "\r\nETag: " + etag_copy + "\r\nConnection: close\r\n\r\n";
            send_all(client, headers.data(), headers.size());
            send_all(client, content_.data(), content_.size());
            return;
        }

        std::uint64_t content_range_total = total;
        std::uint64_t effective_len = requested_len;
        if (behavior == Behavior::wrong_total_size) {
            content_range_total = total + 999; // deliberately wrong
        }
        if (behavior == Behavior::bad_content_range) {
            // Advertise a Content-Range that doesn't match the requested start/end.
            std::string headers = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-0/" + std::to_string(total) +
                                   "\r\nContent-Length: " + std::to_string(effective_len) + "\r\nETag: " + etag_copy +
                                   "\r\nConnection: close\r\n\r\n";
            send_all(client, headers.data(), headers.size());
            send_all(client, content_.data() + range_start, effective_len);
            return;
        }

        std::string headers = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(range_start) + "-" +
                               std::to_string(range_end) + "/" + std::to_string(content_range_total) +
                               "\r\nContent-Length: " + std::to_string(effective_len) + "\r\nETag: " + etag_copy +
                               "\r\nConnection: close\r\n\r\n";
        send_all(client, headers.data(), headers.size());

        if (behavior == Behavior::truncated_body) {
            const std::uint64_t half = effective_len / 2;
            send_all(client, content_.data() + range_start, half);
            return; // close without sending the rest
        }
        if (behavior == Behavior::reset_mid_body) {
            const std::uint64_t half = effective_len / 2;
            send_all(client, content_.data() + range_start, half);
#if !defined(_WIN32)
            linger l{1, 0};
            setsockopt(client, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
#endif
            return; // handle_connection's close_socket() right after this sends a RST (SO_LINGER{1,0})
        }

        send_all(client, content_.data() + range_start, effective_len);
    }

    std::vector<std::byte> content_;
    std::string etag_;
    test_socket_t listen_socket_ = TEST_INVALID_SOCKET;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread acceptor_;
    std::vector<std::thread> handlers_;
    std::atomic<std::size_t> request_count_{0};
    std::mutex mutex_;
    std::deque<Behavior> behaviors_;
};

} // namespace sub0tieredcache::test
