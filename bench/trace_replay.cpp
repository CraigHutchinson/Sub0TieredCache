// sub0tieredcache-trace-replay: replay a recorded row-access trace against a Table over a real file,
// and measure the cache without the application that produced the trace. See docs/trace-replay.md for
// the extents/trace formats and how to read the report.
//
// Each trace batch is processed the way a real consumer would: prefetch the whole batch, then for each
// row try_get (a resident hit costs nothing) or resolve_into (a miss: its wait is timed), then hold the
// batch's leases through a synthetic compute delay, then release them before the next batch.

#include <sub0mempage/local_file_backend.hpp>
#include <sub0tieredcache/sub0tieredcache.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace sub0tieredcache;
using Clock = std::chrono::steady_clock;

struct Extent {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

struct Options {
    std::string data, extents, trace;
    std::uint64_t budget_mib = 1024;
    std::uint32_t readers = 8;
    double compute_us = 0;          // synthetic compute per batch, after its rows are resident
    std::uint64_t chunk_kib = 0;     // TableConfig::fill_chunk_bytes in KiB (0 = one read per row)
    std::uint64_t limit_batches = 0; // 0 = whole trace
};

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "trace-replay: %s\n", message.c_str());
    std::exit(2);
}

template <class T>
T read_pod(std::ifstream& in, const std::string& what) {
    T value{};
    in.read(reinterpret_cast<char*>(&value), sizeof value);
    if (in.gcount() != static_cast<std::streamsize>(sizeof value)) fail("truncated " + what);
    return value;
}

void expect_magic(std::ifstream& in, const char (&magic)[5], const std::string& path) {
    char got[4]{};
    in.read(got, 4);
    if (in.gcount() != 4 || std::memcmp(got, magic, 4) != 0) fail(path + ": not a " + magic + " file");
    if (read_pod<std::uint32_t>(in, path) != 1) fail(path + ": unsupported version");
}

std::vector<Extent> load_extents(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) fail("cannot open " + path);
    expect_magic(in, "S0RX", path);
    const auto rows = read_pod<std::uint64_t>(in, path);
    std::vector<Extent> extents(static_cast<std::size_t>(rows));
    for (auto& e : extents) {
        e.offset = read_pod<std::uint64_t>(in, path);
        e.length = read_pod<std::uint64_t>(in, path);
    }
    return extents;
}

// Batches flattened: offsets into `rows`, so the replay loop touches no allocation.
struct Trace {
    std::vector<std::uint64_t> rows;
    std::vector<std::size_t> starts; // batch b is rows[starts[b], starts[b + 1])
};

Trace load_trace(const std::string& path, std::uint64_t row_count) {
    std::ifstream in(path, std::ios::binary);
    if (!in) fail("cannot open " + path);
    expect_magic(in, "S0RT", path);
    const auto batches = read_pod<std::uint64_t>(in, path);
    Trace t;
    t.starts.reserve(static_cast<std::size_t>(batches) + 1);
    for (std::uint64_t b = 0; b < batches; ++b) {
        t.starts.push_back(t.rows.size());
        const auto n = read_pod<std::uint32_t>(in, path);
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto row = read_pod<std::uint64_t>(in, path);
            if (row >= row_count) fail(path + ": row id past the extents table");
            t.rows.push_back(row);
        }
    }
    t.starts.push_back(t.rows.size());
    return t;
}

struct Resolver {
    const std::vector<Extent>* extents = nullptr;
    [[nodiscard]] std::expected<RowLocation, Status> resolve_extent(std::uint64_t row) const noexcept {
        if (row >= extents->size()) return std::unexpected(Status::out_of_range);
        const Extent& e = (*extents)[static_cast<std::size_t>(row)];
        return RowLocation{0, sub0mempage::ByteRange{e.offset, e.length}};
    }
};

