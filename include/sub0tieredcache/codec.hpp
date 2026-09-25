#pragma once

/** @file codec.hpp
 *  @brief The registered-codec interface (R6) and the one built-in non-identity conversion (bf16 -> f32
 *         widening). "Generic scalar codecs may be built in; model-specific quantization/layout kernels
 *         are supplied by the consumer, never implemented by the format-agnostic core" (REQUIREMENTS.md
 *         R6) -- bf16->f32 is the one generic, dtype-only conversion T0 ships; anything else (a model's
 *         own quantization scheme) is a caller-registered Codec, never a case added here.
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace sub0tieredcache {

/** @brief A caller- or built-in-registered representation converter.
 *
 *  "A codec produces bytes only into an exclusively reserved output and reports failure or completion.
 *  No codec may retain an unowned source pointer." (docs/integration-plan.md "Output and cache
 *  contract"). `convert` must not retain `source` or `destination` past the call, must write only
 *  within `destination`, and must not throw (called from a noexcept fill-completion path).
 */
class Codec {
public:
    Codec() = default;
    Codec(const Codec&) = delete;
    Codec& operator=(const Codec&) = delete;
    virtual ~Codec() = default;

    /// `source` is the just-fetched encoded row (exactly the registered source_row_bytes); `destination`
    /// is the row's exclusive output reservation (exactly the registered output_row_bytes). Returns
    /// false on failure -- the caller publishes nothing when this returns false (R14).
    [[nodiscard]] virtual bool convert(std::span<const std::byte> source,
                                        std::span<std::byte> destination) noexcept = 0;
};

namespace detail {

/// Bit-exact bf16 -> f32 widening: a bf16 value's bit pattern IS an f32's high 16 bits, so widening is a
/// zero-extending 16-bit left shift, not an arithmetic conversion -- this preserves NaN/Inf/denormal/-0
/// patterns exactly (they are not floating-point *values* being rounded, just bits being repositioned).
/// Cited fact, not a guess: bf16 (Google Brain float) is defined exactly as an f32's sign+exponent+top-7
/// mantissa bits (a truncated f32), which is the entire reason this widening is bit-exact and the
/// opposite narrowing direction is not (docs/prior-art.md does not yet cite the bf16 spec itself since
/// this is a format-bit-layout fact, not a policy choice -- verified against IEEE 754 f32 layout and
/// the widely-documented bf16-as-truncated-f32 layout, e.g. used unchanged by every ML framework's own
/// bf16<->f32 cast).
[[nodiscard]] constexpr std::uint32_t bf16_bits_to_f32_bits(std::uint16_t bits) noexcept {
    return static_cast<std::uint32_t>(bits) << 16;
}

} // namespace detail

/// Built-in generic scalar conversion (R6): widens a row of N bf16 elements to N f32 elements,
/// bit-exact per element (see detail::bf16_bits_to_f32_bits). `source.size()` must be even (a whole
/// number of 2-byte elements) and `destination.size()` must be exactly twice `source.size()`; this is
/// checked once at registration (Table::create), not per call, so convert() itself only asserts widths
/// match what registration already proved consistent.
class Bf16ToF32Codec final : public Codec {
public:
    [[nodiscard]] bool convert(std::span<const std::byte> source, std::span<std::byte> destination) noexcept override {
        if (destination.size() != source.size() * 2 || source.size() % 2 != 0) {
            return false; // defensive; Table::create already enforces this width relationship at registration
        }
        const std::size_t elements = source.size() / 2;
        for (std::size_t i = 0; i < elements; ++i) {
            std::uint16_t bf16_bits;
            std::byte src_bytes[2] = {source[2 * i], source[2 * i + 1]};
            std::memcpy(&bf16_bits, src_bytes, sizeof(bf16_bits));
            const std::uint32_t f32_bits = detail::bf16_bits_to_f32_bits(bf16_bits);
            std::memcpy(destination.data() + 4 * i, &f32_bits, sizeof(f32_bits));
        }
        return true;
    }
};

} // namespace sub0tieredcache
