# Sub0Firn Code Style Guide

Sub0Firn originated inside [Sub0Llm](https://github.com/CraigHutchinson/Sub0Llm) (see
`docs/tiered-storage-design.md`'s origin note), so its naming follows Sub0Llm's own observed
conventions rather than a different sibling project's — there is no single "Sub0 house style" across
the family (`Sub0Log`, for instance, follows `Sub0Pipeline`'s own, different convention), so this
document is self-contained rather than a deltas-only note.

## Naming

- **Namespace**: lowercase, unnested — `sub0firn::` (a sibling of `sub0::`, not nested inside it; see
  `README.md`'s naming section for why).
- **Types**: `PascalCase` — e.g. `TableHandle`, `PrefetchTicket`, `RowView`.
- **Free functions**: `snake_case` — e.g. `register_table`, `resolve_into`, `try_get`. Matches every
  function name already fixed by `README.md` sec 3's API surface.
- **Constants / compile-time values**: `ALL_CAPS` for genuine compile-time constants (e.g. a fixed row
  width known at compile time in a specific instantiation); ordinary `snake_case` for runtime
  configuration values (e.g. a `local_disk_cache_dir` field) — the distinction is whether the value can
  ever differ between two builds of the same binary, not just whether it happens to be `const`.

## Modern C++ first, C++23 baseline

Reach for a language feature before inventing a workaround, and prefer the newest form the project's
standard supports. C++23 is a hard requirement (`CMakeLists.txt`'s `target_compile_features(... cxx_std_23)`),
not a preference — R2/R3's synchronous-barrier + never-blocking-`try_get` contract is naturally expressed
with `std::optional`/`std::span`/`std::expected`-shaped return types rather than out-parameters and
sentinel values, and there's no reason to target an older standard than the one that makes the contract
read cleanly.

## Portability is load-bearing, not aspirational (R8)

No header may contain an unconditional `#include <windows.h>`, POSIX-only header, or platform-specific
syscall without an `#if`-guarded portable abstraction on every other platform. A change that only compiles
on the author's own platform is not done — R8 exists specifically because this project's motivating
first consumer (Sub0Llm) is Windows-first, and Sub0Firn deliberately is not; a portability regression
here undoes the entire reason this is a separate project.

## Comment and citation discipline

Every non-obvious design decision gets a comment that says *why*, not just *what* — the style already
used throughout this project's own docs (`docs/prior-art.md`'s confidence tags, `REQUIREMENTS.md`'s
"sentence first, explanation second" structure). Concretely:

- A comment justifying an algorithm/data-structure choice (an eviction policy, a disk format) cites the
  real source it came from — `docs/prior-art.md` if one already exists there, a freshly fetched and
  quoted source if not. "It seemed reasonable" is not a citation.
- A comment noting a deliberate limitation or deferred feature says so explicitly (`// deferred:` or
  equivalent), the same "explicitly deferred, not silently dropped" discipline `docs/tiered-storage-design.md`
  §6 already uses at the design level.
- Where an implementation choice reconciles with a specific real consumer's need, cite the concrete case
  (`docs/reference-consumer-sub0llm.md`'s style: real numbers, real call sites, not a hypothetical).

## No third-party dependencies in the header-only core

Matches R8's portability spirit and `CMakeLists.txt`'s interface-library shape (`README.md` sec 5.1): the
vendored header-only client must build with nothing beyond the C++23 standard library. A future linked
library (`README.md` sec 5.2, once one becomes genuinely necessary) may take on a real dependency (an
HTTP client, a disk-format library) — that boundary is exactly why sec 5 draws the line where it does.
