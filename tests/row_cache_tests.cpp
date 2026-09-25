// T0 acceptance gates (docs/integration-plan.md's delivery table): duplicate/order/bounds, budgets,
// RowLease lifetime, versions (generations) and codec failure, plus registration validation, stats
// (R10) and the bf16->f32 built-in codec's bit-exactness against an independently written oracle (never
// against codec.hpp's own implementation -- see the cross-project "no copied implementation becomes its
// own correctness oracle" rule).

#include "fake_backend.hpp"
#include "test_support.hpp"

#include <sub0tieredcache/sub0tieredcache.hpp>

#include <array>
#include <cstring>
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

/// A row's encoded extent is simply row_index * row_bytes, row_bytes long -- deliberately not "format
/// parsing" (AGENTS.md sec 2), just address arithmetic a caller's adapter would compute the same way.
struct FixedWidthResolver {
    std::uint64_t row_bytes = 0;
    std::uint64_t row_count = 0;

    [[nodiscard]] std::expected<ByteRange, Status> resolve_extent(std::uint64_t row) const noexcept {
        if (row >= row_count) {
            return std::unexpected(Status::out_of_range);
        }
        return ByteRange{row * row_bytes, row_bytes};
    }
};

/// A codec that always fails, for R6/R14's "codec failure publishes nothing" gate.
class FailingCodec final : public Codec {
public:
    [[nodiscard]] bool convert(std::span<const std::byte>, std::span<std::byte>) noexcept override { return false; }
};

/// One registered identity table over a FakeBackend, matching sub0mempage's own source_byte formula so
/// resolved bytes can be checked against that same independent, pure-function oracle.
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
        cfg.source = SourceId{1};
        cfg.source_bytes = row_count * row_bytes;
        cfg.generation = 1;
        cfg.resolve_extent = RowExtentResolverRef(resolver);
        cfg.output_storage = output_storage;
        cfg.budget_rows = budget_rows;
        cfg.max_tickets = max_tickets;
        cfg.max_batch_rows = max_batch;
        auto created = Table::create(cfg, FillBackendRef(backend));
        table = std::move(*created);
    }

    ~IdentityFixture() {
        backend.complete_all_newest_first();
        (void)table->drain();
    }
};

[[nodiscard]] bool matches_source(std::span<const std::byte> bytes, std::uint64_t source_offset) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != sub0mempage::test::source_byte(source_offset + i)) {
            return false;
        }
    }
    return true;
}

void test_registration_validation() {
    Backend backend(4);
    std::vector<std::byte> output(16);
    TableConfig cfg{};
    cfg.row_count = 2;
    cfg.source_row_bytes = 8;
    cfg.output_row_bytes = 8;
    cfg.representation = Representation::identity;
    cfg.source = SourceId{1};
    cfg.source_bytes = 16;
    FixedWidthResolver resolver{8, 2};
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = 2;
    cfg.max_tickets = 2;
    cfg.max_batch_rows = 2;

    check(Table::create(cfg, FillBackendRef(backend)).has_value(), "valid identity config registers");

    TableConfig bad_width = cfg;
    bad_width.output_row_bytes = 4; // identity requires equal widths
    check(Table::create(bad_width, FillBackendRef(backend)).error() == Status::unsupported_conversion,
          "identity with mismatched widths is rejected at registration");

    TableConfig bad_storage = cfg;
    bad_storage.budget_rows = 3; // output_storage still sized for 2 rows
    check(Table::create(bad_storage, FillBackendRef(backend)).error() == Status::invalid_argument,
          "output_storage size must match budget_rows * output_row_bytes");

    TableConfig custom_no_codec = cfg;
    custom_no_codec.representation = Representation::custom;
    custom_no_codec.codec = nullptr;
    check(Table::create(custom_no_codec, FillBackendRef(backend)).error() == Status::invalid_argument,
          "custom representation without a codec is rejected");

    TableConfig bad_bf16 = cfg;
    bad_bf16.representation = Representation::bf16_to_f32;
    bad_bf16.output_row_bytes = 8; // must be source_row_bytes * 2 == 16
    check(Table::create(bad_bf16, FillBackendRef(backend)).error() == Status::unsupported_conversion,
          "bf16_to_f32 with the wrong output width is rejected");
}

