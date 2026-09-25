// T3 end-to-end: a Table whose source is served by the local HTTP test range server through
// remote::Mirror + remote::MirrorBackend. Oracle throughout is the known content buffer the test server
// itself was constructed from -- never this project's own transport/cache/codec code.

#include "support/test_http_server.hpp"
#include "test_support.hpp"

#include <sub0tieredcache/local_file_source.hpp>
#include <sub0tieredcache/remote/http_client.hpp>
#include <sub0tieredcache/remote/mirror.hpp>
#include <sub0tieredcache/remote/mirror_backend.hpp>
#include <sub0tieredcache/sub0tieredcache.hpp>

#include <array>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace sub0tieredcache;
using namespace sub0tieredcache::remote;
using sub0mempage::FillBackendRef;
using sub0mempage::SourceId;
using sub0tieredcache::test::check;
using sub0tieredcache::test::finish;
using sub0tieredcache::test::run;

[[nodiscard]] std::vector<std::byte> make_content(std::size_t size) {
    std::vector<std::byte> content(size);
    for (std::size_t i = 0; i < size; ++i) {
        content[i] = static_cast<std::byte>((i * 53 + 5) % 241);
    }
    return content;
}

[[nodiscard]] std::filesystem::path make_temp_dir(const char* label) {
    static std::atomic<unsigned> counter{0};
    auto dir = std::filesystem::temp_directory_path() /
               ("sub0tieredcache-e2e-" + std::string(label) + "-" + std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(dir);
    return dir;
}

[[nodiscard]] Http1RangeTransport::Options fast_transport_options(std::string url) {
    Http1RangeTransport::Options options;
    options.url = std::move(url);
    options.connect_timeout = std::chrono::milliseconds(500);
    options.recv_timeout = std::chrono::milliseconds(500);
    options.max_retries = 3;
    options.retry_backoff_base = std::chrono::milliseconds(2);
    return options;
}

[[nodiscard]] Mirror::Options make_mirror_options(test::TestRangeServer& server, Http1RangeTransport& transport,
                                                  std::filesystem::path dir, std::uint32_t chunk_size,
                                                  std::string etag) {
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

/// Builds a single-shard Table over `mirror` (registered under SourceId{1} in `backend`), addressing
/// `row_count` fixed-width rows in row-major order, per FlatFileResolver's own layout.
[[nodiscard]] std::unique_ptr<Table> make_table(MirrorBackend& backend, Mirror& mirror,
                                                 FlatFileResolver& resolver, std::span<std::byte> output,
                                                 std::uint64_t row_count, std::uint64_t row_bytes,
                                                 std::uint64_t total_bytes) {
    check(backend.register_mirror(SourceId{1}, mirror) == sub0mempage::Status::ok, "the mirror registers");
    TableConfig cfg{};
    cfg.row_count = row_count;
    cfg.source_row_bytes = row_bytes;
    cfg.output_row_bytes = row_bytes;
    cfg.representation = Representation::identity;
    const auto sources = single_source(SourceId{1}, total_bytes);
    cfg.sources = sources;
    cfg.generation = 1;
    cfg.resolve_extent = RowExtentResolverRef(resolver);
    cfg.output_storage = output;
    cfg.budget_rows = static_cast<std::uint32_t>(row_count);
    cfg.max_tickets = 2;
    cfg.max_batch_rows = static_cast<std::uint32_t>(row_count);
    auto table = Table::create(cfg, FillBackendRef(backend));
    return table.has_value() ? std::move(*table) : nullptr;
}

// --- first resolve fetches over the network; a restart over the same cache dir serves from disk -----------

void test_first_resolve_network_then_restart_serves_from_disk() {
    const std::uint64_t row_bytes = 40;
    const std::uint64_t row_count = 5;
    const auto content = make_content(row_count * row_bytes);
    test::TestRangeServer server(content, "\"v1\"");
    const auto dir = make_temp_dir("restart");
    FlatFileResolver resolver(row_count, row_count, row_bytes, row_bytes);

    {
        Http1RangeTransport transport(fast_transport_options(server.url()));
        Mirror mirror(make_mirror_options(server, transport, dir, 64, "\"v1\""));
        check(mirror.valid(), "mirror opens over a fresh cache directory");

        auto backend = std::move(*MirrorBackend::create({.workers = 2, .queue_capacity = 8, .max_sources = 1}));
        std::vector<std::byte> output(row_bytes * row_count);
        auto table = make_table(*backend, mirror, resolver, output, row_count, row_bytes, content.size());
        check(table != nullptr, "the table registers over the remote mirror");

        check(server.request_count() == 0, "nothing has hit the network yet");
        const std::array<std::uint64_t, 3> rows{0, 2, 4};
        std::vector<RowLease> out(rows.size());
        check(table->resolve_into(rows, out).has_value(), "the first resolve succeeds");
        check(server.request_count() > 0, "the first resolve genuinely went over the network");
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto expected = std::span(content).subspan(rows[i] * row_bytes, row_bytes);
            check(std::equal(expected.begin(), expected.end(), out[i].bytes().begin()),
                  "the network-fetched row matches the known source content");
        }
        (void)table->drain();
        // table, backend and mirror all destruct here, in that order -- Mirror has published every
        // fetched chunk to `dir` by now (Mirror::read only returns success after a successful publish).
    }

    {
        // A brand-new Mirror/MirrorBackend/Table over the SAME directory: nothing above is reused, only
        // the on-disk cache is. request_count() only ever grows, so capturing it now and comparing after
        // proves this second pass adds no further network requests.
        const auto requests_before_restart = server.request_count();
        Http1RangeTransport transport(fast_transport_options(server.url()));
        Mirror mirror(make_mirror_options(server, transport, dir, 64, "\"v1\""));
        check(mirror.valid(), "the restarted mirror reopens the same cache directory");

        auto backend = std::move(*MirrorBackend::create({.workers = 2, .queue_capacity = 8, .max_sources = 1}));
        std::vector<std::byte> output(row_bytes * row_count);
        auto table = make_table(*backend, mirror, resolver, output, row_count, row_bytes, content.size());
        check(table != nullptr, "the restarted table registers");

        const std::array<std::uint64_t, 3> rows{0, 2, 4};
        std::vector<RowLease> out(rows.size());
        check(table->resolve_into(rows, out).has_value(), "the restarted resolve succeeds from disk");
        check(server.request_count() == requests_before_restart,
              "the restart served entirely from the on-disk cache -- zero further network requests");
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto expected = std::span(content).subspan(rows[i] * row_bytes, row_bytes);
            check(std::equal(expected.begin(), expected.end(), out[i].bytes().begin()),
                  "the disk-served row still matches the known source content");
        }
        (void)table->drain();
    }
}

