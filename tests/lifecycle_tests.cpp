// Gates added on lead review of row_cache.hpp @ ea59c15:
//   1. Unfinished fills must not leak (completion worker).
//   2. resolve_into overlaps a batch's own I/O (two-phase admit-then-await).
//   3. A terminal failure unindexes immediately and its slot is reclaimable, not leaked.
//   4. invalidate() carries a new immutable source snapshot; at most one binding retires at a time.
//   5. wait() honours its own deadline even when it becomes the finisher.
//   6. RowCache holds independent tables; WaitOutcome reports "evicted" for a reclaimed prefetch.

#include <sub0mempage/testing/fake_backend.hpp>
#include "test_support.hpp"

#include <sub0tieredcache/sub0tieredcache.hpp>

#include <array>
#include <chrono>
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

/// Like FixedWidthResolver, but every extent is shifted by `base` -- two resolvers with different bases
/// read different source_byte() patterns for the "same" row_index, which is how these tests observe a
/// generation's new source binding actually being used (docs/integration-plan.md's "Versioning and
/// remote tier": invalidation carries a new immutable snapshot). This works without needing the fake
/// backend itself to vary bytes by SourceId (transfer-contract.md treats SourceId as opaque plumbing).
struct OffsetResolver {
    std::uint64_t base;
    std::uint64_t row_bytes;
    std::uint64_t row_count;
    [[nodiscard]] std::expected<RowLocation, Status> resolve_extent(std::uint64_t row) const noexcept {
        if (row >= row_count) {
            return std::unexpected(Status::out_of_range);
        }
        return RowLocation{0, ByteRange{base + row * row_bytes, row_bytes}};
    }
};

/// row 0's extent is deliberately the wrong length; every other row is fine. Exercises
/// start_fill_locked's own "extent->length != source_row_bytes" admission failure.
struct BadLengthResolver {
    std::uint64_t row_bytes;
    std::uint64_t row_count;
    [[nodiscard]] std::expected<RowLocation, Status> resolve_extent(std::uint64_t row) const noexcept {
        if (row >= row_count) {
            return std::unexpected(Status::out_of_range);
        }
        return row == 0 ? RowLocation{0, ByteRange{0, row_bytes / 2}} : RowLocation{0, ByteRange{row * row_bytes, row_bytes}};
    }
};

struct IdentityFixture {
    std::uint64_t row_bytes;
    std::uint64_t row_count;
    std::uint32_t budget_rows;
    Backend backend;
    FixedWidthResolver resolver;
    std::vector<std::byte> output_storage;
    std::unique_ptr<Table> table;

    explicit IdentityFixture(std::uint64_t rows = 20, std::uint64_t bytes = 8, std::uint32_t budget = 4,
                              std::uint32_t max_batch = 8, std::uint32_t max_tickets = 4)
        : row_bytes(bytes), row_count(rows), budget_rows(budget), backend(64), resolver{bytes, rows},
          output_storage(std::size_t{budget} * bytes) {
        TableConfig cfg{};
        cfg.row_count = row_count;
        cfg.source_row_bytes = row_bytes;
        cfg.output_row_bytes = row_bytes;
        cfg.representation = Representation::identity;
        const auto cfg_sources = single_source(SourceId{1}, row_count * row_bytes);
        cfg.sources = cfg_sources;

        cfg.generation = 1;
        cfg.resolve_extent = RowExtentResolverRef(resolver);
        cfg.output_storage = output_storage;
        cfg.budget_rows = budget_rows;
        cfg.max_tickets = max_tickets;
        cfg.max_batch_rows = max_batch;
        table = std::move(*Table::create(cfg, FillBackendRef(backend)));
    }

    ~IdentityFixture() {
        backend.complete_all_newest_first();
        (void)table->drain();
    }
};

// --- fix 1: unfinished fills must not leak --------------------------------------------------------------