double percentile(std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0;
    const auto at = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
    return sorted[std::min(at, sorted.size() - 1)];
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) fail("missing value for " + std::string(a));
            return argv[++i];
        };
        if (a == "--data") o.data = next();
        else if (a == "--extents") o.extents = next();
        else if (a == "--trace") o.trace = next();
        else if (a == "--budget-mib") o.budget_mib = std::stoull(next());
        else if (a == "--readers") o.readers = static_cast<std::uint32_t>(std::stoul(next()));
        else if (a == "--compute-us") o.compute_us = std::stod(next());
        else if (a == "--limit-batches") o.limit_batches = std::stoull(next());
        else if (a == "--chunk-kib") o.chunk_kib = std::stoull(next());
        else fail("unknown argument " + std::string(a));
    }
    if (o.data.empty() || o.extents.empty() || o.trace.empty())
        fail("usage: --data FILE --extents FILE --trace FILE [--budget-mib N] [--readers N] [--compute-us X] "
             "[--limit-batches N] [--chunk-kib N]");
    return o;
}

} // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    const auto extents = load_extents(o.extents);
    const auto trace = load_trace(o.trace, extents.size());
    std::uint64_t row_bytes = 0;
    std::uint32_t max_batch = 1;
    for (const auto& e : extents) row_bytes = std::max(row_bytes, e.length);
    const std::size_t batch_count = trace.starts.size() - 1;
    for (std::size_t b = 0; b < batch_count; ++b)
        max_batch = std::max<std::uint32_t>(max_batch, static_cast<std::uint32_t>(trace.starts[b + 1] - trace.starts[b]));
    const std::uint64_t budget_rows = std::min<std::uint64_t>((o.budget_mib << 20) / row_bytes, extents.size());
    if (budget_rows < 2ull * max_batch) fail("budget holds fewer than two batches");

    std::error_code ec;
    const auto file_bytes = std::filesystem::file_size(o.data, ec);
    if (ec) fail("cannot stat " + o.data);
    const std::uint64_t chunk_bytes = o.chunk_kib << 10;
    const auto chunks_per_row = static_cast<std::uint32_t>(
        chunk_bytes && chunk_bytes < row_bytes ? (row_bytes + chunk_bytes - 1) / chunk_bytes : 1);
    auto backend = sub0mempage::LocalFileBackend::create(
        {.workers = o.readers, .queue_capacity = 4 * max_batch * chunks_per_row, .max_sources = 1});
    if (!backend) fail("backend create failed");
    constexpr auto kSource = static_cast<sub0mempage::SourceId>(1);
    if ((*backend)->register_file(kSource, o.data) != sub0mempage::Status::ok) fail("cannot register " + o.data);

    std::vector<std::byte> storage(static_cast<std::size_t>(budget_rows * row_bytes));
    Resolver resolver{&extents};
    const auto sources = single_source(kSource, file_bytes);
    TableConfig cfg{};
    cfg.row_count = extents.size();
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.row_extent = RowExtent::bounded;
    cfg.sources = sources;
    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = storage;
    cfg.budget_rows = static_cast<std::uint32_t>(budget_rows);
    cfg.max_tickets = 4;
    cfg.max_batch_rows = max_batch;
    cfg.fill_chunk_bytes = chunk_bytes;
    auto table = Table::create(cfg, sub0mempage::FillBackendRef(**backend));
    if (!table) fail("table create failed");

    const std::size_t batches = o.limit_batches ? std::min<std::size_t>(batch_count, o.limit_batches) : batch_count;
    std::vector<RowLease> leases(max_batch);
    std::vector<double> waits_us;
    waits_us.reserve(trace.rows.size());
    std::uint64_t accesses = 0, misses = 0, miss_bytes = 0;
    double stall_us = 0, prefetch_us = 0;
    const auto compute = std::chrono::duration<double, std::micro>(o.compute_us);

    const auto t0 = Clock::now();
    for (std::size_t b = 0; b < batches; ++b) {
        const std::span<const std::uint64_t> rows(trace.rows.data() + trace.starts[b], trace.starts[b + 1] - trace.starts[b]);
        for (auto& lease : leases) lease.reset();
        const auto issued = Clock::now();
        (void)(*table)->prefetch(rows); // a hint: per-row admission failures surface in resolve_into below
        prefetch_us += std::chrono::duration<double, std::micro>(Clock::now() - issued).count();
        for (std::size_t i = 0; i < rows.size(); ++i) {
            ++accesses;
            if (auto hit = (*table)->try_get(rows[i])) {
                leases[i] = std::move(*hit);
                continue;
            }
            ++misses;
            miss_bytes += extents[static_cast<std::size_t>(rows[i])].length;
            const auto start = Clock::now();
            const auto resolved = (*table)->resolve_into(rows.subspan(i, 1), std::span(leases).subspan(i, 1));
            const double waited = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
            if (!resolved) fail(std::string("resolve failed at batch ") + std::to_string(b));
            waits_us.push_back(waited);
            stall_us += waited;
        }
        if (o.compute_us > 0) {
            const auto until = Clock::now() + std::chrono::duration_cast<Clock::duration>(compute);
            while (Clock::now() < until) {} // busy, like the compute it stands in for
        }
    }
    const double wall_s = std::chrono::duration<double>(Clock::now() - t0).count();
    for (auto& lease : leases) lease.reset();
    (void)(*table)->drain();

    std::sort(waits_us.begin(), waits_us.end());
    const auto stats = (*table)->stats();
    const double hit_rate = accesses ? 1.0 - static_cast<double>(misses) / static_cast<double>(accesses) : 0;
    std::printf("trace-replay: %zu batches, %llu accesses, budget %llu rows (%.1f GiB), %u readers, compute %.0f us/batch, chunk %llu KiB\n",
                batches, static_cast<unsigned long long>(accesses), static_cast<unsigned long long>(budget_rows),
                static_cast<double>(budget_rows * row_bytes) / (1ull << 30), o.readers, o.compute_us,
                static_cast<unsigned long long>(o.chunk_kib));
    std::printf("  hit rate %.2f%% | misses %llu (%.2f GiB read) | fetches %llu, evictions %llu\n", 100 * hit_rate,
                static_cast<unsigned long long>(misses), static_cast<double>(miss_bytes) / (1ull << 30),
                static_cast<unsigned long long>(stats.fetches), static_cast<unsigned long long>(stats.evictions));
    std::printf("  miss wait us: p50 %.0f  p90 %.0f  p99 %.0f  max %.0f | total stall %.2f s, prefetch %.2f s, of %.2f s wall\n",
                percentile(waits_us, 0.5), percentile(waits_us, 0.9), percentile(waits_us, 0.99),
                waits_us.empty() ? 0.0 : waits_us.back(), stall_us / 1e6, prefetch_us / 1e6, wall_s);
    std::printf("{\"batches\":%zu,\"accesses\":%llu,\"budget_rows\":%llu,\"readers\":%u,\"compute_us\":%.1f,\"chunk_kib\":%llu,"
                "\"hit_rate\":%.6f,\"misses\":%llu,\"miss_bytes\":%llu,\"fetches\":%llu,\"evictions\":%llu,"
                "\"wait_p50_us\":%.1f,\"wait_p90_us\":%.1f,\"wait_p99_us\":%.1f,\"wait_max_us\":%.1f,"
                "\"stall_s\":%.4f,\"prefetch_s\":%.4f,\"wall_s\":%.4f}\n",
                batches, static_cast<unsigned long long>(accesses), static_cast<unsigned long long>(budget_rows), o.readers,
                o.compute_us, static_cast<unsigned long long>(o.chunk_kib), hit_rate, static_cast<unsigned long long>(misses), static_cast<unsigned long long>(miss_bytes),
                static_cast<unsigned long long>(stats.fetches), static_cast<unsigned long long>(stats.evictions),
                percentile(waits_us, 0.5), percentile(waits_us, 0.9), percentile(waits_us, 0.99),
                waits_us.empty() ? 0.0 : waits_us.back(), stall_us / 1e6, prefetch_us / 1e6, wall_s);
    return 0;
}
