# Optional remote mirror (T3)

Standalone package: no dependency on the T0/T1 row-cache API. See
[integration-plan.md](integration-plan.md)'s T3 row ("Local HTTP server failure matrix and
restart/version tests; real external shard fixture with provenance") and its "Versioning and remote
tier" section for the delivery contract this implements. `docs/tiered-storage-design.md` §2d/§2f
supply the design rationale (extending `gguf.hpp`'s "read exact bytes via a computed offset" pattern
to a remote+versioned-local-mirror shape; the platform-default cache-directory convention).

## What this package is

Four pieces, each its own header under `include/sub0tieredcache/remote/`:

- **`transport.hpp`** — the range-fetch seam. `RangeTransportRef` type-erases anything with a
  `fetch_range(const RangeFetchRequest&) -> FetchRangeResult` method, mirroring Sub0MemPage's
  `FillBackendRef` idiom one layer up.
- **`http_client.hpp`** — `Http1RangeTransport`, the one transport this package implements:
  `http://` only, POSIX sockets (Winsock behind `#ifdef _WIN32`, unverified — see below).
- **`chunk_store.hpp`** — `ChunkStore`, the versioned local disk-tier mirror: fixed-size chunk
  files, each self-describing (magic/version/source-identity/validator/checksum), published
  atomically (temp file + fsync + rename).
- **`mirror.hpp`** — `Mirror`, combining the two into `read(offset, length, dest)`: local hit, or
  fetch-validate-publish-then-copy, with same-chunk concurrent fetches coalesced into one.

Plus `checksum.hpp` (FNV-1a, corruption detection only) and `cache_dir.hpp` (the platform-default
user cache directory, README.md §6).

All of it lives on the resolve-pass/administrative side (REQUIREMENTS.md R2) — `Mirror::read` may
block on network and disk I/O and may allocate. None of it is reachable from a hypothetical future
`try_get`.

## Semantics

- **Source identity** (`SourceIdentity`: URL + `SourceValidator` + total size) is captured once, up
  front, by the caller — never inferred, never re-derived mid-session. A strong ETag is the
  preferred validator; `Last-Modified` + size is the documented fallback when a server has no ETag.
  Both are compared as exact bytes, never hashed away, so a hash collision can never make a stale
  entry look fresh.
- **Validation** happens before anything is trusted or published: HTTP status must be exactly 206,
  `Content-Range` must exactly match the requested byte range, the response's total size and
  validator must match what was registered (`Status::size_mismatch` / `Status::source_changed`
  otherwise, and nothing is published on either). A 200 (full body) response to a range request is
  a hard error, never silently read as if it were the resource. `Transfer-Encoding: chunked` is not
  supported (`Status::unsupported_response`) — the client only understands `Content-Length` framing.
- **No redirects in v1**: any 3xx is `Status::unexpected_status`.
- **Bounded retries with backoff** apply only to `Status::is_transient()` outcomes (connect/send/recv
  timeout, a truncated body or connection reset, HTTP 5xx) — never to a validator mismatch, a bad
  `Content-Range`, or a caller mistake. Exhausting the retry budget after at least one retry reports
  `Status::retries_exhausted`; a zero-retry call preserves its own specific transient status instead
  of collapsing it.
- **Atomic publication**: every chunk is written to a unique temp file in the target directory,
  flushed (+`fsync`/`FlushFileBuffers`), then renamed over the final name. A cancelled or failed
  publish therefore never leaves a partial file at the final name. Leftover temp files from a prior
  crash are cleaned up (best-effort) when a `ChunkStore` opens.
- **Reading a chunk fully validates** its header, source-identity hash, exact validator bytes and
  payload checksum before returning anything; any mismatch is `Status::not_found` (a different
  source/generation's file) or `Status::corrupt` (this generation's file, but its bytes fail the
  checksum) — a caller never gets partial or unchecked bytes either way, and `Mirror` treats both
  identically: refetch, never serve.
- **Same-chunk concurrent fetches coalesce**: `Mirror` tracks one in-flight fetch per chunk index;
  every other concurrent caller for that chunk waits on it instead of issuing a second network
  request. `tests/remote_mirror_tests.cpp`'s `test_concurrent_same_chunk_coalesces_to_one_fetch`
  exercises this with 16 threads and asserts `stats().fetches == 1`.