void test_dropped_prefetch_ticket_still_completes() {
    IdentityFixture f(/*rows=*/20, /*bytes=*/8, /*budget=*/2);
    {
        auto ticket = f.table->prefetch(std::array<std::uint64_t, 1>{5});
        check(ticket.has_value(), "prefetch admits row 5");
        // `ticket` is dropped here without ever being waited on.
    }
    f.backend.complete_all_newest_first();
    check(f.table->drain() == Status::ok, "drain() returns once the completion worker finishes the abandoned fill");
    check(f.table->stats().resident == 1, "the abandoned fill still reached Ready on its own");

    auto lease = f.table->try_get(5);
    check(lease.has_value(), "the row is now a normal, evictable resident row -- nothing was left stuck Filling");
}

void test_resolve_into_partial_admission_failure_leaves_no_stuck_fill() {
    IdentityFixture f(/*rows=*/20, /*bytes=*/8, /*budget=*/2, /*max_batch=*/4);
    std::vector<RowLease> out(3);
    auto result = f.table->resolve_into(std::array<std::uint64_t, 3>{0, 1, 2}, out);
    check(!result.has_value() && result.error() == Status::pool_exhausted, "the third row cannot fit a 2-row budget");
    check(!out[0].is_held() && !out[1].is_held() && !out[2].is_held(), "all-or-nothing: nothing left held");

    f.backend.complete_all_newest_first();
    check(f.table->drain() == Status::ok, "the two fills already submitted before the failure still drain cleanly");
    check(f.table->stats().resident == 2, "both rows admitted before the failure reached Ready via the completion worker");
}

// --- fix 2: resolve_into overlaps its own batch's I/O ----------------------------------------------------

void test_resolve_into_overlaps_batch_io() {
    IdentityFixture f(/*rows=*/20, /*bytes=*/8, /*budget=*/4, /*max_batch=*/4);
    std::vector<RowLease> out(3);
    bool succeeded = false;
    std::thread worker([&] {
        auto result = f.table->resolve_into(std::array<std::uint64_t, 3>{0, 1, 2}, out);
        succeeded = result.has_value();
    });
    while (f.backend.pending() < 3) {
        std::this_thread::yield();
    }
    check(f.backend.pending() == 3, "phase A admitted and submitted all three misses before waiting on any of them");
    f.backend.complete_all_newest_first();
    worker.join();
    check(succeeded, "the batch resolves once all three complete");
    check(f.table->stats().fetches == 3, "three independent fetches were started, not serialised one at a time");
}

void test_duplicate_rows_in_batch_cause_one_fetch() {
    IdentityFixture f(/*rows=*/20, /*bytes=*/8, /*budget=*/4, /*max_batch=*/4);
    BackgroundCompleter pump(f.backend);
    std::vector<RowLease> out(3);
    auto result = f.table->resolve_into(std::array<std::uint64_t, 3>{7, 7, 7}, out);
    check(result.has_value(), "a batch that repeats one row three times still resolves");
    check(f.table->stats().fetches == 1, "duplicates in the same batch still cause exactly one fetch");
    check(f.table->stats().coalesced == 2, "the other two occurrences joined that one fetch");
}

// --- fix 3: a terminal failure unindexes immediately and its slot is reclaimable -------------------------

void test_transient_failure_then_retry_succeeds() {
    Backend backend(8);
    const std::uint64_t row_bytes = 8;
    const std::uint64_t row_count = 4;
    OffsetResolver resolver{0, row_bytes, row_count};
    std::vector<std::byte> output(row_bytes * 2);
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    const auto cfg_sources = single_source(SourceId{9}, row_count * row_bytes);
    cfg.sources = cfg_sources;

    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = 2;
    cfg.max_tickets = 2;
    cfg.max_batch_rows = 2;
    auto table = std::move(*Table::create(cfg, FillBackendRef(backend)));

    {
        std::thread attempt([&] {
            std::vector<RowLease> out(1);
            auto result = table->resolve_into(std::array<std::uint64_t, 1>{1}, out);
            check(!result.has_value() && result.error() == Status::io_error,
                  "a transient transport failure is reported, not silently retried");
        });
        while (backend.pending() == 0) {
            std::this_thread::yield();
        }
        backend.complete(0, sub0mempage::test::FakeCompletion::io_error);
        attempt.join();
    }
    check(table->stats().failed == 1, "the failure is observable via stats() (R10)");

    {
        test::BackgroundCompleter pump(backend);
        std::vector<RowLease> out(1);
        auto result = table->resolve_into(std::array<std::uint64_t, 1>{1}, out);
        check(result.has_value(), "R14: reads repeated after a failure are allowed, and this retry succeeds");
    }
    (void)table->drain();
}