void test_bounds() {
    IdentityFixture f;
    BackgroundCompleter pump(f.backend);
    std::vector<RowLease> out(1);
    auto result = f.table->resolve_into(std::array{f.row_count}, out); // == row_count is out of range
    check(!result.has_value() && result.error() == Status::out_of_range, "row_index == row_count is out of range");
    check(!out[0].is_held(), "no lease held after an out-of-range request");

    check(!f.table->try_get(f.row_count).has_value(), "try_get on an out-of-range row misses cleanly");
}

void test_duplicate_and_order() {
    IdentityFixture f;
    BackgroundCompleter pump(f.backend);
    const std::array<std::uint64_t, 4> rows{3, 1, 3, 2};
    std::vector<RowLease> out(rows.size());
    auto result = f.table->resolve_into(rows, out);
    check(result.has_value() && *result == rows.size(), "duplicate/ordered batch resolves fully");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        check(out[i].row_index() == rows[i], "lease i names the request-order row it was asked for");
        check(matches_source(out[i].bytes(), rows[i] * f.row_bytes), "lease bytes match the source oracle");
    }
    check(out[0].bytes().data() == out[2].bytes().data(), "both leases on row 3 point at the same resident slot");
}

void test_budget_exhaustion_is_all_or_nothing() {
    IdentityFixture f(/*rows=*/20, /*bytes=*/8, /*budget=*/2);
    BackgroundCompleter pump(f.backend);
    std::vector<RowLease> held(2);
    auto first = f.table->resolve_into(std::array<std::uint64_t, 2>{0, 1}, held);
    check(first.has_value(), "filling the whole budget succeeds");

    std::vector<RowLease> out(1);
    auto third = f.table->resolve_into(std::array{std::uint64_t{2}}, out);
    check(!third.has_value() && third.error() == Status::pool_exhausted,
          "a third distinct row cannot evict two pinned rows out of a 2-row budget");
    check(!out[0].is_held(), "the failed call leaves no lease held (R14 all-or-nothing)");
    check(f.table->stats().leased == 2, "the two rows pinned before the failing call are still held");

    held[0].reset();
    held[1].reset();
    check(f.table->stats().leased == 0, "releasing both leases frees the budget");
}

void test_row_lease_lifetime_blocks_eviction() {
    IdentityFixture f(/*rows=*/20, /*bytes=*/8, /*budget=*/1);
    BackgroundCompleter pump(f.backend);
    std::vector<RowLease> out(1);
    check(f.table->resolve_into(std::array{std::uint64_t{0}}, out).has_value(), "row 0 fills the single slot");

    std::vector<RowLease> other(1);
    auto blocked = f.table->resolve_into(std::array{std::uint64_t{1}}, other);
    check(!blocked.has_value() && blocked.error() == Status::pool_exhausted, "the pinned row cannot be evicted");

    out[0].reset(); // release row 0's lease
    check(f.table->resolve_into(std::array{std::uint64_t{1}}, other).has_value(),
          "row 1 can now claim the freed slot");
}

void test_generations() {
    IdentityFixture f(/*rows=*/20, /*bytes=*/8, /*budget=*/2);
    BackgroundCompleter pump(f.backend);
    std::vector<RowLease> gen1(1);
    check(f.table->resolve_into(std::array{std::uint64_t{0}}, gen1).has_value(), "row 0 resolves under generation 1");
    check(gen1[0].generation() == 1, "the lease captures generation 1");

    f.table->invalidate(2);
    std::vector<RowLease> gen2(1);
    check(f.table->resolve_into(std::array{std::uint64_t{0}}, gen2).has_value(),
          "row 0 resolves again under generation 2 while the generation-1 lease is still held");
    check(gen2[0].generation() == 2, "the new lease captures generation 2");
    check(matches_source(gen1[0].bytes(), 0), "the old lease's bytes are still valid and unchanged");
    check(f.table->stats().fetches == 2, "invalidation forced a real second fetch, not a stale hit");
}

