#pragma once

/** @file checksum.hpp
 *  @brief Non-cryptographic hashing used for local-cache corruption detection and short source keys.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sub0tieredcache::remote {

// FNV-1a: public-domain, non-cryptographic hash by Fowler/Noll/Vo
// (http://www.isthe.com/chongo/tech/comp/fnv/). Used here only for local corruption detection (a
// flipped byte in a cached chunk file) and for deriving a short, fixed-width key from a source's URL
// string -- never for anything security-sensitive. Chosen over CRC-32 because it needs no lookup
// table and is trivial to keep byte-for-byte identical across platforms without vendoring a library
// (STYLE_GUIDE.md: no third-party dependencies in the header-only core).
[[nodiscard]] constexpr std::uint32_t fnv1a32(std::span<const std::byte> data) noexcept {
    std::uint32_t hash = 0x811c9dc5u;
    for (std::byte b : data) {
        hash ^= static_cast<std::uint32_t>(b);
        hash *= 0x01000193u;
    }
    return hash;
}

[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view data) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (unsigned char c : data) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

} // namespace sub0tieredcache::remote
