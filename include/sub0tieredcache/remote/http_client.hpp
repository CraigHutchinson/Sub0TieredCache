#pragma once

/** @file http_client.hpp
 *  @brief Minimal built-in HTTP/1.1 Range client, `http://` only. See docs/remote-mirror.md for the
 *         full contract; the short version:
 *
 *         - POSIX sockets on Linux/macOS, Winsock behind `#ifdef _WIN32` (STYLE_GUIDE.md: no
 *           unconditional platform-only header). The Windows path could not be compiled or run in
 *           this container -- written carefully against MSVC /W4 /W4 conventions (NOMINMAX +
 *           WIN32_LEAN_AND_MEAN before `<winsock2.h>`, `ws2_32` linked by CMake) but is unverified;
 *           docs/remote-mirror.md lists it as such.
 *         - HTTPS is explicitly NOT implemented (STYLE_GUIDE.md: no third-party dependency in the
 *           header-only core, and TLS is squarely one). A caller needing `https://` implements
 *           `RangeTransportRef`'s `fetch_range` shape with libcurl/WinHTTP/etc.
 *         - No redirects in v1: any 3xx is `Status::unexpected_status`, documented, not followed.
 *         - `Transfer-Encoding: chunked` responses are not supported (`Status::unsupported_response`)
 *           -- the client only understands a `Content-Length`-framed body, which is what every
 *           standard HTTP Range (206) response uses.
 *         - Bounded retries with backoff for `is_transient()` failures only (connect/send/recv
 *           timeout, connection reset/truncated body, 5xx) -- never for a validator mismatch, a bad
 *           Content-Range, or a caller mistake, per integration-plan.md's "Versioning and remote
 *           tier" section.
 */

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "sub0tieredcache/remote/status.hpp"
#include "sub0tieredcache/remote/transport.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
// ws2_32 is linked by the Sub0TieredCache CMake target on WIN32 (no #pragma comment: MinGW rejects it).
using sub0tieredcache_socket_t = SOCKET;
#define SUB0TIEREDCACHE_INVALID_SOCKET INVALID_SOCKET
#define SUB0TIEREDCACHE_SOCKET_ERROR SOCKET_ERROR
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using sub0tieredcache_socket_t = int;
#define SUB0TIEREDCACHE_INVALID_SOCKET (-1)
#define SUB0TIEREDCACHE_SOCKET_ERROR (-1)
#endif

namespace sub0tieredcache::remote {

namespace detail {

#if defined(_WIN32)
/// Winsock needs one process-wide WSAStartup/WSACleanup pair. A function-local static keeps this
/// header-only (no separate translation unit needed) while still initializing exactly once,
/// regardless of how many Http1RangeTransport instances exist.
struct WsaGuard {
    WsaGuard() noexcept {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WsaGuard() noexcept {
        if (ok) {
            WSACleanup();
        }
    }
    bool ok = false;
};
inline void ensure_wsa_started() noexcept {
    static WsaGuard guard;
    (void)guard;
}
inline void close_socket(sub0tieredcache_socket_t s) noexcept { closesocket(s); }
inline int last_socket_error() noexcept { return WSAGetLastError(); }
inline void set_nonblocking(sub0tieredcache_socket_t s, bool nonblocking) noexcept {
    u_long mode = nonblocking ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
}
#else
inline void ensure_wsa_started() noexcept {}
inline void close_socket(sub0tieredcache_socket_t s) noexcept { ::close(s); }
inline int last_socket_error() noexcept { return errno; }
inline void set_nonblocking(sub0tieredcache_socket_t s, bool nonblocking) noexcept {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return;
    }
    fcntl(s, F_SETFL, nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
#endif

/// RAII socket handle so every early-return path in fetch_once() closes cleanly.
class SocketHandle {
public:
    SocketHandle() noexcept { ensure_wsa_started(); }
    explicit SocketHandle(sub0tieredcache_socket_t s) noexcept : socket_(s) { ensure_wsa_started(); }
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    SocketHandle(SocketHandle&& other) noexcept : socket_(other.socket_) { other.socket_ = SUB0TIEREDCACHE_INVALID_SOCKET; }
    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this != &other) {
            reset();
            socket_ = other.socket_;
            other.socket_ = SUB0TIEREDCACHE_INVALID_SOCKET;
        }
        return *this;
    }
    ~SocketHandle() { reset(); }