void test_generation_budget_conflict_is_rejected() {
    // budget == 1: an old generation's pinned lease and a new fetch for the same row cannot coexist.
    IdentityFixture f(/*rows=*/20, /*bytes=*/8, /*budget=*/1);
    BackgroundCompleter pump(f.backend);
    std::vector<RowLease> gen1(1);
    check(f.table->resolve_into(std::array{std::uint64_t{0}}, gen1).has_value(), "row 0 resolves under generation 1");

    f.table->invalidate(2);
    std::vector<RowLease> gen2(1);
    auto result = f.table->resolve_into(std::array{std::uint64_t{0}}, gen2);
    check(!result.has_value() && result.error() == Status::pool_exhausted,
          "R4: a budget too small for both live generations is rejected explicitly, never overcommitted");
}

void test_codec_failure_publishes_nothing() {
    Backend backend(16);
    const std::uint64_t row_bytes = 8;
    const std::uint64_t row_count = 4;
    const std::uint32_t budget = 2;
    FixedWidthResolver resolver{row_bytes, row_count};
    std::vector<std::byte> output(std::size_t{budget} * row_bytes);
    std::vector<std::byte> scratch(std::size_t{budget} * row_bytes);
    FailingCodec codec;

    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes; // widths are irrelevant to a custom codec's own contract
    cfg.representation = Representation::custom;
    cfg.codec = &codec;
    cfg.source = SourceId{2};
    cfg.source_bytes = row_count * row_bytes;
    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = budget;
    cfg.scratch_storage = scratch;
    cfg.scratch_rows = budget;
    cfg.max_tickets = 2;
    cfg.max_batch_rows = 2;
    auto table = std::move(*Table::create(cfg, FillBackendRef(backend)));

    test::BackgroundCompleter pump(backend);
    std::vector<RowLease> out(1);
    auto result = table->resolve_into(std::array{std::uint64_t{0}}, out);
    check(!result.has_value() && result.error() == Status::codec_failed, "a failing codec reports codec_failed");
    check(!out[0].is_held(), "nothing is published when the codec fails (R14)");
    check(table->stats().codec_failures == 1, "the failure is observable via stats() (R10)");
    (void)table->drain();
}

void test_try_get_never_touches_transport() {
    IdentityFixture f;
    check(!f.table->try_get(0).has_value(), "try_get misses on a row nobody has fetched");
    check(f.backend.accepted() == 0, "a plain miss never touches the transport (R11)");

    auto ticket = f.table->prefetch(std::array{std::uint64_t{0}});
    check(ticket.has_value(), "prefetch submits without blocking");
    check(f.backend.accepted() == 1, "prefetch did submit the underlying fetch");
    check(!f.table->try_get(0).has_value(), "a still-filling row is reported as a miss, not awaited (R11)");

    f.backend.complete_all_newest_first();
    auto outcome = f.table->wait(*ticket);
    check(outcome.status == Status::ok && outcome.filled == 1, "wait() observes the now-complete fetch");
    auto lease = f.table->try_get(0);
    check(lease.has_value() && matches_source(lease->bytes(), 0), "try_get hits once the row is Ready");
}

void test_bf16_to_f32_bit_exact() {
    Backend backend(8);
    const std::uint64_t elements = 4;
    const std::uint64_t source_row_bytes = elements * 2;
    const std::uint64_t output_row_bytes = elements * 4;
    const std::uint64_t row_count = 1;
    const std::uint32_t budget = 1;

    FixedWidthResolver resolver{source_row_bytes, row_count};

    std::vector<std::byte> output(output_row_bytes);
    std::vector<std::byte> scratch(source_row_bytes);

    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = source_row_bytes;
    cfg.output_row_bytes = output_row_bytes;
    cfg.representation = Representation::bf16_to_f32;
    cfg.source = SourceId{3};
    cfg.source_bytes = source_row_bytes;
    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = budget;
    cfg.scratch_storage = scratch;
    cfg.scratch_rows = budget;
    cfg.max_tickets = 1;
    cfg.max_batch_rows = 1;
    auto table = std::move(*Table::create(cfg, FillBackendRef(backend)));

    // The FakeBackend writes its own deterministic source_byte() pattern, not `source_bytes` above --
    // it doesn't know about our row content. So the codec's *input* has to arrive through the fake
    // transport regardless; what we independently verify is the *conversion*, by feeding the backend's
    // own delivered bytes through a widening formula written here, separately from codec.hpp.
    test::BackgroundCompleter pump(backend);
    std::vector<RowLease> out(1);
    auto result = table->resolve_into(std::array{std::uint64_t{0}}, out);
    check(result.has_value(), "bf16_to_f32 conversion succeeds");
    check(table->stats().conversions == 1, "the conversion is counted (R10)");

    for (std::uint64_t i = 0; i < elements; ++i) {
        std::uint16_t bits;
        const std::byte raw[2] = {sub0mempage::test::source_byte(2 * i), sub0mempage::test::source_byte(2 * i + 1)};
        std::memcpy(&bits, raw, 2);
        // Independently written bit-exact reference: bf16's bit pattern IS an f32's top 16 bits, so
        // widening is a zero-extending left shift by 16 -- written here from first principles, not by
        // calling codec.hpp's detail::bf16_bits_to_f32_bits.
        const std::uint32_t expected_bits = static_cast<std::uint32_t>(bits) << 16;
        std::uint32_t actual_bits;
        std::memcpy(&actual_bits, out[0].bytes().data() + 4 * i, 4);
        check(actual_bits == expected_bits, "bf16->f32 widening is bit-exact for this element");
    }
}

