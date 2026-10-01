// SizeClassedTable: rows of a few exact sizes held in exact-width slots. Bytes are checked against
// Sub0MemPage's independent source_byte oracle, never against the table itself.

#include <sub0mempage/testing/fake_backend.hpp>
#include "test_support.hpp"

#include <sub0tieredcache/sub0tieredcache.hpp>

#include <array>
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

// Rows packed back to back: even rows 8 bytes, odd rows 12 -- two classes, like experts whose size
// differs between layers.
constexpr std::uint64_t kRows = 24;
constexpr std::uint64_t width(std::uint64_t row) { return row % 2 == 0 ? 8 : 12; }
constexpr std::uint64_t offset(std::uint64_t row) { return (row / 2) * 20 + (row % 2) * 8; }
constexpr std::uint64_t kSourceBytes = kRows / 2 * 20;

struct TwoWidthResolver {
    std::uint64_t widest_override = 0; // non-zero: row 5 gets this width (a third class)
    [[nodiscard]] std::expected<RowLocation, Status> resolve_extent(std::uint64_t row) const noexcept {
        if (row >= kRows) return std::unexpected(Status::out_of_range);
        const std::uint64_t w = (widest_override != 0 && row == 5) ? widest_override : width(row);
        return RowLocation{0, ByteRange{offset(row), w}};
    }
};

[[nodiscard]] bool matches_source(std::span<const std::byte> bytes, std::uint64_t source_offset) {
    for (std::size_t i = 0; i < bytes.size(); ++i)
        if (bytes[i] != sub0mempage::test::source_byte(source_offset + i)) return false;
    return true;
}

SizeClassedTableConfig config(std::span<std::byte> storage, std::span<const ShardSource> sources,
                              RowExtentResolverRef resolver) {
    SizeClassedTableConfig cfg{};
    cfg.row_count = kRows;
    cfg.sources = sources;
    cfg.generation = 1;
    cfg.resolve_extent = resolver;
    cfg.output_storage = storage;
    cfg.max_tickets = 4;
    cfg.max_batch_rows = 2;
    return cfg;
}

void test_classes_and_exact_slots() {
    Backend backend(32);
    TwoWidthResolver resolver;
    std::vector<std::byte> storage(160); // 8 rows' worth: 80 bytes for each class's share
    const auto sources = single_source(SourceId{1}, kSourceBytes);
    auto table = SizeClassedTable::create(config(storage, sources, RowExtentResolverRef(resolver)), FillBackendRef(backend));
    check(table.has_value(), "a two-width table registers");
    const auto classes = (*table)->classes();
    check(classes.size() == 2 && classes[0].width == 8 && classes[1].width == 12, "two classes, by increasing width");
    check(classes[0].rows == 12 && classes[1].rows == 12, "every row is in its own width's class");
    // Shares follow total bytes (12x8 : 12x12 = 2:3), so 64 and 96 bytes: 8 rows of 8, 8 rows of 12.
    check(classes[0].budget_rows == 8 && classes[1].budget_rows == 8, "each class gets whole exact-width slots");
    // One single-width table over the same 160 bytes would hold only 160 / 12 = 13 rows.
    check(classes[0].budget_rows + classes[1].budget_rows > 160 / 12, "exact slots hold more rows than padded ones");
    {
        BackgroundCompleter pump(backend);
        const std::array<std::uint64_t, 2> rows{5, 4};
        check((*table)->prefetch(rows) == Status::ok, "prefetch spans both classes");
        std::vector<RowLease> out(2);
        auto got = (*table)->resolve_into(rows, out);
        check(got.has_value() && *got == 2, "a mixed-class batch resolves fully");
        for (std::size_t i = 0; i < rows.size(); ++i) {
            check(out[i].bytes().size() == width(rows[i]), "a lease exposes exactly its row's width");
            check(matches_source(out[i].bytes(), offset(rows[i])), "bytes match the source oracle");
        }
        auto hit = (*table)->try_get(5);
        check(hit.has_value() && matches_source(hit->bytes(), offset(5)), "try_get finds a resident row in its class");
        check(!(*table)->try_get(6).has_value(), "try_get misses a row nobody fetched");
        check((*table)->stats().fetches == 2, "stats sum across classes");
    }
    backend.complete_all_newest_first();
    (void)(*table)->drain();
}

void test_resolve_is_all_or_nothing() {
    Backend backend(32);
    TwoWidthResolver resolver;
    std::vector<std::byte> storage(160);
    const auto sources = single_source(SourceId{1}, kSourceBytes);
    auto table = SizeClassedTable::create(config(storage, sources, RowExtentResolverRef(resolver)), FillBackendRef(backend));
    check(table.has_value(), "table registers");
    {
        BackgroundCompleter pump(backend);
        const std::array<std::uint64_t, 2> rows{3, kRows};
        std::vector<RowLease> out(2);
        auto got = (*table)->resolve_into(rows, out);
        check(!got.has_value() && got.error() == Status::out_of_range, "an out-of-range row fails the batch");
        check(!out[0].is_held(), "the batch's earlier lease is released (all-or-nothing)");
    }
    backend.complete_all_newest_first();
    (void)(*table)->drain();
}

void test_registration_refusals() {
    Backend backend(32);
    const auto sources = single_source(SourceId{1}, kSourceBytes);
    TwoWidthResolver three{16};
    std::vector<std::byte> storage(1024);
    auto cfg = config(storage, sources, RowExtentResolverRef(three));
    cfg.max_classes = 2;
    check(SizeClassedTable::create(cfg, FillBackendRef(backend)).error() == Status::invalid_argument,
          "more distinct widths than max_classes is refused");
    // Row 5 alone is 16 bytes wide: a one-row class, held whole, is not refused for lacking two batches.
    std::vector<std::byte> whole(512);
    auto table = SizeClassedTable::create(config(whole, sources, RowExtentResolverRef(three)), FillBackendRef(backend));
    check(table.has_value() && (*table)->classes().size() == 3 && (*table)->classes()[2].budget_rows == 1,
          "a class smaller than two batches is accepted when it is held whole");
    TwoWidthResolver two;
    std::vector<std::byte> tiny(40);
    check(SizeClassedTable::create(config(tiny, sources, RowExtentResolverRef(two)), FillBackendRef(backend)).error() ==
              Status::invalid_argument,
          "storage too small for two batches per class is refused");
}

} // namespace

int main() {
    run(test_classes_and_exact_slots, "classes_and_exact_slots");
    run(test_resolve_is_all_or_nothing, "resolve_is_all_or_nothing");
    run(test_registration_refusals, "registration_refusals");
    return finish();
}
