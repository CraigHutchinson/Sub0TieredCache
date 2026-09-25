// T1: real files through sub0mempage::LocalFileBackend (docs/integration-plan.md's T1 acceptance row).
// Oracle throughout is a direct std::ifstream slice of the file -- never this project's own transport
// or codec code (the cross-project "no copied implementation becomes its own correctness oracle" rule).

#include "test_support.hpp"

#include <sub0tieredcache/local_file_source.hpp>
#include <sub0tieredcache/sub0tieredcache.hpp>

#include <sub0mempage/local_file_backend.hpp>
#include <sub0mempage/testing/fake_backend.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

namespace {

using namespace sub0tieredcache;
using sub0mempage::ByteRange;
using sub0mempage::FillBackendRef;
using sub0mempage::LocalFileBackend;
using sub0mempage::LocalFileBackendConfig;
using sub0mempage::SourceId;
using sub0tieredcache::test::check;
using sub0tieredcache::test::finish;
using sub0tieredcache::test::run;
using FakeBackend = sub0mempage::test::FakeBackend;

/// A fresh, empty temp directory, cleaned up on scope exit. Named from a process-wide counter plus a
/// random_device draw so two test binaries (or two runs) never collide on a shared /tmp.
class TempDir {
public:
    TempDir() {
        static std::atomic<std::uint64_t> counter{0};
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("sub0tieredcache_t1_" + std::to_string(rd()) + "_" +
                 std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] std::filesystem::path file(const char* name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void append_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::vector<std::byte> source_bytes(std::uint64_t count) {
    std::vector<std::byte> bytes(count);
    for (std::uint64_t i = 0; i < count; ++i) {
        bytes[i] = sub0mempage::test::source_byte(i);
    }
    return bytes;
}

/// The independent oracle every real-file result is checked against: a direct synchronous slice of the
/// file via std::ifstream, never this project's own LocalFileBackend/TransferSet/codec path.
[[nodiscard]] std::vector<std::byte> read_file_range_oracle(const std::filesystem::path& path, std::uint64_t offset,
                                                            std::uint64_t length) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::byte> bytes(length);
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
    return bytes;
}

[[nodiscard]] bool bytes_equal(std::span<const std::byte> a, std::span<const std::byte> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// --- 1: identical output to the same table run over the fake backend -------------------------------------

void test_identical_output_to_fake_backend() {
    TempDir dir;
    const std::uint64_t row_bytes = 16;
    const std::uint64_t row_count = 10;
    const auto content = source_bytes(row_count * row_bytes);
    const auto path = dir.file("flat.bin");
    write_file(path, content);

    LocalFileBackendConfig backend_cfg{.workers = 2, .queue_capacity = 16, .max_sources = 2};
    auto file_backend = std::move(*LocalFileBackend::create(backend_cfg));
    check(file_backend->register_file(SourceId{1}, path) == sub0mempage::Status::ok, "file registers");

    FakeBackend fake_backend(16);

    FlatFileResolver resolver(row_count, row_count, row_bytes, row_bytes);
    std::vector<std::byte> output_file(row_bytes * 4);
    std::vector<std::byte> output_fake(row_bytes * 4);

    auto make_cfg = [&](std::span<std::byte> output) {
        TableConfig cfg{};
        cfg.row_count = row_count;
        cfg.source_row_bytes = row_bytes;
        cfg.output_row_bytes = row_bytes;
        cfg.representation = Representation::identity;
        cfg.generation = 1;
        cfg.resolve_extent = RowExtentResolverRef(resolver);
        cfg.output_storage = output;
        cfg.budget_rows = 4;
        cfg.max_tickets = 4;
        cfg.max_batch_rows = 4;
        return cfg;
    };

    auto cfg_file = make_cfg(output_file);
    const auto cfg_file_sources = single_source(SourceId{1}, row_count * row_bytes);
    cfg_file.sources = cfg_file_sources;
    auto table_file = std::move(*Table::create(cfg_file, FillBackendRef(*file_backend)));

    auto cfg_fake = make_cfg(output_fake);
    const auto cfg_fake_sources = single_source(SourceId{2}, row_count * row_bytes);
    cfg_fake.sources = cfg_fake_sources;
    auto table_fake = std::move(*Table::create(cfg_fake, FillBackendRef(fake_backend)));

    test::BackgroundCompleter pump(fake_backend);
    const std::array<std::uint64_t, 4> rows{3, 1, 3, 7};
    std::vector<RowLease> out_file(rows.size());
    std::vector<RowLease> out_fake(rows.size());
    check(table_file->resolve_into(rows, out_file).has_value(), "the real-file table resolves the batch");
    check(table_fake->resolve_into(rows, out_fake).has_value(), "the fake-backend table resolves the same batch");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        check(bytes_equal(out_file[i].bytes(), out_fake[i].bytes()), "real-file and fake-backend rows match byte-for-byte");
        check(bytes_equal(out_file[i].bytes(), read_file_range_oracle(path, rows[i] * row_bytes, row_bytes)),
              "the real-file row also matches the direct ifstream oracle");
    }
    (void)table_file->drain();
    (void)table_fake->drain();
}

// --- 2: a tiny budget forces eviction across a long access sequence ---------------------------------------

void test_tiny_budget_forces_eviction_over_long_sequence() {
    TempDir dir;
    const std::uint64_t row_bytes = 12;
    const std::uint64_t row_count = 40;
    const auto content = source_bytes(row_count * row_bytes);
    const auto path = dir.file("flat.bin");
    write_file(path, content);

    LocalFileBackendConfig backend_cfg{.workers = 2, .queue_capacity = 8, .max_sources = 1};
    auto backend = std::move(*LocalFileBackend::create(backend_cfg));
    check(backend->register_file(SourceId{1}, path) == sub0mempage::Status::ok, "file registers");

    FlatFileResolver resolver(row_count, row_count, row_bytes, row_bytes);
    std::vector<std::byte> output(row_bytes * 3); // budget_rows == 3, far smaller than row_count
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    const auto cfg_sources = single_source(SourceId{1}, row_count * row_bytes);
    cfg.sources = cfg_sources;
    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = 3;
    cfg.max_tickets = 2;
    cfg.max_batch_rows = 1;
    auto table = std::move(*Table::create(cfg, FillBackendRef(*backend)));

    bool all_ok = true;
    std::mt19937 rng(12345);
    std::uniform_int_distribution<std::uint64_t> pick(0, row_count - 1);
    for (int i = 0; i < 200; ++i) {
        const std::uint64_t row = pick(rng);
        std::array<RowLease, 1> out{};
        auto result = table->resolve_into(std::array<std::uint64_t, 1>{row}, out);
        if (!result.has_value() || !bytes_equal(out[0].bytes(), read_file_range_oracle(path, row * row_bytes, row_bytes))) {
            all_ok = false;
            break;
        }
    }
    check(all_ok, "200 resolves over a 40-row file with a 3-row budget all matched the oracle (forcing evictions)");
    check(table->stats().evictions > 0, "the tiny budget genuinely forced evictions, not just cache hits");
    (void)table->drain();
}

// --- 3: unaligned offsets straddling 4 KiB boundaries, row width not a multiple of 8 -----------------------

void test_unaligned_rows_straddle_4kib_boundary() {
    TempDir dir;
    const std::uint64_t row_bytes = 11; // deliberately not a multiple of 8
    const std::uint64_t row_count = 4000; // rows span past many 4096-byte boundaries
    const auto content = source_bytes(row_count * row_bytes);
    const auto path = dir.file("flat.bin");
    write_file(path, content);

    LocalFileBackendConfig backend_cfg{.workers = 2, .queue_capacity = 8, .max_sources = 1};
    auto backend = std::move(*LocalFileBackend::create(backend_cfg));
    check(backend->register_file(SourceId{1}, path) == sub0mempage::Status::ok, "file registers");

    FlatFileResolver resolver(row_count, row_count, row_bytes, row_bytes);
    std::vector<std::byte> output(row_bytes * 4);
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    const auto cfg_sources = single_source(SourceId{1}, row_count * row_bytes);
    cfg.sources = cfg_sources;
    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = 4;
    cfg.max_tickets = 2;
    cfg.max_batch_rows = 4;
    auto table = std::move(*Table::create(cfg, FillBackendRef(*backend)));

    // Pick rows whose byte range provably straddles a 4096 boundary.
    std::vector<std::uint64_t> straddling_rows;
    for (std::uint64_t row = 0; row < row_count && straddling_rows.size() < 4; ++row) {
        const std::uint64_t begin = row * row_bytes;
        const std::uint64_t end = begin + row_bytes;
        if (begin / 4096 != (end - 1) / 4096) {
            straddling_rows.push_back(row);
        }
    }
    check(straddling_rows.size() == 4, "found rows whose range genuinely straddles a 4096-byte boundary");
    std::vector<RowLease> out(straddling_rows.size());
    check(table->resolve_into(straddling_rows, out).has_value(), "the straddling rows resolve");
    for (std::size_t i = 0; i < straddling_rows.size(); ++i) {
        check(bytes_equal(out[i].bytes(), read_file_range_oracle(path, straddling_rows[i] * row_bytes, row_bytes)),
              "a boundary-straddling, non-8-aligned row matches the oracle exactly");
    }
    (void)table->drain();
}

// --- 4: duplicates and order in resolve_into ---------------------------------------------------------------

void test_duplicates_and_order_on_real_file() {
    TempDir dir;
    const std::uint64_t row_bytes = 9;
    const std::uint64_t row_count = 20;
    const auto content = source_bytes(row_count * row_bytes);
    const auto path = dir.file("flat.bin");
    write_file(path, content);

    LocalFileBackendConfig backend_cfg{.workers = 2, .queue_capacity = 8, .max_sources = 1};
    auto backend = std::move(*LocalFileBackend::create(backend_cfg));
    check(backend->register_file(SourceId{1}, path) == sub0mempage::Status::ok, "file registers");

    FlatFileResolver resolver(row_count, row_count, row_bytes, row_bytes);
    std::vector<std::byte> output(row_bytes * 4);
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    const auto cfg_sources = single_source(SourceId{1}, row_count * row_bytes);
    cfg.sources = cfg_sources;
    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = 4;
    cfg.max_tickets = 2;
    cfg.max_batch_rows = 4;
    auto table = std::move(*Table::create(cfg, FillBackendRef(*backend)));

    const std::array<std::uint64_t, 4> rows{5, 2, 5, 2};
    std::vector<RowLease> out(rows.size());
    check(table->resolve_into(rows, out).has_value(), "a duplicate/reordered batch resolves");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        check(out[i].row_index() == rows[i], "lease i names request-order row i");
        check(bytes_equal(out[i].bytes(), read_file_range_oracle(path, rows[i] * row_bytes, row_bytes)),
              "each lease's bytes match the oracle");
    }
    check(table->stats().fetches == 2, "the two distinct rows caused exactly two fetches despite four requests");
    (void)table->drain();
}

// --- 5: bf16 -> f32 from a real file, bit-exact -----------------------------------------------------------

void test_bf16_to_f32_from_real_file() {
    TempDir dir;
    const std::uint64_t elements = 6;
    const std::uint64_t source_row_bytes = elements * 2;
    const std::uint64_t output_row_bytes = elements * 4;
    const std::uint64_t row_count = 3;
    const auto content = source_bytes(row_count * source_row_bytes);
    const auto path = dir.file("bf16.bin");
    write_file(path, content);

    LocalFileBackendConfig backend_cfg{.workers = 2, .queue_capacity = 8, .max_sources = 1};
    auto backend = std::move(*LocalFileBackend::create(backend_cfg));
    check(backend->register_file(SourceId{1}, path) == sub0mempage::Status::ok, "file registers");

    FlatFileResolver resolver(row_count, row_count, source_row_bytes, source_row_bytes);
    std::vector<std::byte> output(output_row_bytes * 2);
    std::vector<std::byte> scratch(source_row_bytes * 2);
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = source_row_bytes;
    cfg.output_row_bytes = output_row_bytes;
    cfg.representation = Representation::bf16_to_f32;
    const auto cfg_sources = single_source(SourceId{1}, row_count * source_row_bytes);
    cfg.sources = cfg_sources;
    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = 2;
    cfg.scratch_storage = scratch;
    cfg.scratch_rows = 2;
    cfg.max_tickets = 2;
    cfg.max_batch_rows = 2;
    auto table = std::move(*Table::create(cfg, FillBackendRef(*backend)));

    std::array<RowLease, 1> out{};
    check(table->resolve_into(std::array<std::uint64_t, 1>{1}, out).has_value(), "row 1 converts from the real file");
    check(table->stats().conversions == 1, "the conversion is counted");

    const auto raw = read_file_range_oracle(path, 1 * source_row_bytes, source_row_bytes);
    for (std::uint64_t i = 0; i < elements; ++i) {
        std::uint16_t bits;
        std::memcpy(&bits, raw.data() + 2 * i, 2);
        const std::uint32_t expected_bits = static_cast<std::uint32_t>(bits) << 16; // independent bit-shift oracle
        std::uint32_t actual_bits;
        std::memcpy(&actual_bits, out[0].bytes().data() + 4 * i, 4);
        check(actual_bits == expected_bits, "bf16->f32 from a real file is bit-exact against the independent oracle");
    }
    (void)table->drain();
}

// --- 6: two shards with an interleaved row mapping ----------------------------------------------------------

/// Even row_index -> shard 0, odd -> shard 1; local row = row_index / 2 in either shard. A genuinely
/// interleaved mapping (not FlatFileResolver's contiguous-block-per-shard shape), written directly here
/// as the small caller-side adapter AGENTS.md sec 2 asks for -- not something the core needs to know.
struct InterleavedResolver {
    std::uint64_t row_bytes;
    std::uint64_t row_count;
    [[nodiscard]] std::expected<RowLocation, Status> resolve_extent(std::uint64_t row) const noexcept {
        if (row >= row_count) {
            return std::unexpected(Status::out_of_range);
        }
        const std::uint32_t shard = static_cast<std::uint32_t>(row % 2);
        const std::uint64_t local_row = row / 2;
        return RowLocation{shard, ByteRange{local_row * row_bytes, row_bytes}};
    }
};

void test_two_shards_interleaved_mapping() {
    TempDir dir;
    const std::uint64_t row_bytes = 10;
    const std::uint64_t row_count = 12; // 6 even rows in shard 0, 6 odd rows in shard 1
    const auto shard0_content = source_bytes(6 * row_bytes);
    auto shard1_content = source_bytes(6 * row_bytes);
    for (auto& b : shard1_content) {
        b ^= std::byte{0xFF}; // deliberately distinct from shard 0's content so a mix-up would be caught
    }
    const auto path0 = dir.file("shard0.bin");
    const auto path1 = dir.file("shard1.bin");
    write_file(path0, shard0_content);
    write_file(path1, shard1_content);

    LocalFileBackendConfig backend_cfg{.workers = 2, .queue_capacity = 8, .max_sources = 2};
    auto backend = std::move(*LocalFileBackend::create(backend_cfg));
    check(backend->register_file(SourceId{1}, path0) == sub0mempage::Status::ok, "shard 0 registers");
    check(backend->register_file(SourceId{2}, path1) == sub0mempage::Status::ok, "shard 1 registers");

    InterleavedResolver resolver{row_bytes, row_count};
    std::vector<std::byte> output(row_bytes * 4);
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    const std::array<ShardSource, 2> sources{ShardSource{SourceId{1}, 6 * row_bytes}, ShardSource{SourceId{2}, 6 * row_bytes}};
    cfg.sources = sources;
    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = 4;
    cfg.max_tickets = 2;
    cfg.max_batch_rows = 4;
    auto table = std::move(*Table::create(cfg, FillBackendRef(*backend)));

    const std::array<std::uint64_t, 4> rows{0, 1, 4, 5}; // rows 0,4 -> shard 0; rows 1,5 -> shard 1
    std::vector<RowLease> out(rows.size());
    check(table->resolve_into(rows, out).has_value(), "the interleaved batch resolves across both shards");
    check(bytes_equal(out[0].bytes(), read_file_range_oracle(path0, 0, row_bytes)), "row 0 reads shard 0's oracle bytes");
    check(bytes_equal(out[1].bytes(), read_file_range_oracle(path1, 0, row_bytes)), "row 1 reads shard 1's oracle bytes");
    check(bytes_equal(out[2].bytes(), read_file_range_oracle(path0, 2 * row_bytes, row_bytes)), "row 4 reads shard 0's oracle bytes");
    check(bytes_equal(out[3].bytes(), read_file_range_oracle(path1, 2 * row_bytes, row_bytes)), "row 5 reads shard 1's oracle bytes");
    (void)table->drain();
}

// --- 7: a short shard fails explicitly, is never published, and a retry doesn't see stale state -----------

void test_short_shard_fails_then_retry_sees_fresh_state() {
    TempDir dir;
    const std::uint64_t row_bytes = 16;
    const std::uint64_t row_count = 4;
    const std::uint64_t full_bytes = row_count * row_bytes;
    const auto path = dir.file("short.bin");
    // Constructed already-short (per the task brief's Windows note: never truncate an open file) --
    // only the first two rows' worth of bytes exist when this is registered.
    write_file(path, source_bytes(2 * row_bytes));

    LocalFileBackendConfig backend_cfg{.workers = 2, .queue_capacity = 8, .max_sources = 1};
    auto backend = std::move(*LocalFileBackend::create(backend_cfg));
    check(backend->register_file(SourceId{1}, path) == sub0mempage::Status::ok, "the short file still registers");

    FlatFileResolver resolver(row_count, row_count, row_bytes, row_bytes);
    std::vector<std::byte> output(row_bytes * 2);
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    // The registered extent claims the FULL expected size, not the file's actual (short) size --
    // TransferSet's own bounds check permits the read; the real backend then discovers the true EOF.
    const auto cfg_sources = single_source(SourceId{1}, full_bytes);
    cfg.sources = cfg_sources;
    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = 2;
    cfg.max_tickets = 2;
    cfg.max_batch_rows = 1;
    auto table = std::move(*Table::create(cfg, FillBackendRef(*backend)));

    std::array<RowLease, 1> out{};
    auto result = table->resolve_into(std::array<std::uint64_t, 1>{3}, out); // row 3 lies entirely past real EOF
    check(!result.has_value() && result.error() == Status::short_read,
          "a row past the real (short) end of file fails explicitly with short_read, never publishing partial bytes");
    check(!out[0].is_held(), "nothing is published on the short read (R14)");
    check(table->stats().failed == 1, "the failure is observable via stats()");

    // Extend the file (append-only growth, portable on Windows too) so the same row is now genuinely
    // present, then retry: it must NOT see the earlier short_read as stale state.
    append_file(path, source_bytes(2 * row_bytes)); // fills rows 2 and 3 for real now (content differs from
                                                     // the true oracle-by-offset only in absolute file
                                                     // position, which is fine: we re-read via the oracle too)
    std::array<RowLease, 1> retry{};
    auto retried = table->resolve_into(std::array<std::uint64_t, 1>{3}, retry);
    check(retried.has_value(), "R14: a retry after the file is extended succeeds -- not stuck on the stale failure");
    check(bytes_equal(retry[0].bytes(), read_file_range_oracle(path, 3 * row_bytes, row_bytes)),
          "the retried row's bytes match a fresh read of the now-complete file");
    check(table->stats().fetches == 2, "the retry was a genuine second fetch, not a cached failure");
    (void)table->drain();
}

// --- 8: 24 threads resolving the same rows concurrently, fetches == distinct row count (R5) ----------------

void test_concurrent_real_file_coalescing() {
    TempDir dir;
    const std::uint64_t row_bytes = 8;
    const std::uint64_t row_count = 6;
    const auto content = source_bytes(row_count * row_bytes);
    const auto path = dir.file("flat.bin");
    write_file(path, content);

    LocalFileBackendConfig backend_cfg{.workers = 4, .queue_capacity = 64, .max_sources = 1};
    auto backend = std::move(*LocalFileBackend::create(backend_cfg));
    check(backend->register_file(SourceId{1}, path) == sub0mempage::Status::ok, "file registers");

    FlatFileResolver resolver(row_count, row_count, row_bytes, row_bytes);
    std::vector<std::byte> output(row_bytes * row_count);
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    const auto cfg_sources = single_source(SourceId{1}, row_count * row_bytes);
    cfg.sources = cfg_sources;
    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = static_cast<std::uint32_t>(row_count);
    cfg.max_tickets = 24;
    cfg.max_batch_rows = 1;
    auto table = std::move(*Table::create(cfg, FillBackendRef(*backend)));

    constexpr int thread_count = 24;
    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            const std::uint64_t row = static_cast<std::uint64_t>(t) % row_count;
            for (int i = 0; i < 20; ++i) {
                std::array<RowLease, 1> out{};
                auto result = table->resolve_into(std::array<std::uint64_t, 1>{row}, out);
                if (!result.has_value() ||
                    !bytes_equal(out[0].bytes(), read_file_range_oracle(path, row * row_bytes, row_bytes))) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    check(mismatches.load() == 0, "every concurrent resolve over the real file matched the oracle");
    check(table->stats().fetches == row_count, "exactly one fetch per distinct row despite 24 threads x 20 requests (R5)");
    (void)table->drain();
}

// --- 9: zero allocations on try_get and resolve_into hits, over a real file --------------------------------

void test_zero_allocations_on_real_file_hits() {
    TempDir dir;
    const std::uint64_t row_bytes = 8;
    const std::uint64_t row_count = 6;
    const auto content = source_bytes(row_count * row_bytes);
    const auto path = dir.file("flat.bin");
    write_file(path, content);

    LocalFileBackendConfig backend_cfg{.workers = 2, .queue_capacity = 8, .max_sources = 1};
    auto backend = std::move(*LocalFileBackend::create(backend_cfg)); // administrative: may allocate
    check(backend->register_file(SourceId{1}, path) == sub0mempage::Status::ok, "file registers");

    FlatFileResolver resolver(row_count, row_count, row_bytes, row_bytes);
    std::vector<std::byte> output(row_bytes * 4);
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    const auto cfg_sources = single_source(SourceId{1}, row_count * row_bytes);
    cfg.sources = cfg_sources;
    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = 4;
    cfg.max_tickets = 2;
    cfg.max_batch_rows = 2;
    auto table = std::move(*Table::create(cfg, FillBackendRef(*backend))); // administrative

    std::array<RowLease, 2> warm{};
    check(table->resolve_into(std::array<std::uint64_t, 2>{0, 1}, warm).has_value(), "warm rows 0 and 1");
    warm[0].reset();
    warm[1].reset();

    const std::uint64_t before_try_get = test::allocation_count();
    for (int i = 0; i < 500; ++i) {
        auto lease = table->try_get(0);
        check(lease.has_value(), "try_get keeps hitting the resident row");
    }
    check(test::allocation_count() == before_try_get, "500 try_get hits over a real-file table allocated nothing");

    const std::uint64_t before_resolve = test::allocation_count();
    for (int i = 0; i < 500; ++i) {
        std::array<RowLease, 1> out{};
        auto result = table->resolve_into(std::array<std::uint64_t, 1>{0}, out);
        check(result.has_value(), "resolve_into keeps hitting the resident row");
    }
    check(test::allocation_count() == before_resolve, "500 resolve_into hits over a real-file table allocated nothing");
    (void)table->drain();
}

// --- 10: invalidate to a new file generation while an old lease is held ------------------------------------

void test_invalidate_to_new_file_generation_with_old_lease_held() {
    TempDir dir;
    const std::uint64_t row_bytes = 8;
    const std::uint64_t row_count = 4;
    const auto old_content = source_bytes(row_count * row_bytes);
    std::vector<std::byte> new_content(old_content);
    for (auto& b : new_content) {
        b ^= std::byte{0xA5}; // genuinely different bytes so old-vs-new is unambiguous
    }
    const auto old_path = dir.file("gen1.bin");
    const auto new_path = dir.file("gen2.bin");
    write_file(old_path, old_content);
    write_file(new_path, new_content);

    LocalFileBackendConfig backend_cfg{.workers = 2, .queue_capacity = 8, .max_sources = 2};
    auto backend = std::move(*LocalFileBackend::create(backend_cfg));
    check(backend->register_file(SourceId{1}, old_path) == sub0mempage::Status::ok, "generation 1 file registers");
    check(backend->register_file(SourceId{2}, new_path) == sub0mempage::Status::ok, "generation 2 file registers");

    FlatFileResolver resolver(row_count, row_count, row_bytes, row_bytes);
    std::vector<std::byte> output(row_bytes * 2);
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    const auto cfg_sources = single_source(SourceId{1}, row_count * row_bytes);
    cfg.sources = cfg_sources;
    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = 2;
    cfg.max_tickets = 2;
    cfg.max_batch_rows = 2;
    auto table = std::move(*Table::create(cfg, FillBackendRef(*backend)));

    std::array<RowLease, 1> gen1{};
    check(table->resolve_into(std::array<std::uint64_t, 1>{2}, gen1).has_value(), "row 2 resolves under generation 1");
    check(bytes_equal(gen1[0].bytes(), read_file_range_oracle(old_path, 2 * row_bytes, row_bytes)),
          "generation 1's bytes match the old file's oracle");

    check(table->invalidate(2, single_source(SourceId{2}, row_count * row_bytes)) == Status::ok,
          "invalidate succeeds while the generation-1 lease is still held");

    std::array<RowLease, 1> gen2{};
    check(table->resolve_into(std::array<std::uint64_t, 1>{2}, gen2).has_value(), "row 2 resolves fresh under generation 2");
    check(bytes_equal(gen2[0].bytes(), read_file_range_oracle(new_path, 2 * row_bytes, row_bytes)),
          "generation 2's bytes match the NEW file's oracle");
    check(bytes_equal(gen1[0].bytes(), read_file_range_oracle(old_path, 2 * row_bytes, row_bytes)),
          "the generation-1 lease's bytes are completely unaffected by the transition");
    (void)table->drain();
}

} // namespace

int main() {
    run(test_identical_output_to_fake_backend, "identical_output_to_fake_backend");
    run(test_tiny_budget_forces_eviction_over_long_sequence, "tiny_budget_forces_eviction_over_long_sequence");
    run(test_unaligned_rows_straddle_4kib_boundary, "unaligned_rows_straddle_4kib_boundary");
    run(test_duplicates_and_order_on_real_file, "duplicates_and_order_on_real_file");
    run(test_bf16_to_f32_from_real_file, "bf16_to_f32_from_real_file");
    run(test_two_shards_interleaved_mapping, "two_shards_interleaved_mapping");
    run(test_short_shard_fails_then_retry_sees_fresh_state, "short_shard_fails_then_retry_sees_fresh_state");
    run(test_concurrent_real_file_coalescing, "concurrent_real_file_coalescing");
    run(test_zero_allocations_on_real_file_hits, "zero_allocations_on_real_file_hits");
    run(test_invalidate_to_new_file_generation_with_old_lease_held, "invalidate_to_new_file_generation_with_old_lease_held");
    return finish();
}
