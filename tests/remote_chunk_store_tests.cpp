/** @file remote_chunk_store_tests.cpp
 *  @brief ChunkStore: publish/read round-trip, restart-over-same-directory, corruption detection,
 *         stale-version rejection, and stale temp-file cleanup on open (AGENTS.md #4).
 */

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "sub0tieredcache/remote/chunk_store.hpp"
#include "test_support.hpp"

using namespace sub0tieredcache::remote;
using namespace sub0tieredcache::test;

namespace {

std::filesystem::path make_temp_dir(const char* label) {
    static std::atomic<unsigned> counter{0};
    auto dir = std::filesystem::temp_directory_path() /
               ("sub0tieredcache-chunkstore-" + std::string(label) + "-" + std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(dir);
    return dir;
}

std::vector<std::byte> pattern(std::size_t size, std::byte seed) {
    std::vector<std::byte> v(size);
    for (std::size_t i = 0; i < size; ++i) {
        v[i] = static_cast<std::byte>((static_cast<unsigned>(seed) + i * 7) % 256);
    }
    return v;
}

SourceIdentity make_source(std::string url, std::string etag, std::uint64_t total_size) {
    SourceIdentity id;
    id.url = std::move(url);
    id.validator.etag = std::move(etag);
    id.validator.total_size = total_size;
    return id;
}

void test_publish_then_read_hit() {
    auto dir = make_temp_dir("publish-read");
    ChunkStore store(ChunkStore::Options{.directory = dir, .chunk_size = 256});
    check(store.valid(), "store opens over a fresh directory");

    auto source = make_source("http://example/data", "\"v1\"", 1000);
    auto payload = pattern(256, std::byte{1});
    check(store.publish_chunk(source, 0, payload) == Status::ok, "publish succeeds");

    std::vector<std::byte> dest(256);
    auto result = store.read_chunk(source, 0, dest);
    check(result.status == Status::ok, "read after publish is a hit");
    check(result.bytes == 256, "read returns the full payload length");
    check(dest == payload, "read bytes match exactly what was published");
}

void test_restart_serves_from_disk() {
    auto dir = make_temp_dir("restart");
    auto source = make_source("http://example/data", "\"v1\"", 1000);
    auto payload = pattern(128, std::byte{7});
    {
        ChunkStore store(ChunkStore::Options{.directory = dir, .chunk_size = 256});
        check(store.publish_chunk(source, 2, payload) == Status::ok, "first process publishes chunk 2");
    }
    {
        ChunkStore store(ChunkStore::Options{.directory = dir, .chunk_size = 256}); // new object, same dir
        std::vector<std::byte> dest(256);
        auto result = store.read_chunk(source, 2, dest);
        check(result.status == Status::ok, "a fresh ChunkStore over the same directory serves the hit");
        check(result.bytes == 128, "restart preserves the exact stored payload length");
        std::vector<std::byte> expected(dest.begin(), dest.begin() + 128);
        check(expected == payload, "restart preserves the exact stored bytes");
    }
}

void test_corrupted_chunk_detected() {
    auto dir = make_temp_dir("corrupt");
    ChunkStore store(ChunkStore::Options{.directory = dir, .chunk_size = 64});
    auto source = make_source("http://example/data", "\"v1\"", 1000);
    auto payload = pattern(64, std::byte{3});
    check(store.publish_chunk(source, 0, payload) == Status::ok, "publish before corrupting");

    // Flip one byte inside the published file, well past the header, to corrupt the payload only.
    bool flipped = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        std::fstream f(entry.path(), std::ios::in | std::ios::out | std::ios::binary);
        f.seekg(0, std::ios::end);
        const std::streamoff size = f.tellg();
        check(size > 40, "chunk file is large enough to contain header + payload");
        f.seekp(size - 1);
        char byte = 0;
        f.seekg(size - 1);
        f.read(&byte, 1);
        byte = static_cast<char>(~byte);
        f.seekp(size - 1);
        f.write(&byte, 1);
        flipped = true;
    }
    check(flipped, "found the published chunk file to corrupt");

    std::vector<std::byte> dest(64);
    auto result = store.read_chunk(source, 0, dest);
    check(result.status == Status::corrupt, "a flipped byte is detected as corrupt, never served");
}

void test_stale_version_not_served() {
    auto dir = make_temp_dir("stale-version");
    ChunkStore store(ChunkStore::Options{.directory = dir, .chunk_size = 64});
    auto source_v1 = make_source("http://example/data", "\"v1\"", 1000);
    auto source_v2 = make_source("http://example/data", "\"v2\"", 1000); // same URL, different ETag
    auto payload = pattern(64, std::byte{9});
    check(store.publish_chunk(source_v1, 0, payload) == Status::ok, "publish under v1's validator");

    std::vector<std::byte> dest(64);
    auto result = store.read_chunk(source_v2, 0, dest);
    check(result.status == Status::not_found, "a chunk published under a different validator is never served as v2's");

    auto still_v1 = store.read_chunk(source_v1, 0, dest);
    check(still_v1.status == Status::ok, "the original v1 entry is still readable under its own validator");
}

void test_stale_temp_files_cleaned_on_open() {
    auto dir = make_temp_dir("temp-cleanup");
    std::filesystem::create_directories(dir);
    const auto leftover = dir / ".stc1tmp-leftover-from-a-crash";
    {
        std::ofstream f(leftover, std::ios::binary);
        f << "partial garbage";
    }
    check(std::filesystem::exists(leftover), "leftover temp file exists before ChunkStore opens");

    ChunkStore store(ChunkStore::Options{.directory = dir, .chunk_size = 64});
    (void)store;
    check(!std::filesystem::exists(leftover), "opening ChunkStore cleans up the stale temp file");
}

void test_no_final_file_on_publish_failure() {
    // Publishing a payload larger than chunk_size is rejected before any file is touched.
    auto dir = make_temp_dir("publish-failure");
    ChunkStore store(ChunkStore::Options{.directory = dir, .chunk_size = 16});
    auto source = make_source("http://example/data", "\"v1\"", 1000);
    auto oversized = pattern(17, std::byte{2});
    check(store.publish_chunk(source, 0, oversized) == Status::invalid_argument, "oversized payload is rejected");

    std::size_t file_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        (void)entry;
        ++file_count;
    }
    check(file_count == 0, "a rejected publish leaves no file at all, final or temp");
}

} // namespace

int main() {
    run(test_publish_then_read_hit, "test_publish_then_read_hit");
    run(test_restart_serves_from_disk, "test_restart_serves_from_disk");
    run(test_corrupted_chunk_detected, "test_corrupted_chunk_detected");
    run(test_stale_version_not_served, "test_stale_version_not_served");
    run(test_stale_temp_files_cleaned_on_open, "test_stale_temp_files_cleaned_on_open");
    run(test_no_final_file_on_publish_failure, "test_no_final_file_on_publish_failure");
    return finish();
}
