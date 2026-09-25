// Zero-hot-path-allocation gate (AGENTS.md sec 1 / STYLE_GUIDE.md), mirroring Sub0MemPage's own
// allocation gate: try_get and a resolve_into that only hits already-Ready rows must not call global
// operator new. Registration and the fetch/codec path (transport submission, codec conversion) are
// administrative/steady-state-but-not-hot and are explicitly allowed to allocate.

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
using sub0tieredcache::test::allocation_count;
using sub0tieredcache::test::check;
using sub0tieredcache::test::finish;
using sub0tieredcache::test::run;
using Backend = sub0mempage::test::FakeBackend;

struct FixedWidthResolver {
    std::uint64_t row_bytes;
    std::uint64_t row_count;
    [[nodiscard]] std::expected<ByteRange, Status> resolve_extent(std::uint64_t row) const noexcept {
        if (row >= row_count) {
            return std::unexpected(Status::out_of_range);
        }
        return ByteRange{row * row_bytes, row_bytes};
    }
};

void test_try_get_and_hit_resolve_never_allocate() {
    Backend backend(16);
    const std::uint64_t row_bytes = 32;
    const std::uint64_t row_count = 8;
    FixedWidthResolver resolver{row_bytes, row_count};
    std::vector<std::byte> output(row_bytes * 4);
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    cfg.source = SourceId{9};
    cfg.source_bytes = row_count * row_bytes;
    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = 4;
    cfg.max_tickets = 4;
    cfg.max_batch_rows = 4;
    auto table = std::move(*Table::create(cfg, FillBackendRef(backend))); // administrative: may allocate

    {
        BackgroundCompleter pump(backend);
        std::vector<RowLease> warm(2);
        check(table->resolve_into(std::array<std::uint64_t, 2>{0, 1}, warm).has_value(), "warm rows 0 and 1");
        warm[0].reset();
        warm[1].reset();

        std::vector<RowLease> reuse(2);
        check(table->resolve_into(std::array<std::uint64_t, 2>{0, 1}, reuse).has_value(), "re-warm to steady state");
        reuse[0].reset();
        reuse[1].reset();
    }

    const std::uint64_t before_try_get = allocation_count();
    for (int i = 0; i < 1000; ++i) {
        auto lease = table->try_get(0);
        check(lease.has_value(), "try_get keeps hitting the resident row");
    }
    check(allocation_count() == before_try_get, "1000 try_get hits performed zero heap allocations");

    const std::uint64_t before_resolve = allocation_count();
    for (int i = 0; i < 1000; ++i) {
        std::array<RowLease, 1> out{};
        auto result = table->resolve_into(std::array<std::uint64_t, 1>{0}, out);
        check(result.has_value(), "resolve_into keeps hitting the resident row");
    }
    check(allocation_count() == before_resolve, "1000 resolve_into hits performed zero heap allocations");
}

} // namespace

int main() {
    run(test_try_get_and_hit_resolve_never_allocate, "try_get_and_hit_resolve_never_allocate");
    return finish();
}
