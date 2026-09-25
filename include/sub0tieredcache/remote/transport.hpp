#pragma once

/** @file transport.hpp
 *  @brief The range-fetch seam a caller plugs a real transport into (README.md sec 3/3a: "Remote
 *         acquisition ... belong[s] here"). Mirrors Sub0MemPage's FillBackendRef shape (transfer.hpp)
 *         deliberately -- same "type-erase a concept the caller supplies" idiom, one layer up: here
 *         the erased operation is a validated HTTP-Range-shaped fetch rather than a raw byte fill.
 *
 *         `Http1RangeTransport` (http_client.hpp) is the only transport this package implements
 *         in-process, and it is plain `http://` only -- see docs/remote-mirror.md for why HTTPS is
 *         deliberately not built in (STYLE_GUIDE.md: no third-party dependencies in the header-only
 *         core, and a TLS stack is squarely a third-party dependency). A caller that needs HTTPS
 *         implements this same `fetch_range` shape with libcurl, WinHTTP, or any TLS-capable client
 *         and hands it to `Mirror` exactly like the built-in transport.
 */

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>

#include "sub0tieredcache/remote/status.hpp"

namespace sub0tieredcache::remote {

/// A source's identity for staleness detection (README.md sec 3a: "No TTL, file mtime or advisory
/// callback substitutes for identity"). A strong ETag is the preferred validator; when a server does
/// not advertise one, `last_modified` + `total_size` is the documented fallback -- weaker (a server
/// could serve different bytes at the same mtime+size without either value changing), but strictly
/// better than trusting a TTL. Exactly one of `etag`/`last_modified` is expected to be non-empty in
/// practice; both are compared exactly (byte-for-byte), never hashed away, so a validator collision
/// can never make a stale chunk look fresh.
struct SourceValidator {
    std::string etag;          ///< Raw ETag header value, including quotes, if any. Empty = not used.
    std::string last_modified; ///< Raw Last-Modified header value. Empty = not used (ETag path).
    std::uint64_t total_size = 0; ///< The source's total byte length, as registered by the caller.

    [[nodiscard]] bool operator==(const SourceValidator&) const = default;

    /// A validator with neither identity field set can never be satisfied by any real response --
    /// used to catch a mis-registered source before any network call is made.
    [[nodiscard]] bool has_identity() const noexcept { return !etag.empty() || !last_modified.empty(); }
};

/// One bounded byte-range fetch request. `destination.size()` is the exact number of bytes the
/// caller expects back; a transport must never write more than that, and reports fewer only via
/// `FetchRangeResult::bytes` together with a non-`ok` status (never a silent short fill).
struct RangeFetchRequest {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::span<std::byte> destination; ///< Non-owning; caller-owned, size() == length.
    SourceValidator expected;         ///< The validator captured at registration; compared against
                                       ///< what the response actually reports (README.md sec 3a).
};

/// What a transport reports for one `fetch_range` call, after this package's own validation
/// (integration-plan.md "Versioning and remote tier": "Validate range status/length, source-validator
/// consistency, bounds, retries and truncated responses").
struct FetchRangeResult {
    Status status = Status::ok;
    std::uint64_t bytes = 0;      ///< Bytes actually written into `destination`. Only meaningful
                                   ///< (and only ever `== length`) when `status == Status::ok`.
    SourceValidator observed{};   ///< The validator the response actually carried, whether or not it
                                   ///< matched `expected` -- callers use this to detect the exact
                                   ///< mismatch that produced `Status::source_changed`.
    unsigned retries = 0;         ///< How many transient-failure retries this call consumed before
                                   ///< reaching its final status (0 on a first-attempt success).
};

/** @brief Type-erased, non-owning reference to a range-fetch transport.
 *
 *  Transport contract (checked by the exposition-only concept below, documented here because a
 *  concept cannot carry prose):
 *  - `fetch_range` is synchronous and may block on I/O -- this whole package lives on the
 *    resolve-pass/administrative side (never `try_get`'s hot path, REQUIREMENTS.md R2), so blocking
 *    and allocating here are both fine (matches Sub0MemPage's own "administrative calls may block/
 *    allocate" precedent).
 *  - A transport performs its own bounded retries for the failures `is_transient()` names and
 *    reports the total retry count in the result; it never retries a validator mismatch, a bad
 *    Content-Range, or a caller-mistake status.
 *  - `fetch_range` never writes past `request.destination` and never reports `status == ok` with
 *    `bytes != request.length` -- a short/truncated response is a distinct failing status
 *    (`Status::truncated`), never a shorter "success".
 *
 *  Erased (not a template parameter) for the same reason as `FillBackendRef`: it lets `Mirror` stay
 *  a plain, non-template type while still accepting any conforming transport (the built-in HTTP
 *  client, a test fixture, or a caller's own TLS-capable client) -- one indirect call per fetch,
 *  negligible next to the network I/O it issues.
 */
class RangeTransportRef {
public:
    template <class Transport>
        requires(!std::same_as<std::remove_cv_t<Transport>, RangeTransportRef>) &&
                requires(Transport& transport, const RangeFetchRequest& request) {
            { transport.fetch_range(request) } -> std::same_as<FetchRangeResult>;
        }
    explicit RangeTransportRef(Transport& transport) noexcept
        : transport_(&transport), fetch_([](void* self, const RangeFetchRequest& request) {
              return static_cast<Transport*>(self)->fetch_range(request);
          }) {}

    [[nodiscard]] FetchRangeResult fetch_range(const RangeFetchRequest& request) const {
        return fetch_(transport_, request);
    }

private:
    void* transport_; // non-owning; must outlive every Mirror registered against it
    FetchRangeResult (*fetch_)(void*, const RangeFetchRequest&);
};

} // namespace sub0tieredcache::remote
