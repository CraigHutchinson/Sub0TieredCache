// R5: "Multiple threads calling prefetch/resolve_into concurrently for the same row must trigger one
// live fetch/conversion per source-generation, row, representation and destination domain." AGENTS.md
// sec 10: "A concurrency claim needs an actual concurrent test" -- this drives a real multithreaded
// repeated-row workload and asserts the resulting fetch count, not just that the answer is correct.

#include <sub0mempage/testing/fake_backend.hpp>
#include "test_support.hpp"

#include <sub0tieredcache/sub0tieredcache.hpp>

#include <array>
#include <atomic>
#include <random>
#include <thread>
#include <vector>

namespace {

using namespace sub0tieredcache;
using sub0mempage::ByteRange;
using sub0mempage::FillBackendRef;
using sub0mempage::SourceId;
using sub0tieredcache::test::BackgroundCompleter;
using sub0tieredcache::test::check;
using sub0tieredcache::test::finish;
using sub0tieredcache::test::run;
using Backend = sub0mempage::test::FakeBackend;

struct FixedWidthResolver {
    std::uint64_t row_bytes;
    std::uint64_t row_count;
    [[nodiscard]] std::expected<RowLocation, Status> resolve_extent(std::uint64_t row) const noexcept {
        if (row >= row_count) {
            return std::unexpected(Status::out_of_range);
        }
        return RowLocation{0, ByteRange{row * row_bytes, row_bytes}};
    }
};

/// docs/reference-consumer-sub0llm.md cites ~24 concurrent callers on the reference machine; this uses
/// a comparable order of magnitude, all hammering the SAME row, so the only way for R5 to hold is
/// exactly one live fetch total.
void test_concurrent_same_row_coalesces() {
    constexpr int thread_count = 24;
    constexpr int requests_per_thread = 50;

    Backend backend(64);
    const std::uint64_t row_bytes = 16;
    const std::uint64_t row_count = 4;
    FixedWidthResolver resolver{row_bytes, row_count};
    std::vector<std::byte> output(row_bytes * 2); // budget 2 rows: room for the hot row plus slack
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    const auto cfg_sources = single_source(SourceId{7}, row_count * row_bytes);
    cfg.sources = cfg_sources;

    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = 2;
    cfg.max_tickets = static_cast<std::uint32_t>(thread_count);
    cfg.max_batch_rows = 1;
    auto table = std::move(*Table::create(cfg, FillBackendRef(backend)));

    BackgroundCompleter pump(backend);
    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < requests_per_thread; ++i) {
                std::vector<RowLease> out(1);
                auto result = table->resolve_into(std::array<std::uint64_t, 1>{0}, out);
                if (!result.has_value() || out[0].row_index() != 0) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                for (std::size_t b = 0; b < row_bytes; ++b) {
                    if (out[0].bytes()[b] != sub0mempage::test::source_byte(b)) {
                        mismatches.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    check(mismatches.load() == 0, "every concurrent resolve of row 0 observed correct, complete bytes");
    const auto stats = table->stats();
    check(stats.fetches == 1, "exactly one live fetch was started for the same row under concurrent load (R5)");
    // Every request beyond the one that actually started the fetch either joined it while still Filling
    // (coalesced) or landed after it was already Ready (hits) -- both are "did not trigger a second live
    // fetch", which is what R5 actually requires; which of the two a given request sees is a race with
    // the BackgroundCompleter and is not itself part of the contract.
    check(stats.hits + stats.coalesced >= static_cast<std::uint64_t>(thread_count * requests_per_thread - 1),
          "every request beyond the first joined the same in-flight fetch or an already-Ready row");
    (void)table->drain();
}

/// A mixed workload over several distinct rows: still only ever one live fetch per row at a time.
void test_concurrent_distinct_rows_each_fetch_once() {
    constexpr int thread_count = 16;
    constexpr std::uint64_t row_count = 8;

    Backend backend(64);
    const std::uint64_t row_bytes = 8;
    FixedWidthResolver resolver{row_bytes, row_count};
    std::vector<std::byte> output(row_bytes * row_count);
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    const auto cfg_sources = single_source(SourceId{8}, row_count * row_bytes);
    cfg.sources = cfg_sources;

    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = static_cast<std::uint32_t>(row_count);
    cfg.max_tickets = static_cast<std::uint32_t>(thread_count);
    cfg.max_batch_rows = 1;
    auto table = std::move(*Table::create(cfg, FillBackendRef(backend)));

    BackgroundCompleter pump(backend);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    std::atomic<int> failures{0};
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(static_cast<unsigned>(t) * 7919u + 17u);
            std::uniform_int_distribution<std::uint64_t> pick(0, row_count - 1);
            for (int i = 0; i < 40; ++i) {
                const std::uint64_t row = pick(rng);
                std::vector<RowLease> out(1);
                auto result = table->resolve_into(std::array<std::uint64_t, 1>{row}, out);
                if (!result.has_value() || out[0].row_index() != row) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    check(failures.load() == 0, "every concurrent resolve across distinct rows succeeded and matched its row");
    check(table->stats().fetches <= row_count, "at most one fetch per distinct row was ever started");
    (void)table->drain();
}

} // namespace

int main() {
    run(test_concurrent_same_row_coalesces, "concurrent_same_row_coalesces");
    run(test_concurrent_distinct_rows_each_fetch_once, "concurrent_distinct_rows_each_fetch_once");
    return finish();
}
