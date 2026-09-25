/** @file remote_mirror_tests.cpp
 *  @brief Mirror: the full miss -> fetch -> publish -> hit path, restart-over-same-directory reuse,
 *         corrupted-entry refetch, unaligned/tail-chunk byte-exactness against a direct slice of the
 *         known source, and same-chunk concurrent-fetch coalescing (AGENTS.md #10: "a concurrency
 *         claim needs an actual concurrent test").
 */

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "sub0tieredcache/remote/http_client.hpp"
#include "sub0tieredcache/remote/mirror.hpp"
#include "support/test_http_server.hpp"
#include "test_support.hpp"

using namespace sub0tieredcache::remote;
using namespace sub0tieredcache::test;

namespace {

std::vector<std::byte> make_content(std::size_t size) {
    std::vector<std::byte> content(size);
    for (std::size_t i = 0; i < size; ++i) {
        content[i] = static_cast<std::byte>((i * 53 + 5) % 241);
    }
    return content;
}

std::filesystem::path make_temp_dir(const char* label) {
    static std::atomic<unsigned> counter{0};
    auto dir = std::filesystem::temp_directory_path() /
               ("sub0tieredcache-mirror-" + std::string(label) + "-" + std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(dir);
    return dir;
}

Http1RangeTransport::Options fast_transport_options(std::string url) {
    Http1RangeTransport::Options options;
    options.url = std::move(url);
    options.connect_timeout = std::chrono::milliseconds(500);
    options.recv_timeout = std::chrono::milliseconds(500);
    options.max_retries = 3;
    options.retry_backoff_base = std::chrono::milliseconds(2);
    return options;
}

Mirror::Options make_mirror_options(sub0tieredcache::test::TestRangeServer& server, Http1RangeTransport& transport,
                                     std::filesystem::path dir, std::uint32_t chunk_size, std::string etag) {
    SourceIdentity source;
    source.url = server.url();
    source.validator.etag = std::move(etag);
    source.validator.total_size = server.total_size();
    return Mirror::Options{
        .source = std::move(source),
        .chunk_size = chunk_size,
        .directory = std::move(dir),
        .transport = RangeTransportRef(transport),
    };
}

void test_miss_fetch_publish_hit() {
    auto content = make_content(1000);
    sub0tieredcache::test::TestRangeServer server(content, "\"v1\"");
    Http1RangeTransport transport(fast_transport_options(server.url()));
    auto dir = make_temp_dir("miss-hit");
    Mirror mirror(make_mirror_options(server, transport, dir, 256, "\"v1\""));
    check(mirror.valid(), "mirror opens over a fresh directory");

    std::vector<std::byte> dest(100);
    auto outcome = mirror.read(10, 100, dest);
    check(outcome.status == Status::ok, "first read (miss) succeeds via fetch");
    check(outcome.bytes == 100, "first read returns the full requested length");
    std::vector<std::byte> expected(content.begin() + 10, content.begin() + 110);
    check(dest == expected, "fetched bytes match the source exactly");

    const auto stats_after_first = mirror.stats();
    check(stats_after_first.fetches == 1, "one chunk covers [10,110) -> exactly one fetch");
    check(stats_after_first.misses == 1, "first read recorded one local miss");

    const auto requests_before = server.request_count();
    std::vector<std::byte> dest2(100);
    auto outcome2 = mirror.read(10, 100, dest2);
    check(outcome2.status == Status::ok, "second read (same chunk) succeeds");
    check(dest2 == expected, "second read returns the same bytes");
    check(server.request_count() == requests_before, "second read is a pure local hit -- zero further network");
    const auto stats_after_second = mirror.stats();
    check(stats_after_second.hits == 1, "second read recorded one local hit");
    check(stats_after_second.fetches == 1, "second read triggered no new fetch");
}

void test_restart_serves_from_disk() {
    auto content = make_content(600);
    sub0tieredcache::test::TestRangeServer server(content, "\"v1\"");
    auto dir = make_temp_dir("restart");
    {
        Http1RangeTransport transport(fast_transport_options(server.url()));
        Mirror mirror(make_mirror_options(server, transport, dir, 128, "\"v1\""));
        std::vector<std::byte> dest(128);
        auto outcome = mirror.read(0, 128, dest);
        check(outcome.status == Status::ok, "first process warms the mirror");
    }
    {
        // A second Mirror over the same directory must never touch the network for what's already there.
        sub0tieredcache::test::TestRangeServer server_should_not_be_hit(content, "\"v1\"");
        Http1RangeTransport unused_transport(fast_transport_options(server_should_not_be_hit.url()));
        Mirror mirror(make_mirror_options(server, unused_transport, dir, 128, "\"v1\""));
        std::vector<std::byte> dest(128);
        auto outcome = mirror.read(0, 128, dest);
        check(outcome.status == Status::ok, "a fresh Mirror over the same directory serves the hit");
        std::vector<std::byte> expected(content.begin(), content.begin() + 128);
        check(dest == expected, "restart preserves the exact bytes");
        check(mirror.stats().hits == 1 && mirror.stats().fetches == 0, "restart never re-fetches an already-mirrored chunk");
    }
}

void test_corrupted_entry_refetched() {
    auto content = make_content(500);
    sub0tieredcache::test::TestRangeServer server(content, "\"v1\"");
    Http1RangeTransport transport(fast_transport_options(server.url()));
    auto dir = make_temp_dir("corrupt-refetch");
    Mirror mirror(make_mirror_options(server, transport, dir, 128, "\"v1\""));

    std::vector<std::byte> dest(64);
    check(mirror.read(0, 64, dest).status == Status::ok, "initial read warms chunk 0");

    // Corrupt the on-disk chunk file directly (flip its last byte -- inside the payload).
    bool flipped = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        std::fstream f(entry.path(), std::ios::in | std::ios::out | std::ios::binary);
        f.seekg(0, std::ios::end);
        const std::streamoff size = f.tellg();
        f.seekg(size - 1);
        char byte = 0;
        f.read(&byte, 1);
        byte = static_cast<char>(~byte);
        f.seekp(size - 1);
        f.write(&byte, 1);
        flipped = true;
    }
    check(flipped, "found the on-disk chunk file to corrupt");

    const auto requests_before = server.request_count();
    std::vector<std::byte> dest2(64);
    auto outcome = mirror.read(0, 64, dest2);
    check(outcome.status == Status::ok, "a corrupted local chunk is refetched, not surfaced as a read failure");
    std::vector<std::byte> expected(content.begin(), content.begin() + 64);
    check(dest2 == expected, "the refetched bytes are correct");
    check(server.request_count() == requests_before + 1, "corruption triggers exactly one refetch");
    check(mirror.stats().corrupt_entries_skipped == 1, "the corrupt entry is counted, never silently ignored");
}

void test_stale_version_chunk_not_served() {
    auto content_v1 = make_content(300);
    sub0tieredcache::test::TestRangeServer server(content_v1, "\"v1\"");
    auto dir = make_temp_dir("stale-version");
    {
        Http1RangeTransport transport(fast_transport_options(server.url()));
        Mirror mirror(make_mirror_options(server, transport, dir, 128, "\"v1\""));
        std::vector<std::byte> dest(128);
        check(mirror.read(0, 128, dest).status == Status::ok, "warm the mirror under v1");
    }
    // A new Mirror pointed at the same URL and directory but a *different* registered ETag (v2) must
    // not serve v1's cached bytes -- it has to refetch under its own validator.
    server.set_etag("\"v2\"");
    Http1RangeTransport transport(fast_transport_options(server.url()));
    Mirror mirror(make_mirror_options(server, transport, dir, 128, "\"v2\""));
    std::vector<std::byte> dest(128);
    auto outcome = mirror.read(0, 128, dest);
    check(outcome.status == Status::ok, "v2 mirror fetches fresh instead of reading v1's stale chunk");
    check(mirror.stats().fetches == 1, "v2 mirror did not treat v1's on-disk chunk as a hit");
}

void test_unaligned_range_crossing_chunks_and_tail() {
    // 10 chunks of 64 bytes each, total 640 -- deliberately not a multiple of chunk_size at a range
    // that starts mid-chunk and ends mid-chunk two chunks later, plus a separate tail-at-EOF case.
    auto content = make_content(640);
    sub0tieredcache::test::TestRangeServer server(content, "\"v1\"");
    Http1RangeTransport transport(fast_transport_options(server.url()));
    auto dir = make_temp_dir("unaligned");
    Mirror mirror(make_mirror_options(server, transport, dir, 64, "\"v1\""));

    // Unaligned range crossing three chunk boundaries.
    std::vector<std::byte> dest(150);
    auto outcome = mirror.read(40, 150, dest); // chunk 0 [0,64), 1 [64,128), 2 [128,192)
    check(outcome.status == Status::ok, "unaligned cross-chunk read succeeds");
    std::vector<std::byte> expected(content.begin() + 40, content.begin() + 190);
    check(dest == expected, "unaligned cross-chunk read is byte-exact against a direct slice");

    // Tail chunk at EOF: 640 % 64 == 0 here, so make a source whose last chunk is genuinely short.
    auto content2 = make_content(600); // 9 full 64-byte chunks + a 24-byte tail chunk
    sub0tieredcache::test::TestRangeServer server2(content2, "\"v1\"");
    Http1RangeTransport transport2(fast_transport_options(server2.url()));
    auto dir2 = make_temp_dir("tail-chunk");
    Mirror mirror2(make_mirror_options(server2, transport2, dir2, 64, "\"v1\""));
    std::vector<std::byte> tail_dest(24);
    auto tail_outcome = mirror2.read(576, 24, tail_dest); // exactly the short tail chunk, [576,600)
    check(tail_outcome.status == Status::ok, "reading the short tail chunk at EOF succeeds");
    std::vector<std::byte> tail_expected(content2.begin() + 576, content2.end());
    check(tail_dest == tail_expected, "the tail chunk is byte-exact, including its shorter length");

    // Reading past EOF is rejected, not silently clamped.
    std::vector<std::byte> oob_dest(10);
    auto oob_outcome = mirror2.read(595, 10, oob_dest);
    check(oob_outcome.status == Status::out_of_range, "a read extending past the registered total size is rejected");
}

void test_concurrent_same_chunk_coalesces_to_one_fetch() {
    auto content = make_content(4096);
    sub0tieredcache::test::TestRangeServer server(content, "\"v1\"");
    Http1RangeTransport transport(fast_transport_options(server.url()));
    auto dir = make_temp_dir("concurrent");
    Mirror mirror(make_mirror_options(server, transport, dir, 4096, "\"v1\"")); // one chunk covers everything

    constexpr int kThreads = 16;
    std::vector<std::thread> threads;
    std::atomic<int> ok_count{0};
    std::atomic<int> mismatch_count{0};
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            std::vector<std::byte> dest(200);
            auto outcome = mirror.read(1000, 200, dest);
            if (outcome.status == Status::ok) {
                ok_count.fetch_add(1);
                std::vector<std::byte> expected(content.begin() + 1000, content.begin() + 1200);
                if (dest != expected) {
                    mismatch_count.fetch_add(1);
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    check(ok_count.load() == kThreads, "every concurrent reader of the same chunk succeeds");
    check(mismatch_count.load() == 0, "every concurrent reader sees byte-exact data");
    const auto stats = mirror.stats();
    check(stats.fetches == 1, "16 concurrent readers of one uncached chunk produce exactly one network fetch");
    // Every reader but the fetcher either coalesces onto the in-flight fetch or, if it happens to
    // check the local store just after the fetcher has already published, gets a direct hit --
    // both are correct outcomes of "no second fetch," so the robust assertion is on their sum, not
    // on coalesced alone (which depends on exact thread-scheduling timing).
    check(stats.coalesced + stats.hits == static_cast<std::uint64_t>(kThreads - 1),
          "every reader but the fetcher itself either coalesced or hit the just-published chunk");
    check(server.request_count() == 1, "the fixture server itself also saw exactly one request");
}

} // namespace

int main() {
    run(test_miss_fetch_publish_hit, "test_miss_fetch_publish_hit");
    run(test_restart_serves_from_disk, "test_restart_serves_from_disk");
    run(test_corrupted_entry_refetched, "test_corrupted_entry_refetched");
    run(test_stale_version_chunk_not_served, "test_stale_version_chunk_not_served");
    run(test_unaligned_range_crossing_chunks_and_tail, "test_unaligned_range_crossing_chunks_and_tail");
    run(test_concurrent_same_chunk_coalesces_to_one_fetch, "test_concurrent_same_chunk_coalesces_to_one_fetch");
    return finish();
}
