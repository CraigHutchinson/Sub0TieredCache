#pragma once

/** @file local_file_source.hpp
 *  @brief T1 convenience: a flat-layout row->location adapter, and a helper that registers files with a
 *         caller-owned sub0mempage::LocalFileBackend, for the trivial "one row-major file, fixed row
 *         stride" shape (README.md sec 3's `local_sharded`, the single-shard case).
 *
 *  The core (row_cache.hpp) never parses any file format (AGENTS.md sec 2) -- this header is exactly
 *  the caller-side adapter code that boundary asks for: pure row_index -> (shard, byte offset) address
 *  arithmetic, nothing about what the bytes at that offset mean. A caller whose rows are NOT a flat
 *  row-major array at a fixed stride (e.g. a real external shard format) writes its own resolver against
 *  RowExtentResolverRef the same way; this header covers only the common flat-file case explicitly, per
 *  the T1 task brief.
 */

#include "row_cache.hpp"

#include <sub0mempage/local_file_backend.hpp>

#include <cstdint>
#include <filesystem>

namespace sub0tieredcache {

/** @brief row_index -> RowLocation for one or more row-major flat files sharing the same stride.
 *
 *  Row `i` in shard `shard_of(i)` starts at `base_offset + local_row_index(i) * row_stride` and is
 *  `row_width` bytes long (`row_width <= row_stride` -- a stride wider than the row itself is a padded
 *  layout, e.g. row-aligned to a cache line; the gap is simply never read). `rows_per_shard` rows are
 *  addressed per registered shard in order (shard 0 first): row_index / rows_per_shard picks the shard,
 *  row_index % rows_per_shard the row within it. A single-file table passes `rows_per_shard =
 *  row_count` (or any value >= row_count) so every row resolves to shard 0.
 */
class FlatFileResolver {
public:
    constexpr FlatFileResolver(std::uint64_t row_count, std::uint64_t rows_per_shard, std::uint64_t row_stride,
                                std::uint64_t row_width, std::uint64_t base_offset = 0) noexcept
        : row_count_(row_count), rows_per_shard_(rows_per_shard), row_stride_(row_stride),
          row_width_(row_width), base_offset_(base_offset) {}

    [[nodiscard]] std::expected<RowLocation, Status> resolve_extent(std::uint64_t row_index) const noexcept {
        if (row_index >= row_count_ || rows_per_shard_ == 0) {
            return std::unexpected(Status::out_of_range);
        }
        const std::uint64_t shard = row_index / rows_per_shard_;
        const std::uint64_t local_row = row_index % rows_per_shard_;
        if (shard > UINT32_MAX) {
            return std::unexpected(Status::out_of_range);
        }
        return RowLocation{static_cast<std::uint32_t>(shard),
                            sub0mempage::ByteRange{base_offset_ + local_row * row_stride_, row_width_}};
    }

private:
    std::uint64_t row_count_;
    std::uint64_t rows_per_shard_;
    std::uint64_t row_stride_;
    std::uint64_t row_width_;
    std::uint64_t base_offset_;
};

/** @brief Administrative: registers `path` under `source` with `backend`, then returns a ShardSource
 *  describing it. `backend` is caller-owned and must outlive the Table it serves (matching
 *  sub0mempage::LocalFileBackend's own contract) -- this helper does not take ownership of anything.
 *  May block and allocate (register_file() itself does); never called on a hot path.
 */
[[nodiscard]] inline std::expected<ShardSource, Status>
register_local_file_shard(sub0mempage::LocalFileBackend& backend, sub0mempage::SourceId source,
                           const std::filesystem::path& path, std::uint64_t bytes) {
    const sub0mempage::Status status = backend.register_file(source, path);
    if (status != sub0mempage::Status::ok) {
        switch (status) {
        case sub0mempage::Status::invalid_argument: return std::unexpected(Status::invalid_argument);
        case sub0mempage::Status::ticket_exhausted: return std::unexpected(Status::ticket_exhausted);
        default: return std::unexpected(Status::io_error);
        }
    }
    return ShardSource{source, bytes};
}

/// Same as the four-argument overload, but reads the file's own size from the filesystem instead of
/// requiring the caller to already know it (a convenience for tests/tools; a caller that already knows
/// its row_count * row_stride extent from elsewhere should prefer passing it explicitly, since a file
/// larger than expected here would otherwise silently widen the registered extent).
[[nodiscard]] inline std::expected<ShardSource, Status>
register_local_file_shard(sub0mempage::LocalFileBackend& backend, sub0mempage::SourceId source,
                           const std::filesystem::path& path) {
    std::error_code error_code;
    const auto size = std::filesystem::file_size(path, error_code);
    if (error_code) {
        return std::unexpected(Status::io_error);
    }
    return register_local_file_shard(backend, source, path, static_cast<std::uint64_t>(size));
}

} // namespace sub0tieredcache
