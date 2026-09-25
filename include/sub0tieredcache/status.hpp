#pragma once

/** @file status.hpp
 *  @brief Status vocabulary for the row-cache contract. Mirrors sub0mempage::Status's shape (see
 *         Sub0MemPage's transfer.hpp) deliberately -- STYLE_GUIDE.md asks for std::expected-shaped
 *         returns, and reusing the same "value, never an exception" discipline one layer up keeps the
 *         two libraries' error handling legible together.
 */

#include <cstdint>

namespace sub0tieredcache {

/// Every outcome a call can report. Exhaustion and failure are values, never exceptions: the hot-path
/// calls (try_get, and resolve_into/wait once bytes are Ready) are noexcept (REQUIREMENTS.md R2).
enum class Status : std::uint8_t {
    ok,
    pending,               ///< wait(): a requested row is still filling (only with a deadline/timeout).
    not_resident,          ///< try_get: the row is not an already-ready lease. No I/O was started.
    pool_exhausted,        ///< Every resident slot is pinned/filling; no reclaimable victim (R4, R12).
    batch_too_large,       ///< Request exceeds the registered per-call row bound or its own pool.
    ticket_exhausted,      ///< Prefetch ticket table full, including dropped-but-still-in-flight ones.
    out_of_range,          ///< row_index >= row_count, or the adapter returned an out-of-bounds extent.
    empty_range,
    invalid_argument,      ///< Registration parameters are inconsistent, or an adapter misbehaved.
    busy,                  ///< Destination/source range still claimed by another live transfer.
    short_read,            ///< Underlying transport completed with fewer bytes than requested.
    io_error,
    cancelled,
    timeout,               ///< Deadline passed; the fill and its slot are STILL live.
    codec_failed,          ///< A registered codec reported failure; nothing was published (R6, R14).
    unsupported_conversion,///< Registration requested a source/output pairing no codec can perform (R6).
    declined,               ///< Speculative admission not granted (reserved; T0 has no speculative class).
};

} // namespace sub0tieredcache