void test_bf16_codec_special_values() {
    // Exercises Bf16ToF32Codec directly (no transport involved) so specific bit patterns -- +0, -0, a
    // quiet NaN, +Inf, -Inf and a denormal -- can be chosen deliberately, independently verified against
    // a widening formula written here rather than reused from codec.hpp.
    Bf16ToF32Codec codec;
    const std::array<std::uint16_t, 6> patterns{
        0x0000, // +0
        0x8000, // -0
        0x7FC0, // quiet NaN
        0x7F80, // +Inf
        0xFF80, // -Inf
        0x0001, // smallest positive denormal
    };
    std::vector<std::byte> source(patterns.size() * 2);
    std::memcpy(source.data(), patterns.data(), source.size());
    std::vector<std::byte> destination(patterns.size() * 4);

    check(codec.convert(source, destination), "Bf16ToF32Codec::convert succeeds on well-formed widths");
    for (std::size_t i = 0; i < patterns.size(); ++i) {
        const std::uint32_t expected_bits = static_cast<std::uint32_t>(patterns[i]) << 16;
        std::uint32_t actual_bits;
        std::memcpy(&actual_bits, destination.data() + 4 * i, 4);
        check(actual_bits == expected_bits, "bf16 special-value bit pattern widens bit-exactly");
    }

    std::vector<std::byte> wrong_size(destination.size() - 1);
    check(!codec.convert(source, wrong_size), "a mismatched destination width is rejected, not truncated");
}

void test_stats_snapshot() {
    IdentityFixture f(/*rows=*/20, /*bytes=*/8, /*budget=*/4);
    BackgroundCompleter pump(f.backend);
    std::vector<RowLease> out(2);
    check(f.table->resolve_into(std::array<std::uint64_t, 2>{0, 1}, out).has_value(), "warm two rows");
    auto stats1 = f.table->stats();
    check(stats1.fetches == 2 && stats1.resident == 2 && stats1.leased == 2, "fresh fetches are counted and resident/leased");

    std::vector<RowLease> hit(1);
    check(f.table->resolve_into(std::array{std::uint64_t{0}}, hit).has_value(), "resolve the same row again");
    auto stats2 = f.table->stats();
    check(stats2.hits == 1 && stats2.fetches == 2, "a second resolve of an already-Ready row is a hit, not a fetch");
}

} // namespace

int main() {
    run(test_registration_validation, "registration_validation");
    run(test_bounds, "bounds");
    run(test_duplicate_and_order, "duplicate_and_order");
    run(test_budget_exhaustion_is_all_or_nothing, "budget_exhaustion_is_all_or_nothing");
    run(test_row_lease_lifetime_blocks_eviction, "row_lease_lifetime_blocks_eviction");
    run(test_generations, "generations");
    run(test_generation_budget_conflict_is_rejected, "generation_budget_conflict_is_rejected");
    run(test_codec_failure_publishes_nothing, "codec_failure_publishes_nothing");
    run(test_try_get_never_touches_transport, "try_get_never_touches_transport");
    run(test_bf16_to_f32_bit_exact, "bf16_to_f32_bit_exact");
    run(test_bf16_codec_special_values, "bf16_codec_special_values");
    run(test_stats_snapshot, "stats_snapshot");
    return finish();
}