    void reset(sub0tieredcache_socket_t s = SUB0TIEREDCACHE_INVALID_SOCKET) noexcept {
        if (socket_ != SUB0TIEREDCACHE_INVALID_SOCKET) {
            close_socket(socket_);
        }
        socket_ = s;
    }
    [[nodiscard]] sub0tieredcache_socket_t get() const noexcept { return socket_; }
    [[nodiscard]] bool valid() const noexcept { return socket_ != SUB0TIEREDCACHE_INVALID_SOCKET; }

private:
    sub0tieredcache_socket_t socket_ = SUB0TIEREDCACHE_INVALID_SOCKET;
};

/// A parsed `http://host[:port]/path` URL. Only the one scheme this client supports.
struct ParsedUrl {
    std::string host;
    std::string port = "80";
    std::string path = "/";
    bool valid = false;
};

[[nodiscard]] inline ParsedUrl parse_http_url(std::string_view url) {
    ParsedUrl result;
    constexpr std::string_view prefix = "http://";
    if (url.substr(0, prefix.size()) != prefix) {
        return result; // valid stays false: not http://, including https:// -- documented, not a bug.
    }
    std::string_view rest = url.substr(prefix.size());
    const auto path_pos = rest.find('/');
    std::string_view authority = path_pos == std::string_view::npos ? rest : rest.substr(0, path_pos);
    if (authority.empty()) {
        return result;
    }
    result.path = path_pos == std::string_view::npos ? std::string("/") : std::string(rest.substr(path_pos));
    if (result.path.empty()) {
        result.path = "/";
    }
    const auto colon_pos = authority.rfind(':');
    if (colon_pos != std::string_view::npos) {
        result.host = std::string(authority.substr(0, colon_pos));
        result.port = std::string(authority.substr(colon_pos + 1));
        if (result.port.empty()) {
            return result;
        }
    } else {
        result.host = std::string(authority);
    }
    if (result.host.empty()) {
        return result;
    }
    result.valid = true;
    return result;
}

/// Case-insensitive header-name compare (RFC 7230 headers are case-insensitive).
[[nodiscard]] inline bool header_name_is(std::string_view line_lower_name, std::string_view name) noexcept {
    if (line_lower_name.size() != name.size()) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(line_lower_name[i])) != std::tolower(static_cast<unsigned char>(name[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

struct ParsedHeaders {
    int status_code = 0;
    std::string content_range; // raw value, e.g. "bytes 10-19/100"
    std::string etag;
    std::string last_modified;
    std::string transfer_encoding;
    long long content_length = -1;
    bool has_content_length = false;
};

/// Parses the status line + headers out of `text` (already known to contain a full "\r\n\r\n").
/// Returns the byte offset of the body's first byte within `text`.
[[nodiscard]] inline std::size_t parse_response_head(std::string_view text, ParsedHeaders& out) {
    const auto header_end = text.find("\r\n\r\n");
    std::string_view head = text.substr(0, header_end);
    std::size_t line_start = 0;
    bool first = true;
    while (line_start <= head.size()) {
        const auto line_end = head.find("\r\n", line_start);
        std::string_view line = line_end == std::string_view::npos ? head.substr(line_start) : head.substr(line_start, line_end - line_start);
        if (first) {
            first = false;
            // "HTTP/1.1 206 Partial Content"
            const auto sp1 = line.find(' ');
            if (sp1 != std::string_view::npos) {
                std::string_view rest = line.substr(sp1 + 1);
                const auto sp2 = rest.find(' ');
                std::string_view code = sp2 == std::string_view::npos ? rest : rest.substr(0, sp2);
                int value = 0;
                std::from_chars(code.data(), code.data() + code.size(), value);
                out.status_code = value;
            }
        } else if (!line.empty()) {
            const auto colon = line.find(':');
            if (colon != std::string_view::npos) {
                std::string_view name = line.substr(0, colon);
                std::string_view value = trim(line.substr(colon + 1));
                if (header_name_is(name, "Content-Range")) {
                    out.content_range = std::string(value);
                } else if (header_name_is(name, "ETag")) {
                    out.etag = std::string(value);
                } else if (header_name_is(name, "Last-Modified")) {
                    out.last_modified = std::string(value);
                } else if (header_name_is(name, "Transfer-Encoding")) {
                    out.transfer_encoding = std::string(value);
                } else if (header_name_is(name, "Content-Length")) {
                    long long len = -1;
                    auto conv = std::from_chars(value.data(), value.data() + value.size(), len);
                    if (conv.ec == std::errc{}) {
                        out.content_length = len;
                        out.has_content_length = true;
                    }
                }
            }
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 2;
    }
    return header_end + 4;
}

/// Parses "bytes start-end/total" -- the only Content-Range form this client accepts. Returns false
/// on anything else (including "bytes */123", the unsatisfiable-range form: treated uniformly as a
/// bad/unmatched Content-Range by the caller, since a v1 client never asks for an unsatisfiable range).
[[nodiscard]] inline bool parse_content_range(std::string_view value, std::uint64_t& start, std::uint64_t& end, std::uint64_t& total) {
    constexpr std::string_view prefix = "bytes ";
    if (value.substr(0, prefix.size()) != prefix) {
        return false;
    }
    std::string_view rest = value.substr(prefix.size());
    const auto dash = rest.find('-');
    const auto slash = rest.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos || slash < dash) {
        return false;
    }
    std::string_view start_sv = rest.substr(0, dash);
    std::string_view end_sv = rest.substr(dash + 1, slash - dash - 1);
    std::string_view total_sv = rest.substr(slash + 1);
    auto conv1 = std::from_chars(start_sv.data(), start_sv.data() + start_sv.size(), start);
    auto conv2 = std::from_chars(end_sv.data(), end_sv.data() + end_sv.size(), end);
    auto conv3 = std::from_chars(total_sv.data(), total_sv.data() + total_sv.size(), total);
    return conv1.ec == std::errc{} && conv2.ec == std::errc{} && conv3.ec == std::errc{};
}

} // namespace detail

/// The built-in `http://`-only Range transport. See the file comment for the full contract.
class Http1RangeTransport {
public:
    struct Options {
        std::string url; ///< "http://host[:port]/path" -- the resource whose byte ranges are fetched.
        std::chrono::milliseconds connect_timeout{2000};
        std::chrono::milliseconds recv_timeout{5000};
        unsigned max_retries = 3; ///< Additional attempts after the first, only for is_transient() statuses.
        std::chrono::milliseconds retry_backoff_base{2};
    };

    explicit Http1RangeTransport(Options options) : options_(std::move(options)), url_(detail::parse_http_url(options_.url)) {}

    /// Satisfies RangeTransportRef's transport contract: synchronous, retries transient failures
    /// internally, never retries a validator/content-range/caller-mistake status.
    [[nodiscard]] FetchRangeResult fetch_range(const RangeFetchRequest& request) const {
        if (!url_.valid) {
            return {.status = Status::invalid_argument};
        }
        if (request.length == 0 || request.destination.size() != request.length) {
            return {.status = Status::invalid_argument};
        }

        unsigned retries = 0;
        for (;;) {
            FetchRangeResult result = fetch_once(request);
            result.retries = retries;
            if (result.status == Status::ok || !is_transient(result.status)) {
                return result;
            }
            if (retries >= options_.max_retries) {
                // Only remap to retries_exhausted once at least one retry was actually spent: with
                // max_retries == 0 the caller asked for exactly one raw attempt, so its own specific
                // transient status (truncated, connect_failed, ...) is preserved rather than
                // collapsed into a generic "exhausted" that would lose which failure it was.
                if (retries > 0) {
                    result.status = Status::retries_exhausted;
                }
                return result;
            }
            ++retries;
            // Simple bounded exponential backoff; capped so the offline test suite (a "503 then
            // success" fixture, sub_mirror_tests.cpp) stays fast rather than because of any measured
            // production tuning -- deliberately not cited as prior art, it is not a policy choice.
            const auto backoff = options_.retry_backoff_base * (1u << std::min(retries, 4u));
            std::this_thread::sleep_for(backoff);
        }
    }

private:
    [[nodiscard]] FetchRangeResult fetch_once(const RangeFetchRequest& request) const {
        using namespace detail;

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* resolved = nullptr;
        ensure_wsa_started();
        if (getaddrinfo(url_.host.c_str(), url_.port.c_str(), &hints, &resolved) != 0 || resolved == nullptr) {
            return {.status = Status::connect_failed};
        }
        struct AddrInfoGuard {
            addrinfo* p;
            ~AddrInfoGuard() { freeaddrinfo(p); }
        } addr_guard{resolved};

        SocketHandle sock;
        bool connected = false;
        for (addrinfo* ai = resolved; ai != nullptr; ai = ai->ai_next) {
            sub0tieredcache_socket_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s == SUB0TIEREDCACHE_INVALID_SOCKET) {
                continue;
            }
            SocketHandle candidate(s);
            set_nonblocking(candidate.get(), true);
            const int rc = connect(candidate.get(), ai->ai_addr, static_cast<int>(ai->ai_addrlen));
            if (rc == 0) {
                connected = true;
            } else {
#if defined(_WIN32)
                const bool in_progress = last_socket_error() == WSAEWOULDBLOCK;
#else
                const bool in_progress = last_socket_error() == EINPROGRESS;
#endif
                if (in_progress && wait_writable(candidate.get(), options_.connect_timeout)) {
                    connected = connect_succeeded(candidate.get());
                }
            }
            if (connected) {
                set_nonblocking(candidate.get(), false);
                sock = std::move(candidate);
                break;
            }
        }
        if (!connected || !sock.valid()) {
            return {.status = Status::connect_failed};
        }

        set_recv_timeout(sock.get(), options_.recv_timeout);

        const std::uint64_t range_end = request.offset + request.length - 1;
        std::string req;
        req.reserve(128 + url_.path.size() + url_.host.size());
        req += "GET " + url_.path + " HTTP/1.1\r\n";
        req += "Host: " + url_.host + "\r\n";
        req += "Range: bytes=" + std::to_string(request.offset) + "-" + std::to_string(range_end) + "\r\n";
        req += "Connection: close\r\n";
        req += "User-Agent: sub0tieredcache/1\r\n";
        req += "\r\n";

        std::size_t sent_total = 0;
        while (sent_total < req.size()) {
#if defined(_WIN32)
            const int sent = send(sock.get(), req.data() + sent_total, static_cast<int>(req.size() - sent_total), 0);
#else
            const auto sent = ::send(sock.get(), req.data() + sent_total, req.size() - sent_total, 0);
#endif
            if (sent <= 0) {
                return {.status = Status::send_failed};
            }
            sent_total += static_cast<std::size_t>(sent);
        }

        // Read until the header terminator is seen, then read exactly Content-Length more bytes.
        std::vector<char> buffer;
        buffer.reserve(4096);
        std::size_t header_end = std::string_view::npos;
        char chunk[4096];
        for (;;) {
#if defined(_WIN32)
            const int received = recv(sock.get(), chunk, sizeof(chunk), 0);
#else
            const auto received = ::recv(sock.get(), chunk, sizeof(chunk), 0);
#endif
            if (received == 0) {
                return {.status = Status::truncated}; // closed before headers completed
            }
            if (received < 0) {
                return {.status = is_recv_timeout() ? Status::timeout : Status::truncated};
            }
            buffer.insert(buffer.end(), chunk, chunk + received);
            std::string_view text(buffer.data(), buffer.size());
            if (const auto pos = text.find("\r\n\r\n"); pos != std::string_view::npos) {
                header_end = pos + 4;
                break;
            }
            if (buffer.size() > (1u << 20)) { // headers this large would be pathological; bail out.
                return {.status = Status::unexpected_status};
            }
        }

        ParsedHeaders headers;
        std::string_view text(buffer.data(), buffer.size());
        header_end = parse_response_head(text, headers);

        if (headers.status_code >= 500 && headers.status_code < 600) {
            return {.status = Status::server_error};
        }
        if (headers.status_code != 206) {
            // 200 (full body to a range request), any 3xx (no redirects in v1) and any other 4xx all
            // land here, documented: never silently read as if the whole resource were the range.
            return {.status = Status::unexpected_status};
        }
        if (!headers.transfer_encoding.empty()) {
            return {.status = Status::unsupported_response};
        }
        if (!headers.has_content_length) {
            return {.status = Status::bad_content_range};
        }

        std::uint64_t cr_start = 0, cr_end = 0, cr_total = 0;
        if (headers.content_range.empty() || !parse_content_range(headers.content_range, cr_start, cr_end, cr_total)) {
            return {.status = Status::bad_content_range};
        }
        if (cr_start != request.offset || cr_end != range_end) {
            return {.status = Status::bad_content_range};
        }
        if (static_cast<std::uint64_t>(headers.content_length) != request.length) {
            return {.status = Status::bad_content_range};
        }

        FetchRangeResult result;
        result.observed.etag = headers.etag;
        result.observed.last_modified = headers.last_modified;
        result.observed.total_size = cr_total;

        if (request.expected.total_size != 0 && cr_total != request.expected.total_size) {
            result.status = Status::size_mismatch;
            return result;
        }
        if (request.expected.has_identity()) {
            const bool etag_ok = !request.expected.etag.empty() && request.expected.etag == result.observed.etag;
            const bool lm_ok = request.expected.etag.empty() && !request.expected.last_modified.empty() &&
                                request.expected.last_modified == result.observed.last_modified;
            if (!etag_ok && !lm_ok) {
                result.status = Status::source_changed;
                return result;
            }
        }

        // Body bytes already in `buffer` past header_end, plus whatever remains to be read.
        std::size_t already = buffer.size() - header_end;
        std::size_t to_copy = std::min<std::size_t>(already, request.length);
        std::memcpy(request.destination.data(), buffer.data() + header_end, to_copy);
        std::size_t received_total = to_copy;

        while (received_total < request.length) {
            const std::size_t want = std::min<std::size_t>(sizeof(chunk), request.length - received_total);
#if defined(_WIN32)
            const int received = recv(sock.get(), chunk, static_cast<int>(want), 0);
#else
            const auto received = ::recv(sock.get(), chunk, want, 0);
#endif
            if (received == 0) {
                result.status = Status::truncated; // connection closed before the body completed
                result.bytes = received_total;
                return result;
            }
            if (received < 0) {
                result.status = is_recv_timeout() ? Status::timeout : Status::truncated;
                result.bytes = received_total;
                return result;
            }
            std::memcpy(request.destination.data() + received_total, chunk, static_cast<std::size_t>(received));
            received_total += static_cast<std::size_t>(received);
        }

        result.status = Status::ok;
        result.bytes = received_total;
        return result;
    }

    [[nodiscard]] bool wait_writable(sub0tieredcache_socket_t s, std::chrono::milliseconds timeout) const {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(s, &write_set);
        timeval tv{};
        tv.tv_sec = static_cast<long>(timeout.count() / 1000);
        tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
#if defined(_WIN32)
        const int rc = select(0, nullptr, &write_set, nullptr, &tv);
#else
        const int rc = select(s + 1, nullptr, &write_set, nullptr, &tv);
#endif
        return rc > 0 && FD_ISSET(s, &write_set);
    }

    [[nodiscard]] bool connect_succeeded(sub0tieredcache_socket_t s) const {
        int err = 0;
#if defined(_WIN32)
        int len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
#else
        socklen_t len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
        return err == 0;
    }

    void set_recv_timeout(sub0tieredcache_socket_t s, std::chrono::milliseconds timeout) const {
#if defined(_WIN32)
        DWORD ms = static_cast<DWORD>(timeout.count());
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
        tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    [[nodiscard]] static bool is_recv_timeout() noexcept {
#if defined(_WIN32)
        return detail::last_socket_error() == WSAETIMEDOUT;
#else
        const int err = errno;
        return err == EAGAIN || err == EWOULDBLOCK;
#endif
    }

    Options options_;
    detail::ParsedUrl url_;
};

} // namespace sub0tieredcache::remote
