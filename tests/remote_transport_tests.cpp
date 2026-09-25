/** @file remote_transport_tests.cpp
 *  @brief Http1RangeTransport against the local fault-injecting fixture server (support/test_http_server.hpp).
 *         Covers integration-plan.md's failure matrix: correct 206; 200 full body; wrong
 *         Content-Range; short/truncated body; connection reset mid-body; 503 then success (retry
 *         counted); ETag change (source_changed); total-size mismatch.
 */

#include <cstddef>
#include <vector>

#include "sub0tieredcache/remote/http_client.hpp"
#include "sub0tieredcache/remote/transport.hpp"
#include "support/test_http_server.hpp"
#include "test_support.hpp"

using namespace sub0tieredcache::remote;
using namespace sub0tieredcache::test;

namespace {

std::vector<std::byte> make_content(std::size_t size) {
    std::vector<std::byte> content(size);
    for (std::size_t i = 0; i < size; ++i) {
        content[i] = static_cast<std::byte>((i * 37 + 11) % 251);
    }
    return content;
}

Http1RangeTransport::Options fast_options(std::string url) {
    Http1RangeTransport::Options options;
    options.url = std::move(url);
    options.connect_timeout = std::chrono::milliseconds(500);
    options.recv_timeout = std::chrono::milliseconds(500);
    options.max_retries = 3;
    options.retry_backoff_base = std::chrono::milliseconds(2);
    return options;
}

void test_correct_206() {
    auto content = make_content(4096);
    sub0tieredcache::test::TestRangeServer server(content, "\"abc123\"");
    Http1RangeTransport transport(fast_options(server.url()));

    std::vector<std::byte> dest(128);
    RangeFetchRequest request;
    request.offset = 200;
    request.length = 128;
    request.destination = dest;
    request.expected.etag = "\"abc123\"";
    request.expected.total_size = server.total_size();

    const auto result = transport.fetch_range(request);
    check(result.status == Status::ok, "correct 206 -> ok");
    check(result.bytes == 128, "correct 206 -> full byte count");
    check(result.retries == 0, "correct 206 -> no retries needed");
    check(result.observed.etag == "\"abc123\"", "correct 206 -> observed etag captured");
    bool matches = true;
    for (std::size_t i = 0; i < dest.size(); ++i) {
        if (dest[i] != content[200 + i]) {
            matches = false;
            break;
        }
    }
    check(matches, "correct 206 -> bytes match the source slice exactly");
}

void test_full_body_200_rejected() {
    auto content = make_content(1024);
    sub0tieredcache::test::TestRangeServer server(content, "\"etag\"");
    server.push_behavior(sub0tieredcache::test::TestRangeServer::Behavior::full_body_200);
    Http1RangeTransport transport(fast_options(server.url()));

    std::vector<std::byte> dest(64);
    RangeFetchRequest request;
    request.offset = 0;
    request.length = 64;
    request.destination = dest;
    request.expected.etag = "\"etag\"";
    request.expected.total_size = server.total_size();

    const auto result = transport.fetch_range(request);
    check(result.status == Status::unexpected_status, "200 full body to a range request is rejected");
    check(result.retries == 0, "200 full body is not retried (not transient)");
}

void test_bad_content_range() {
    auto content = make_content(1024);
    sub0tieredcache::test::TestRangeServer server(content, "\"etag\"");
    server.push_behavior(sub0tieredcache::test::TestRangeServer::Behavior::bad_content_range);
    Http1RangeTransport transport(fast_options(server.url()));

    std::vector<std::byte> dest(64);
    RangeFetchRequest request;
    request.offset = 100;
    request.length = 64;
    request.destination = dest;
    request.expected.etag = "\"etag\"";
    request.expected.total_size = server.total_size();

    const auto result = transport.fetch_range(request);
    check(result.status == Status::bad_content_range, "mismatched Content-Range is rejected");
}

void test_truncated_body() {
    auto content = make_content(1024);
    sub0tieredcache::test::TestRangeServer server(content, "\"etag\"");
    server.push_behavior(sub0tieredcache::test::TestRangeServer::Behavior::truncated_body);
    // truncated is a transient status (a retry against the same server usually recovers, and the
    // 503-then-success test above covers that retry path) -- zero retries here isolates the single
    // attempt's own status instead of observing a masking, successful retry.
    Http1RangeTransport::Options options = fast_options(server.url());
    options.max_retries = 0;
    Http1RangeTransport transport(options);

    std::vector<std::byte> dest(200);
    RangeFetchRequest request;
    request.offset = 0;
    request.length = 200;
    request.destination = dest;
    request.expected.etag = "\"etag\"";
    request.expected.total_size = server.total_size();

    const auto result = transport.fetch_range(request);
    check(result.status == Status::truncated, "short body is reported as truncated, not a partial success");
}

void test_reset_mid_body() {
    auto content = make_content(1024);
    sub0tieredcache::test::TestRangeServer server(content, "\"etag\"");
    server.push_behavior(sub0tieredcache::test::TestRangeServer::Behavior::reset_mid_body);
    Http1RangeTransport::Options options = fast_options(server.url()); // see test_truncated_body's comment
    options.max_retries = 0;
    Http1RangeTransport transport(options);

    std::vector<std::byte> dest(200);
    RangeFetchRequest request;
    request.offset = 0;
    request.length = 200;
    request.destination = dest;
    request.expected.etag = "\"etag\"";
    request.expected.total_size = server.total_size();

    const auto result = transport.fetch_range(request);
    check(result.status == Status::truncated, "a connection reset mid-body is reported as truncated");
}

void test_503_then_success() {
    auto content = make_content(1024);
    sub0tieredcache::test::TestRangeServer server(content, "\"etag\"");
    server.push_behavior(sub0tieredcache::test::TestRangeServer::Behavior::server_error_503);
    Http1RangeTransport transport(fast_options(server.url()));

    std::vector<std::byte> dest(64);
    RangeFetchRequest request;
    request.offset = 10;
    request.length = 64;
    request.destination = dest;
    request.expected.etag = "\"etag\"";
    request.expected.total_size = server.total_size();

    const auto result = transport.fetch_range(request);
    check(result.status == Status::ok, "503 then success -> eventual ok");
    check(result.retries == 1, "503 then success -> exactly one retry counted");
    check(server.request_count() == 2, "503 then success -> server saw exactly two requests");
}

void test_retries_exhausted() {
    auto content = make_content(256);
    sub0tieredcache::test::TestRangeServer server(content, "\"etag\"");
    for (int i = 0; i < 5; ++i) {
        server.push_behavior(sub0tieredcache::test::TestRangeServer::Behavior::server_error_503);
    }
    Http1RangeTransport::Options options = fast_options(server.url());
    options.max_retries = 2;
    Http1RangeTransport transport(options);

    std::vector<std::byte> dest(32);
    RangeFetchRequest request;
    request.offset = 0;
    request.length = 32;
    request.destination = dest;
    request.expected.etag = "\"etag\"";
    request.expected.total_size = server.total_size();

    const auto result = transport.fetch_range(request);
    check(result.status == Status::retries_exhausted, "persistent 5xx -> retries_exhausted after the bound");
    check(result.retries == 2, "persistent 5xx -> exactly max_retries retries consumed");
}

void test_source_changed() {
    auto content = make_content(1024);
    sub0tieredcache::test::TestRangeServer server(content, "\"current-etag\"");
    Http1RangeTransport transport(fast_options(server.url()));

    std::vector<std::byte> dest(64);
    RangeFetchRequest request;
    request.offset = 0;
    request.length = 64;
    request.destination = dest;
    request.expected.etag = "\"stale-etag\""; // caller registered against a since-superseded ETag
    request.expected.total_size = server.total_size();

    const auto result = transport.fetch_range(request);
    check(result.status == Status::source_changed, "ETag mismatch is reported as source_changed");
    check(result.retries == 0, "a validator mismatch is never retried");
    check(result.observed.etag == "\"current-etag\"", "the actually-observed ETag is reported back");
}

void test_size_mismatch() {
    auto content = make_content(1024);
    sub0tieredcache::test::TestRangeServer server(content, "\"etag\"");
    server.push_behavior(sub0tieredcache::test::TestRangeServer::Behavior::wrong_total_size);
    Http1RangeTransport transport(fast_options(server.url()));

    std::vector<std::byte> dest(64);
    RangeFetchRequest request;
    request.offset = 0;
    request.length = 64;
    request.destination = dest;
    request.expected.etag = "\"etag\"";
    request.expected.total_size = server.total_size();

    const auto result = transport.fetch_range(request);
    check(result.status == Status::size_mismatch, "a changed total size is reported as size_mismatch");
}

void test_local_argument_validation_sends_no_request() {
    auto content = make_content(64);
    sub0tieredcache::test::TestRangeServer server(content, "\"etag\"");
    Http1RangeTransport transport(fast_options(server.url()));

    std::vector<std::byte> dest(10); // deliberately smaller than the requested length
    RangeFetchRequest request;
    request.offset = 0;
    request.length = 64;
    request.destination = dest;
    request.expected.total_size = server.total_size();

    const auto result = transport.fetch_range(request);
    check(result.status == Status::invalid_argument, "destination/length mismatch is caught locally");
    check(server.request_count() == 0, "a locally-caught mistake never reaches the network");
}

} // namespace

int main() {
    run(test_correct_206, "test_correct_206");
    run(test_full_body_200_rejected, "test_full_body_200_rejected");
    run(test_bad_content_range, "test_bad_content_range");
    run(test_truncated_body, "test_truncated_body");
    run(test_reset_mid_body, "test_reset_mid_body");
    run(test_503_then_success, "test_503_then_success");
    run(test_retries_exhausted, "test_retries_exhausted");
    run(test_source_changed, "test_source_changed");
    run(test_size_mismatch, "test_size_mismatch");
    run(test_local_argument_validation_sends_no_request, "test_local_argument_validation_sends_no_request");
    return finish();
}
