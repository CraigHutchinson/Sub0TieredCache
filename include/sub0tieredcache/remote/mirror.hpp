#pragma once

/** @file mirror.hpp
 *  @brief Combines the transport seam (transport.hpp) with the versioned local chunk store
 *         (chunk_store.hpp) into the one call a caller actually wants: `read(offset, length, dest)`.
 *         Blocking/administrative-path only (REQUIREMENTS.md R2) -- Mirror is meant to be driven from
 *         a resolve pass, never from `try_get`'s hot path. AGENTS.md #9/#10: stats are first-class,
 *         and same-chunk concurrent fetches genuinely coalesce (one network fetch), not just claimed.
 */

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "sub0tieredcache/remote/cache_dir.hpp"
#include "sub0tieredcache/remote/chunk_store.hpp"
#include "sub0tieredcache/remote/status.hpp"
#include "sub0tieredcache/remote/transport.hpp"

namespace sub0tieredcache::remote {

class Mirror {
public:
    struct Options {
        SourceIdentity source;        ///< url + validator (etag/last_modified) + total_size, all
                                       ///< captured up front -- README.md sec 3a: identity is fixed
                                       ///< for the life of one Mirror instance, never inferred later.
        std::uint32_t chunk_size = 0;
        std::filesystem::path directory; ///< Empty -> default_user_cache_dir()'s per-source default.
        RangeTransportRef transport;
    };

    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t fetches = 0;
        std::uint64_t bytes_fetched = 0;
        std::uint64_t retries = 0;
        std::uint64_t validator_mismatches = 0;
        std::uint64_t corrupt_entries_skipped = 0;
        std::uint64_t publish_failures = 0;
        std::uint64_t coalesced = 0;
    };

    explicit Mirror(Options options)
        : source_(std::move(options.source)),
          transport_(options.transport),
          store_(ChunkStore::Options{
              .directory = options.directory.empty() ? default_user_cache_dir() / sanitize_for_path(source_.url) : options.directory,
              .chunk_size = options.chunk_size,
          }) {}

    [[nodiscard]] bool valid() const noexcept { return store_.valid() && store_.chunk_size() > 0; }

    struct ReadOutcome {
        Status status = Status::ok;
        std::uint64_t bytes = 0;
    };

    /// Reads `length` bytes starting at `offset`, covering as many chunks as needed, each validated
    /// locally or fetched-and-published before its relevant sub-range is copied into `dest`. Byte-
    /// exact against a direct slice of the source for both unaligned ranges crossing chunk
    /// boundaries and the tail chunk at EOF (tests/remote_mirror_tests.cpp exercises both).
    [[nodiscard]] ReadOutcome read(std::uint64_t offset, std::uint64_t length, std::span<std::byte> dest) {
        if (!valid() || dest.size() < length) {
            return {.status = Status::invalid_argument};
        }
        if (length == 0) {
            return {.status = Status::invalid_argument};
        }
        const std::uint64_t total = source_.validator.total_size;
        if (offset > total || length > total - offset) {
            return {.status = Status::out_of_range};
        }

        const std::uint32_t chunk_size = store_.chunk_size();
        const std::uint32_t first_chunk = static_cast<std::uint32_t>(offset / chunk_size);
        const std::uint32_t last_chunk = static_cast<std::uint32_t>((offset + length - 1) / chunk_size);

        std::vector<std::byte> chunk_buffer(chunk_size);
        std::uint64_t written = 0;
        for (std::uint32_t chunk_index = first_chunk; chunk_index <= last_chunk; ++chunk_index) {
            const std::uint64_t chunk_start = static_cast<std::uint64_t>(chunk_index) * chunk_size;
            const std::uint64_t chunk_len = std::min<std::uint64_t>(chunk_size, total - chunk_start);

            const Status status = ensure_chunk_local(chunk_index, chunk_len, chunk_buffer);
            if (status != Status::ok) {
                return {.status = status, .bytes = written};
            }

            const std::uint64_t range_start = std::max(offset, chunk_start);
            const std::uint64_t range_end = std::min(offset + length, chunk_start + chunk_len);
            if (range_end > range_start) {
                const std::uint64_t copy_len = range_end - range_start;
                const std::uint64_t src_off = range_start - chunk_start;
                const std::uint64_t dst_off = range_start - offset;
                std::memcpy(dest.data() + dst_off, chunk_buffer.data() + src_off, copy_len);
                written += copy_len;
            }
        }
        return {.status = Status::ok, .bytes = written};
    }

