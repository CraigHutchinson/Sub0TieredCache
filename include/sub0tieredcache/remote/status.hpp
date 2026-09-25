#pragma once

/** @file status.hpp
 *  @brief Outcomes for the remote-mirror seam (docs/remote-mirror.md). Every failure this package
 *         can produce -- transport-level, validation-level or local-cache-level -- is a distinct
 *         value here, matching AGENTS.md/README.md's "Status values, not exceptions on hot paths"
 *         convention already established by Sub0MemPage's own transfer.hpp. This whole package sits
 *         on the resolve-pass/administrative side (never on try_get's hot path -- REQUIREMENTS.md
 *         R2), so calls here may still block or allocate; they just never throw as their primary
 *         error channel.
 */

#include <cstdint>

namespace sub0tieredcache::remote {

enum class Status : std::uint8_t {
    ok,

    // -- request-shape errors (caller mistakes, checked before any I/O) --
    invalid_argument, ///< Malformed URL/options, zero-length range, destination span too small, etc.
    out_of_range,     ///< [offset, offset+length) extends past the registered total size.

    // -- transport-level, potentially transient (bounded retry candidates) --
    connect_failed, ///< Could not establish a TCP connection within connect_timeout.
    send_failed,    ///< The request could not be written to the socket.
    timeout,        ///< No response within recv_timeout.
    truncated,      ///< Connection closed / reset before the promised body length was received.
    server_error,   ///< HTTP 5xx -- transient by convention, retried like the transport failures above.

    // -- transport-level, not retried (a different request wouldn't fix it) --
    unexpected_status, ///< Any non-206 response that isn't 5xx: 200 (full body), 3xx (no redirects
                        ///< in v1), 4xx. A 200 to a range request is explicitly an error, never
                        ///< silently read as if it were the whole file.
    bad_content_range, ///< 206 response with a missing/malformed Content-Range, or one that doesn't
                        ///< exactly match the requested byte range.
    unsupported_response, ///< e.g. Transfer-Encoding: chunked -- not supported by the built-in v1 client.

    // -- validation-level (README.md sec 3a / integration-plan.md "Versioning and remote tier") --
    source_changed, ///< The response's ETag (or Last-Modified+size fallback) does not match the
                     ///< validator captured at registration. Nothing is published on this outcome.
    size_mismatch,  ///< The response's reported total size does not match the registered total size.

    // -- retry bookkeeping --
    retries_exhausted, ///< Every bounded retry attempt ended in a transient failure.

    // -- local chunk-mirror (disk tier) --
    not_found,       ///< No local chunk file for this (source identity, chunk index) -- a plain miss.
    corrupt,         ///< A local chunk file exists but failed header/validator/checksum validation;
                      ///< treated exactly like a miss by callers, never served.
    publish_failed,  ///< The atomic temp-write + rename publish step failed (disk full, permissions).
    io_error,        ///< Any other local filesystem I/O failure (open/read/write outside the above).
};

/// True for the subset of `Status` this package's bounded retry loop treats as transient -- worth
/// retrying with backoff. Every other failure (validator mismatch, bad content-range, size mismatch,
/// caller mistakes) is retried never: a different attempt at the same request would not fix it.
[[nodiscard]] constexpr bool is_transient(Status status) noexcept {
    switch (status) {
        case Status::connect_failed:
        case Status::send_failed:
        case Status::timeout:
        case Status::truncated:
        case Status::server_error:
            return true;
        default:
            return false;
    }
}

} // namespace sub0tieredcache::remote