// --- a validator change surfaces as a failed row, not stale bytes -----------------------------------------

void test_validator_change_fails_explicitly_not_stale() {
    const std::uint64_t row_bytes = 32;
    const std::uint64_t row_count = 4;
    const std::uint32_t chunk_size = 64; // rows 0-1 -> chunk 0; rows 2-3 -> chunk 1 (a distinct chunk)
    const auto content = make_content(row_count * row_bytes);
    test::TestRangeServer server(content, "\"v1\"");
    const auto dir = make_temp_dir("validator-change");
    FlatFileResolver resolver(row_count, row_count, row_bytes, row_bytes);

    {
        // Warm the disk cache under the original validator.
        Http1RangeTransport transport(fast_transport_options(server.url()));
        Mirror mirror(make_mirror_options(server, transport, dir, chunk_size, "\"v1\""));
        auto backend = std::move(*MirrorBackend::create({.workers = 2, .queue_capacity = 8, .max_sources = 1}));
        std::vector<std::byte> output(row_bytes * row_count);
        auto table = make_table(*backend, mirror, resolver, output, row_count, row_bytes, content.size());
        check(table != nullptr, "the warm-up table registers");
        std::array<RowLease, 1> out{};
        check(table->resolve_into(std::array<std::uint64_t, 1>{1}, out).has_value(), "row 1 warms the disk cache");
        (void)table->drain();
    }

    // The server's version changes (a real new ETag, e.g. the upstream resource was updated).
    server.set_etag("\"v2\"");

    // A Mirror constructed with the OLD ETag as its expected validator, against a server now reporting
    // "v2": any fetch this Mirror attempts detects the mismatch (Status::source_changed) rather than
    // trusting the (now-stale) locally cached "v1" chunk it never revalidates on its own.
    Http1RangeTransport transport(fast_transport_options(server.url()));
    Mirror mirror(make_mirror_options(server, transport, dir, chunk_size, "\"v1\""));
    auto backend = std::move(*MirrorBackend::create({.workers = 2, .queue_capacity = 8, .max_sources = 1}));
    std::vector<std::byte> output(row_bytes * row_count);
    auto table = make_table(*backend, mirror, resolver, output, row_count, row_bytes, content.size());
    check(table != nullptr, "the post-change table registers");

    // Row 2 lives in chunk 1, which the warm-up phase never touched (it only read row 1, in chunk 0) --
    // so this read must go over HTTP against the "v1"-expecting Mirror, which now detects the server's
    // "v2" mismatch, rather than silently serving anything already on disk from a different chunk.
    std::array<RowLease, 1> out{};
    auto result = table->resolve_into(std::array<std::uint64_t, 1>{2}, out);
    check(!result.has_value(), "a validator mismatch on fetch surfaces as an explicit row failure");
    check(!out[0].is_held(), "nothing is published on a validator mismatch -- never stale bytes (R14)");
    check(table->stats().failed == 1, "the failure is observable via stats()");
    (void)table->drain();
}

} // namespace

int main() {
    run(test_first_resolve_network_then_restart_serves_from_disk, "first_resolve_network_then_restart_serves_from_disk");
    run(test_validator_change_fails_explicitly_not_stale, "validator_change_fails_explicitly_not_stale");
    return finish();
}