- **Stats** (`Mirror::stats()`, AGENTS.md #9): `hits`, `misses`, `fetches`, `bytes_fetched`,
  `retries`, `validator_mismatches`, `corrupt_entries_skipped`, `publish_failures`, `coalesced`.

## What is verified here

- The full HTTP failure matrix against an in-process local range server
  (`tests/support/test_http_server.hpp`): correct 206; 200 full body rejected; wrong Content-Range
  rejected; truncated body; connection reset mid-body; 503-then-success (retry counted, exhaustion
  path separately covered); ETag change mid-session (`source_changed`, nothing published); total-size
  mismatch.
- `ChunkStore`: publish→read round-trip; a second `ChunkStore` instance over the same directory
  (simulating a process restart) serving from disk with no re-fetch; a flipped byte detected as
  corrupt; a different-validator entry never served as a match; stale temp files cleaned on open; a
  rejected publish leaving no file (final or temp) at all.
- `Mirror`: miss→fetch→publish→hit with zero further network calls; restart-over-same-directory
  reuse; a corrupted on-disk chunk transparently refetched (and counted); a stale-version chunk not
  served to a differently-validated `Mirror`; an unaligned range crossing three chunk boundaries and
  a genuinely short tail chunk at EOF, both compared byte-exact against a direct slice of the known
  source; a read extending past the registered total size rejected outright; 16-thread concurrent
  reads of one uncached chunk producing exactly one network fetch.
- All three test executables build clean with `-Wall -Wextra -Wpedantic -Werror` (GCC 13 and Clang
  18) and pass under ASan+UBSan and under ThreadSanitizer (run repeatedly for the concurrency test).

## Limits and what is not verified

- **HTTPS is not built in, deliberately.** `Http1RangeTransport` is `http://` only
  (STYLE_GUIDE.md: no third-party dependency in the header-only core, and a TLS stack is squarely
  one). A caller needing `https://` implements `RangeTransportRef`'s `fetch_range` shape against
  libcurl, WinHTTP, or any TLS-capable client and hands it to `Mirror` exactly like the built-in
  transport — no other code here changes.
- **Windows and macOS are unverified.** The Windows path (`#ifdef _WIN32`: Winsock, `MoveFileExW`,
  `FlushFileBuffers`) is written against MSVC `/W4 /WX` conventions (`NOMINMAX` +
  `WIN32_LEAN_AND_MEAN` before `<winsock2.h>`, `ws2_32` linked by CMake when `WIN32`) but could not
  be compiled or run in this container — it has not actually built or executed anywhere. macOS is
  expected to build and behave like Linux (same POSIX socket/`fsync`/`rename` code paths, and
  `cache_dir.hpp`'s `__APPLE__` branch for `~/Library/Caches`) but was likewise not exercised here.
  Both need real CI runs before being called verified (REQUIREMENTS.md R8 / AGENTS.md #3).
- **No redirect following, no chunked transfer-encoding, no HTTP/2 or keep-alive.** Every request
  sends `Connection: close`; a v1 caller pays one TCP handshake per chunk fetch. Documented as a v1
  scope limit, not an oversight — extending it is straightforward but out of scope for this pass.
- **No garbage collection of superseded chunk generations.** A chunk file is keyed by
  `(hash(url), hash(validator), chunk_index)`, so an old generation's files are never overwritten by
  a new one, and equally never automatically reclaimed. A deployment that invalidates its source
  repeatedly will accumulate old-generation chunk files on disk; a caller wanting bounded disk usage
  needs to manage that externally for now (`ChunkStore::directory()` exposes where they live).
- **No disk-tier eviction/co-access placement.** This package is the versioned mirror + validated
  transport only; the Bandana-style co-access reordering and LRU/LFU sizing
  `docs/tiered-storage-design.md` §3 discusses for the disk tier are explicitly out of scope here
  (see that doc's §6 "Explicitly deferred").
- **The real external shard fixture with provenance** integration-plan.md's T3 row names is not
  included in this pass — the test suite is entirely the offline, deterministic local-server fixture
  described above; wiring a real HTTPS-hosted external shard through a caller-supplied TLS transport
  is a follow-on, not part of this package's own standalone correctness claim.