void test_prefetch_admission_failure_slot_is_reclaimable() {
    Backend backend(8);
    const std::uint64_t row_bytes = 8;
    const std::uint64_t row_count = 4;
    BadLengthResolver resolver{row_bytes, row_count};
    std::vector<std::byte> output(row_bytes); // budget_rows == 1: the only slot in the whole table
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    const auto cfg_sources = single_source(SourceId{11}, row_count * row_bytes);
    cfg.sources = cfg_sources;

    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = 1;
    cfg.max_tickets = 2;
    cfg.max_batch_rows = 1;
    auto table = std::move(*Table::create(cfg, FillBackendRef(backend)));

    auto ticket = table->prefetch(std::array<std::uint64_t, 1>{0}); // row 0's resolver deliberately fails
    check(ticket.has_value(), "prefetch itself succeeds; the failure is per-row, recorded on the slot");
    auto outcome = table->wait(*ticket);
    check(outcome.status != Status::ok && outcome.failed == 1, "row 0's admission failure is reported by wait()");

    test::BackgroundCompleter pump(backend);
    std::vector<RowLease> out(1);
    auto result = table->resolve_into(std::array<std::uint64_t, 1>{1}, out);
    check(result.has_value(), "the only slot was reclaimed for row 1 -- not leaked as a permanently-Failed slot");
    (void)table->drain();
}

// --- fix 4: invalidate() carries a new immutable source snapshot ------------------------------------------

void test_invalidate_new_source_old_lease_unaffected() {
    Backend backend(32);
    const std::uint64_t row_bytes = 8;
    const std::uint64_t row_count = 4;
    OffsetResolver resolver_v1{0, row_bytes, row_count};
    OffsetResolver resolver_v2{100'000, row_bytes, row_count};
    const std::uint64_t source_bytes = 200'000;
    std::vector<std::byte> output(row_bytes * 2);
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    const auto cfg_sources = single_source(SourceId{5}, source_bytes);
    cfg.sources = cfg_sources;

    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver_v1);
    cfg.output_storage = output;
    cfg.budget_rows = 2;
    cfg.max_tickets = 2;
    cfg.max_batch_rows = 2;
    auto table = std::move(*Table::create(cfg, FillBackendRef(backend)));
    test::BackgroundCompleter pump(backend);

    std::vector<RowLease> gen1(1);
    check(table->resolve_into(std::array<std::uint64_t, 1>{2}, gen1).has_value(), "row 2 resolves under generation 1");
    check(gen1[0].generation() == 1, "the lease captures generation 1");
    const std::vector<std::byte> gen1_bytes(gen1[0].bytes().begin(), gen1[0].bytes().end());

    check(table->invalidate(2, single_source(SourceId{6}, source_bytes), RowExtentResolverRef(resolver_v2)) == Status::ok,
          "invalidate succeeds with a new source and resolver");

    std::vector<RowLease> gen2(1);
    check(table->resolve_into(std::array<std::uint64_t, 1>{2}, gen2).has_value(), "row 2 resolves fresh under generation 2");
    check(gen2[0].generation() == 2, "the new lease captures generation 2");
    check(!std::equal(gen1_bytes.begin(), gen1_bytes.end(), gen2[0].bytes().begin()),
          "generation 2 reads a genuinely different source snapshot, not a re-read of the old one");
    check(std::equal(gen1_bytes.begin(), gen1_bytes.end(), gen1[0].bytes().begin()),
          "the generation-1 lease's bytes are completely unaffected by the transition");
    check(table->stats().fetches == 2, "two independent fetches, one per generation");
    (void)table->drain();
}

