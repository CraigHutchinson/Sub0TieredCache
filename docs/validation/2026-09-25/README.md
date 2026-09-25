# T0/T1/T3 validation (2026-09-25)

Scope: `include/sub0tieredcache/` at the PR #2 head, over Sub0MemPage `213acdd` (M2 state machines, M3
`LocalFileBackend`, `Sub0MemPage::testing`). Host-domain rows only; nothing here qualifies a GPU, Intel
USM or NVIDIA GDS path (T2/T4).

| Gate | Result |
|---|---|
| Linux GCC 13, Release, `-Wall -Wextra -Wpedantic -Werror` | 9/9 CTest |
| Linux clang 18 + libc++ | 9/9 |
| GCC ASan+UBSan (`-fno-sanitize-recover=all`), tests x3 | clean |
| GCC TSan, tests x3 | clean |
| mingw-w64 GCC 13 `-Werror`, run under Wine | every suite, same check counts as Linux |
| GitHub Actions: Linux gcc/clang, macOS, Windows MSVC `/W4 /WX`, ASan, TSan | green on PR #1 (T0 + standalone T3); PR #2 run recorded on the PR |

Per-suite check counts (Linux; identical under Wine):

| Suite | Checks |
|---|---:|
| row-cache | 65 |
| lifecycle | 48 |
| concurrency | 5 |
| allocation (zero `operator new` on hits) | 2004 |
| local-file (real files via `LocalFileBackend`, `ifstream` oracle) | 1071 |
| remote-transport (local HTTP server fault matrix) | 21 |
| remote-chunk-store | 20 |
| remote-mirror | 34 |
| remote-e2e (`Table` over `MirrorBackend`) | 25 |

## Defects found while building, and what caught them

- **Abandoned fills never finished** (review). A dropped prefetch ticket left a slot Filling forever,
  so `drain()` hung and the destructor terminated. Each table now has a completion worker.
- **Batch I/O serialised** (review). `resolve_into` waited on each row before admitting the next. It now
  admits and submits the whole batch, then waits.
- **Stale failures blocked retries** (review). A failed row stayed indexed, and prefetch-side start
  failures leaked slots. Failed rows are unindexed immediately and reclaimed by CLOCK.
- **`invalidate` re-read the old source** (review). A generation now binds its own source set.
- **Deadlock on a converting table's scratch pool** (implementation). A blocking scratch acquisition
  under the table lock waited on a release that needed the same lock. Acquisition is now non-blocking.
- **Dangling `single_source()` span** (ASan). Assigning the temporary array's span directly dangled.
- **Unregistered shard index** (mutation). Removing the bounds check went unnoticed until a test was
  added for it.
- **Windows file sharing** (Wine). A test grew a file that the backend held open with read sharing
  only. The test no longer does that, and on Windows the refusal enforces source immutability.
- **MSVC-only build errors** (CI). A missing `<array>`, single-element `std::array` deduction, and the
  deprecated `fopen`/`getenv` (now `_wfopen_s` and `GetEnvironmentVariableW` with native paths).

## Not verified

- A real external shard fixture with provenance (T1/T3 acceptance row); the fixtures are synthetic.
- HTTPS: the built-in client is plain `http://`; TLS is a caller-supplied `RangeTransportRef`.
- Performance: no timing claims. Measurements belong on the dedicated machine, after Sub0Llm defines
  its E1 fixture (docs/integration-plan.md, Sub0Llm `docs/STORAGE_STACK_PLAN.md` S1).
