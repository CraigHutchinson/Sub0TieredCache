#pragma once

/** @file chunk_store.hpp
 *  @brief Versioned local chunk mirror (README.md sec 3a / sec 6, AGENTS.md #4: "a second process
 *         reading a cache directory written by an older/incompatible version must be able to tell a
 *         stale entry from a fresh one, not trust file mtimes or assume compatibility").
 *
 *         Fixed-size chunks, keyed by (source identity, chunk index). Every chunk file carries a
 *         self-describing header (magic, format version, source-identity hash, the *exact* validator
 *         bytes -- not a hash of them, so a validator collision can never make a stale chunk look
 *         fresh -- chunk index, payload length, checksum) so a second process/run can detect and skip
 *         a stale or corrupt entry outright (AGENTS.md #4's hard requirement) rather than trusting
 *         anything about when or how the file was written.
 *
 *         Publication is atomic: write to a unique temp file in the same directory, flush (+fsync
 *         where available), then rename over the final name. A cancelled or failed publish therefore
 *         never leaves a partial file at the final name; `ChunkStore`'s constructor best-effort
 *         cleans any leftover temp files from a prior crash on open.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <system_error>

#include "sub0tieredcache/remote/checksum.hpp"
#include "sub0tieredcache/remote/status.hpp"
#include "sub0tieredcache/remote/transport.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h> // _get_osfhandle
#else
#include <unistd.h>
#endif

namespace sub0tieredcache::remote {

/// A source's stable identity for chunk-store keying: a URL/name plus the validator captured at
/// registration (README.md sec 3a: "Disk entries include source identity, generation and
/// representation"). Two `SourceIdentity`s with the same `url` but a different `validator` key
/// different chunk files -- an old-version chunk is never overwritten in place, it is simply never
/// matched by a reader expecting the new validator (and is free to be garbage-collected separately;
/// v1 does not implement GC of superseded entries, see docs/remote-mirror.md).
struct SourceIdentity {
    std::string url;
    SourceValidator validator;
};

namespace detail {

/// fopen that takes the native path (wide on Windows, so non-ASCII paths work) and avoids MSVC's
/// deprecated narrow fopen. `mode` is plain ASCII ("rb"/"wb").
[[nodiscard]] inline std::FILE* open_file(const std::filesystem::path& path, const char* mode) noexcept {
#if defined(_WIN32)
    wchar_t wide_mode[4] = {};
    for (std::size_t i = 0; i < 3 && mode[i] != '\0'; ++i) {
        wide_mode[i] = static_cast<wchar_t>(mode[i]);
    }
    std::FILE* file = nullptr;
    return _wfopen_s(&file, path.c_str(), wide_mode) == 0 ? file : nullptr;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// Chunk file format v1. "STC1" magic distinguishes this library's files from anything else that
// might share a cache directory; format_version lets a v2 reader recognize and reject a file it
// does not understand instead of misinterpreting its bytes (AGENTS.md #4).
inline constexpr std::uint32_t kChunkMagic = 0x31435453u; // "STC1" little-endian bytes 'S','T','C','1'
inline constexpr std::uint32_t kChunkFormatVersion = 1u;

#pragma pack(push, 1)
struct ChunkFileHeader {
    std::uint32_t magic = kChunkMagic;
    std::uint32_t format_version = kChunkFormatVersion;
    std::uint64_t source_id_hash = 0;  ///< fnv1a64(url) -- a directory can hold many sources' chunks.
    std::uint64_t total_size = 0;      ///< Registered total source size; compared exactly on read.
    std::uint32_t chunk_index = 0;
    std::uint32_t payload_length = 0;  ///< Actual stored bytes (< chunk_size only for the tail chunk).
    std::uint32_t validator_length = 0; ///< Bytes of the canonical validator string that follow.
    std::uint32_t payload_checksum = 0; ///< fnv1a32 over the payload bytes only.
};
#pragma pack(pop)
static_assert(sizeof(ChunkFileHeader) == 4 + 4 + 8 + 8 + 4 + 4 + 4 + 4);

/// Canonicalizes a validator into the exact bytes stored in (and compared against) a chunk file.
/// Kept as a plain string, not a hash, precisely so two different validators can never collide into
/// looking like the same one (see file comment).
[[nodiscard]] inline std::string canonical_validator(const SourceValidator& v) {
    std::string s;
    s.reserve(v.etag.size() + v.last_modified.size() + 8);
    s += "E:";
    s += v.etag;
    s += "|L:";
    s += v.last_modified;
    return s;
}

/// The on-disk filename encodes both the source URL's hash and the validator's hash, not just the
/// URL: this is what makes two generations of the same source (README.md sec 3a's "source identity
/// = URL + validator + total size") coexist as two distinct files on disk instead of one
/// overwriting the other, so an old generation's leases/readers keep working until it is explicitly
/// superseded. The header's own exact (non-hashed) validator bytes, checked on every read, are what
/// actually enforce correctness -- this filename hash only needs to avoid *accidental* collisions
/// between generations, not defend against them.
[[nodiscard]] inline std::string chunk_file_name(const SourceIdentity& source, std::uint32_t chunk_index) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%016llx_%016llx_%010u.stc1chunk",
                  static_cast<unsigned long long>(fnv1a64(source.url)),
                  static_cast<unsigned long long>(fnv1a64(canonical_validator(source.validator))), chunk_index);
    return std::string(buf);
}

[[nodiscard]] inline bool fsync_file(std::FILE* f) noexcept {
    std::fflush(f);
#if defined(_WIN32)
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)));
    return h != INVALID_HANDLE_VALUE && FlushFileBuffers(h) != 0;
#else
    return ::fsync(fileno(f)) == 0;
#endif
}

[[nodiscard]] inline bool atomic_rename(const std::filesystem::path& from, const std::filesystem::path& to) noexcept {
#if defined(_WIN32)
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    std::error_code ec;
    std::filesystem::rename(from, to, ec); // POSIX rename() is atomic within one filesystem.
    return !ec;
#endif
}

inline constexpr std::string_view kTempPrefix = ".stc1tmp-";

} // namespace detail

/// The disk-tier chunk mirror. One directory holds chunks for any number of sources; chunk size is
/// fixed per `ChunkStore` instance (README.md sec 3a: "Fixed-size chunks"). Every method here is on
/// the resolve-pass/administrative side (REQUIREMENTS.md R2) -- blocking filesystem I/O is expected.
class ChunkStore {
public:
    struct Options {
        std::filesystem::path directory; ///< Created if missing. Empty -> caller error (invalid_argument).
        std::uint32_t chunk_size = 0;    ///< Bytes per chunk except a possible shorter tail chunk.
    };

    explicit ChunkStore(Options options) : chunk_size_(options.chunk_size), directory_(std::move(options.directory)) {
        if (directory_.empty() || chunk_size_ == 0) {
            valid_ = false;
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(directory_, ec);
        valid_ = !ec || std::filesystem::exists(directory_);
        cleanup_stale_temp_files();
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::uint32_t chunk_size() const noexcept { return chunk_size_; }
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

    struct ReadResult {
        Status status = Status::not_found;
        std::uint32_t bytes = 0; ///< Payload bytes copied into `dest`, valid only when status == ok.
    };

    /// Reads one chunk, fully validating header/identity/validator/checksum before returning any
    /// bytes. A mismatch at any check is reported as `not_found` (identity/validator mismatch -- an
    /// old-generation or different-source file that simply isn't this chunk) or `corrupt` (this *is*
    /// the right file, but its bytes fail the checksum) -- never partially served either way.
    [[nodiscard]] ReadResult read_chunk(const SourceIdentity& source, std::uint32_t chunk_index, std::span<std::byte> dest) const {
        std::scoped_lock lock(mutex_);
        const auto path = directory_ / detail::chunk_file_name(source, chunk_index);
        std::FILE* f = detail::open_file(path, "rb");
        if (f == nullptr) {
            return {.status = Status::not_found};
        }
        struct FileGuard {
            std::FILE* fp;
            ~FileGuard() { std::fclose(fp); }
        } guard{f};

        detail::ChunkFileHeader header{};
        if (std::fread(&header, sizeof(header), 1, f) != 1) {
            return {.status = Status::corrupt};
        }
        if (header.magic != detail::kChunkMagic || header.format_version != detail::kChunkFormatVersion) {
            return {.status = Status::corrupt};
        }
        if (header.source_id_hash != fnv1a64(source.url) || header.total_size != source.validator.total_size ||
            header.chunk_index != chunk_index) {
            return {.status = Status::not_found}; // right filename hash bucket, wrong/old identity
        }
        if (header.payload_length > chunk_size_ || header.payload_length > dest.size()) {
            return {.status = Status::corrupt};
        }
        std::string validator_bytes(header.validator_length, '\0');
        if (header.validator_length > 0 && std::fread(validator_bytes.data(), 1, header.validator_length, f) != header.validator_length) {
            return {.status = Status::corrupt};
        }
        if (validator_bytes != detail::canonical_validator(source.validator)) {
            return {.status = Status::not_found}; // superseded generation: not an error, just stale
        }
        if (header.payload_length > 0 && std::fread(dest.data(), 1, header.payload_length, f) != header.payload_length) {
            return {.status = Status::corrupt};
        }
        const auto checksum = fnv1a32(std::as_bytes(std::span(dest.data(), header.payload_length)));
        if (checksum != header.payload_checksum) {
            return {.status = Status::corrupt};
        }
        return {.status = Status::ok, .bytes = header.payload_length};
    }

    /// Publishes one chunk atomically: write to a unique temp file, flush+fsync, rename over the
    /// final name. `payload.size()` must be `<= chunk_size()` (the tail chunk of a source may be
    /// shorter; every other chunk is expected to be exactly `chunk_size()`, but this is not enforced
    /// here -- Mirror is the layer that knows which chunk is the tail).
    [[nodiscard]] Status publish_chunk(const SourceIdentity& source, std::uint32_t chunk_index, std::span<const std::byte> payload) {
        if (payload.size() > chunk_size_) {
            return Status::invalid_argument;
        }
        std::scoped_lock lock(mutex_);
        const std::string validator_bytes = detail::canonical_validator(source.validator);

        detail::ChunkFileHeader header{};
        header.source_id_hash = fnv1a64(source.url);
        header.total_size = source.validator.total_size;
        header.chunk_index = chunk_index;
        header.payload_length = static_cast<std::uint32_t>(payload.size());
        header.validator_length = static_cast<std::uint32_t>(validator_bytes.size());
        header.payload_checksum = fnv1a32(payload);

        const auto temp_path = directory_ / (std::string(detail::kTempPrefix) + std::to_string(temp_nonce_) + "-" +
                                              std::to_string(temp_counter_++));
        std::FILE* f = detail::open_file(temp_path, "wb");
        if (f == nullptr) {
            return Status::publish_failed;
        }
        bool ok = std::fwrite(&header, sizeof(header), 1, f) == 1;
        ok = ok && (validator_bytes.empty() || std::fwrite(validator_bytes.data(), 1, validator_bytes.size(), f) == validator_bytes.size());
        ok = ok && (payload.empty() || std::fwrite(payload.data(), 1, payload.size(), f) == payload.size());
        ok = ok && detail::fsync_file(f);
        std::fclose(f);
        if (!ok) {
            std::error_code ec;
            std::filesystem::remove(temp_path, ec);
            return Status::publish_failed;
        }

        const auto final_path = directory_ / detail::chunk_file_name(source, chunk_index);
        if (!detail::atomic_rename(temp_path, final_path)) {
            std::error_code ec;
            std::filesystem::remove(temp_path, ec);
            return Status::publish_failed;
        }
        return Status::ok;
    }

private:
    /// A prior process crashing between opening and renaming a temp file would leave it behind
    /// forever otherwise; a fresh temp name is chosen for every publish (counter + this-pointer), so
    /// any file still matching the temp prefix on open is unambiguously a leftover, safe to remove.
    void cleanup_stale_temp_files() const {
        if (!valid_) {
            return;
        }
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
            if (entry.path().filename().string().starts_with(detail::kTempPrefix)) {
                std::error_code remove_ec;
                std::filesystem::remove(entry.path(), remove_ec);
            }
        }
    }

    std::uint32_t chunk_size_;
    std::filesystem::path directory_;
    bool valid_ = true;
    mutable std::mutex mutex_;
    mutable std::uint64_t temp_counter_ = 0;
    /// Random per store, so two processes publishing into one directory never pick the same temp name.
    std::uint64_t temp_nonce_ = (std::uint64_t{std::random_device{}()} << 32) ^ std::random_device{}();
};

} // namespace sub0tieredcache::remote