    [[nodiscard]] Stats stats() const {
        std::scoped_lock lock(stats_mutex_);
        return stats_;
    }

private:
    /// One chunk's coalesced-fetch state: every concurrent caller after the first waits on this
    /// instead of issuing a second network fetch (README REQUIREMENTS.md R5's same coalescing
    /// discipline, applied to the remote mirror's own fetch path rather than the row cache's).
    struct InFlight {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        Status status = Status::ok;
    };

    /// Ensures the given chunk is present and valid in the local store, fetching (with coalescing)
    /// only if needed, then reads it into `chunk_buffer`. On success `chunk_buffer[0, chunk_len)` is
    /// valid payload.
    [[nodiscard]] Status ensure_chunk_local(std::uint32_t chunk_index, std::uint64_t chunk_len, std::vector<std::byte>& chunk_buffer) {
        {
            auto local = store_.read_chunk(source_, chunk_index, chunk_buffer);
            if (local.status == Status::ok) {
                record_hit();
                return Status::ok;
            }
            if (local.status == Status::corrupt) {
                record_corrupt();
            } else {
                record_miss();
            }
        }

        std::shared_ptr<InFlight> state;
        bool am_fetcher = false;
        {
            std::scoped_lock lock(inflight_mutex_);
            auto it = inflight_.find(chunk_index);
            if (it != inflight_.end()) {
                state = it->second;
                record_coalesced();
            } else {
                state = std::make_shared<InFlight>();
                inflight_.emplace(chunk_index, state);
                am_fetcher = true;
            }
        }

        if (am_fetcher) {
            const Status fetch_status = fetch_and_publish(chunk_index, chunk_len);
            {
                std::scoped_lock lock(state->m);
                state->status = fetch_status;
                state->done = true;
            }
            state->cv.notify_all();
            {
                std::scoped_lock lock(inflight_mutex_);
                inflight_.erase(chunk_index);
            }
        } else {
            std::unique_lock lock(state->m);
            state->cv.wait(lock, [&] { return state->done; });
        }

        if (state->status != Status::ok) {
            return state->status;
        }
        // The fetcher (or the thread we coalesced onto) published successfully; read it back so this
        // caller gets its own validated copy rather than trusting another thread's buffer directly.
        auto local = store_.read_chunk(source_, chunk_index, chunk_buffer);
        if (local.status != Status::ok) {
            return local.status == Status::corrupt ? Status::corrupt : Status::not_found;
        }
        return Status::ok;
    }

    [[nodiscard]] Status fetch_and_publish(std::uint32_t chunk_index, std::uint64_t chunk_len) {
        const std::uint64_t chunk_start = static_cast<std::uint64_t>(chunk_index) * store_.chunk_size();
        std::vector<std::byte> fetch_buffer(chunk_len);

        RangeFetchRequest request;
        request.offset = chunk_start;
        request.length = chunk_len;
        request.destination = fetch_buffer;
        request.expected = source_.validator;

        const FetchRangeResult result = transport_.fetch_range(request);
        {
            std::scoped_lock lock(stats_mutex_);
            stats_.fetches += 1;
            stats_.retries += result.retries;
            if (result.status == Status::ok) {
                stats_.bytes_fetched += result.bytes;
            } else if (result.status == Status::source_changed) {
                stats_.validator_mismatches += 1;
            }
        }
        if (result.status != Status::ok) {
            return result.status;
        }
        if (result.bytes != chunk_len) {
            return Status::truncated;
        }

        const Status publish_status = store_.publish_chunk(source_, chunk_index, fetch_buffer);
        if (publish_status != Status::ok) {
            std::scoped_lock lock(stats_mutex_);
            stats_.publish_failures += 1;
        }
        return publish_status;
    }

    void record_hit() {
        std::scoped_lock lock(stats_mutex_);
        stats_.hits += 1;
    }
    void record_miss() {
        std::scoped_lock lock(stats_mutex_);
        stats_.misses += 1;
    }
    void record_corrupt() {
        std::scoped_lock lock(stats_mutex_);
        stats_.misses += 1;
        stats_.corrupt_entries_skipped += 1;
    }
    void record_coalesced() {
        std::scoped_lock lock(stats_mutex_);
        stats_.coalesced += 1;
    }

    [[nodiscard]] static std::string sanitize_for_path(const std::string& url) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(fnv1a64(url)));
        return std::string(buf);
    }

    SourceIdentity source_;
    RangeTransportRef transport_;
    ChunkStore store_;

    std::mutex inflight_mutex_;
    std::unordered_map<std::uint32_t, std::shared_ptr<InFlight>> inflight_;

    mutable std::mutex stats_mutex_;
    Stats stats_;
};

} // namespace sub0tieredcache::remote