void test_invalidate_busy_while_retiring_in_flight() {
    Backend backend(8);
    const std::uint64_t row_bytes = 8;
    const std::uint64_t row_count = 4;
    OffsetResolver resolver{0, row_bytes, row_count};
    std::vector<std::byte> output(row_bytes); // budget_rows == 1
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    const auto cfg_sources = single_source(SourceId{12}, row_count * row_bytes);
    cfg.sources = cfg_sources;

    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = 1;
    cfg.max_tickets = 2;
    cfg.max_batch_rows = 1;
    auto table = std::move(*Table::create(cfg, FillBackendRef(backend)));

    // No BackgroundCompleter yet: row 0's fetch is deliberately left in flight under generation 1.
    auto ticket = table->prefetch(std::array<std::uint64_t, 1>{0});
    check(ticket.has_value(), "row 0's fetch starts under generation 1");

    check(table->invalidate(2, single_source(SourceId{13}, row_count * row_bytes)) == Status::ok,
          "the first invalidate succeeds -- the still-in-flight fetch becomes the retiring binding's problem");
    const auto busy = table->invalidate(3, single_source(SourceId{14}, row_count * row_bytes));
    check(busy == Status::busy,
          "R4: a second invalidate while the retiring binding still has an in-flight fetch is rejected, "
          "not silently losing track of the live transfer");

    backend.complete_all_newest_first();
    check(table->drain() == Status::ok, "the retiring generation's fetch still completes on its own");
    check(table->invalidate(3, single_source(SourceId{14}, row_count * row_bytes)) == Status::ok,
          "once the retiring binding has drained, invalidate succeeds again");
    (void)table->drain();
}

// --- fix 5: wait() honours its own deadline even when it becomes the finisher ------------------------------

void test_wait_deadline_leaves_row_pending() {
    Backend backend(8);
    const std::uint64_t row_bytes = 8;
    const std::uint64_t row_count = 4;
    OffsetResolver resolver{0, row_bytes, row_count};
    std::vector<std::byte> output(row_bytes * 2);
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    const auto cfg_sources = single_source(SourceId{15}, row_count * row_bytes);
    cfg.sources = cfg_sources;

    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = 2;
    cfg.max_tickets = 2;
    cfg.max_batch_rows = 2;
    auto table = std::move(*Table::create(cfg, FillBackendRef(backend)));

    // No BackgroundCompleter: the fetch is deliberately never completed until after the deadline.
    auto ticket = table->prefetch(std::array<std::uint64_t, 1>{0});
    check(ticket.has_value(), "row 0's fetch starts");
    const auto outcome = table->wait(*ticket, Clock::now() + std::chrono::milliseconds(50));
    check(outcome.status == Status::timeout && outcome.pending == 1,
          "wait() times out while the underlying fetch is still outstanding, rather than blocking forever");

    backend.complete_all_newest_first();
    const auto outcome2 = table->wait(*ticket);
    check(outcome2.status == Status::ok && outcome2.filled == 1,
          "a later wait() with no deadline observes the now-complete fetch -- the timeout left it live, not lost");
    (void)table->drain();
}

// --- fix 6: RowCache independence, and WaitOutcome::evicted ------------------------------------------------

void test_row_cache_multiple_tables_are_independent() {
    Backend backend(16);
    const std::uint64_t row_bytes = 8;
    const std::uint64_t row_count = 4;
    FixedWidthResolver resolver_a{row_bytes, row_count};
    FixedWidthResolver resolver_b{row_bytes, row_count};
    std::vector<std::byte> output_a(row_bytes * 2);
    std::vector<std::byte> output_b(row_bytes * 2);

    TableConfig cfg_a{};
    cfg_a.row_count = row_count;
    cfg_a.source_row_bytes = row_bytes;
    cfg_a.output_row_bytes = row_bytes;
    cfg_a.representation = Representation::identity;
    const auto cfg_a_sources = single_source(SourceId{20}, row_count * row_bytes);
    cfg_a.sources = cfg_a_sources;

    cfg_a.generation = 1;
    cfg_a.resolve_extent = RowExtentResolverRef(resolver_a);
    cfg_a.output_storage = output_a;
    cfg_a.budget_rows = 2;
    cfg_a.max_tickets = 2;
    cfg_a.max_batch_rows = 2;

    TableConfig cfg_b = cfg_a;
    const auto cfg_b_sources = single_source(SourceId{21}, row_count * row_bytes);
    cfg_b.sources = cfg_b_sources;
    cfg_b.resolve_extent = RowExtentResolverRef(resolver_b);
    cfg_b.output_storage = output_b;

    RowCache cache;
    auto handle_a = cache.register_table(cfg_a, FillBackendRef(backend));
    auto handle_b = cache.register_table(cfg_b, FillBackendRef(backend));
    check(handle_a.has_value() && handle_b.has_value(), "both tables register");
    check(*handle_a != *handle_b, "the two tables get distinct handles");
    check(cache.table_count() == 2, "two tables are registered");
    check(cache.table(*handle_a) != nullptr && cache.table(*handle_b) != nullptr, "both handles resolve");

    test::BackgroundCompleter pump(backend);
    std::vector<RowLease> out_a(1);
    check(cache.table(*handle_a)->resolve_into(std::array<std::uint64_t, 1>{0}, out_a).has_value(),
          "table A resolves row 0 through its own binding");
    check(!cache.table(*handle_b)->try_get(0).has_value(), "table B's own cache is untouched by table A's activity");
    check(cache.table(*handle_a)->stats().fetches == 1 && cache.table(*handle_b)->stats().fetches == 0,
          "each table's stats() is independent");
    (void)cache.table(*handle_a)->drain();
    (void)cache.table(*handle_b)->drain();
}

void test_wait_outcome_evicted() {
    IdentityFixture f(/*rows=*/20, /*bytes=*/8, /*budget=*/1);
    BackgroundCompleter pump(f.backend);
    auto ticket = f.table->prefetch(std::array<std::uint64_t, 1>{0});
    check(ticket.has_value(), "prefetch admits row 0 into the only slot");
    check(f.table->drain() == Status::ok, "the completion worker drives it to Ready on its own");
    check(f.table->stats().resident == 1, "row 0 is resident and unpinned (prefetch never pins, R6)");

    std::vector<RowLease> out(1);
    check(f.table->resolve_into(std::array<std::uint64_t, 1>{1}, out).has_value(),
          "row 1 forces eviction of row 0's now-Ready, unpinned, unreferenced slot (budget == 1)");

    const auto outcome = f.table->wait(*ticket);
    check(outcome.evicted == 1, "the original ticket observes its row as reclaimed before it was ever consumed");
}

} // namespace

int main() {
    run(test_dropped_prefetch_ticket_still_completes, "dropped_prefetch_ticket_still_completes");
    run(test_resolve_into_partial_admission_failure_leaves_no_stuck_fill,
        "resolve_into_partial_admission_failure_leaves_no_stuck_fill");
    run(test_resolve_into_overlaps_batch_io, "resolve_into_overlaps_batch_io");
    run(test_duplicate_rows_in_batch_cause_one_fetch, "duplicate_rows_in_batch_cause_one_fetch");
    run(test_transient_failure_then_retry_succeeds, "transient_failure_then_retry_succeeds");
    run(test_prefetch_admission_failure_slot_is_reclaimable, "prefetch_admission_failure_slot_is_reclaimable");
    run(test_invalidate_new_source_old_lease_unaffected, "invalidate_new_source_old_lease_unaffected");
    run(test_invalidate_busy_while_retiring_in_flight, "invalidate_busy_while_retiring_in_flight");
    run(test_wait_deadline_leaves_row_pending, "wait_deadline_leaves_row_pending");
    run(test_row_cache_multiple_tables_are_independent, "row_cache_multiple_tables_are_independent");
    run(test_wait_outcome_evicted, "wait_outcome_evicted");
    return finish();
}
