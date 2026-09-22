# Agent instructions for Sub0TieredCache

This file is a pre-flight checklist, not a tutorial — the same role
[Sub0Llm's own `AGENTS.md`](https://github.com/CraigHutchinson/Sub0Llm/blob/main/AGENTS.md) plays there,
which this project descends from (see `docs/tiered-storage-design.md`'s origin note). No code has shipped
yet, so unlike that document these rules aren't citing a bug that already happened — they're derived
directly from real decisions already made in `REQUIREMENTS.md`/`README.md`/`docs/`, stated here as
constraints so implementation starts from them rather than rediscovering them the hard way. Update this
file the day a rule below is actually tested by a real incident, and say so.

If you're about to implement, extend, or change anything here: read this first.

## 1. `try_get` never blocks; only `resolve_into`/`wait` may touch I/O (REQUIREMENTS.md R2)

This is the entire reason the project exists — a caller with its own no-heap-allocation or
bounded-latency hot-path rule (`docs/reference-consumer-sub0llm.md`'s real trace: Sub0Llm's `forward()`,
running inside a rule that forbids exactly this kind of implicit-latency branch) needs a hard guarantee,
not a "usually fast." Any implementation change that makes `try_get` occasionally block, or that adds a
new call implying it might do I/O without saying so explicitly, breaks the contract this project's first
real consumer was designed around — not a style nit, a correctness violation.

## 2. Never bake a specific consumer's domain knowledge into the core (R1, R7, R9)

The reuse boundary is the offset-resolution callback and the opaque, fixed-width row — `register_table`
never learns what "n-gram," "Qwen," or "safetensors" mean, and the core never parses any specific
external file format. A change that special-cases any one caller's domain inside the tiered-cache engine
itself is a scope violation, not a convenience — it's exactly the failure mode `README.md` §2 argues
against (the entire point of spinning this out of Sub0Llm was that the problem is generic). If a need
looks domain-specific, it belongs in the caller's own adapter code, not here.

## 3. Portability is checked on all three platforms, not compiled on one (R8)

See `STYLE_GUIDE.md`. A change that only builds/behaves correctly on the author's own OS is not done.
This is a harder rule for Sub0TieredCache than it would be for a typical library, because the motivating first
consumer (Sub0Llm) is Windows-first, and a careless implementer could quietly let Windows-only
assumptions back in on the theory that "that's what the consumer uses anyway" — that reasoning is exactly
backwards; it's precisely why this project exists separately.

## 4. On-disk cache format changes carry the same blast radius as a checkpoint format (R4)

Sub0Llm's own `AGENTS.md` §3 treats checkpoint/binary-format changes as its highest-blast-radius category
because an incompatible change silently corrupts or discards real work. The disk-tier cache format here
has the same shape of risk once a system/user-level shared cache is in play (`README.md` §6): a second
process reading a cache directory written by an older/incompatible version must be able to tell a stale
entry from a fresh one (R4's version-tagging requirement), not trust file mtimes or assume compatibility.
Before changing the on-disk format: work out what an old reader does when it meets a new-format entry,
and what a new reader does when it meets an old one — both directions, not just the one you're actively
testing.

## 5. Verify eviction/caching algorithm choices against real prior art before implementing (mirrors Sub0Llm `AGENTS.md` §5)

`docs/prior-art.md` exists so a policy choice (LRU vs. frequency-based eviction, co-access-aware physical
placement, LMDB vs. RocksDB for a disk tier) is argued from a real, cited source with an honest confidence
tag — not recalled from training data or picked because it "seems standard." If the citation that would
justify a choice doesn't exist yet in `docs/prior-art.md`, fetch and quote the real source before writing
the code, and add it there. A plausible-sounding algorithm description is not the same thing as a verified
one — this project's own prior-art table already flags, honestly, which of its citations are verbatim-quoted
vs. tool-summarized; extend that same discipline to new sources, don't relax it.

## 6. Before changing the public API, re-check `docs/reference-consumer-sub0llm.md`'s trace

That document is this project's acceptance test, not just an example — it exists specifically so the API
is checked against a real caller's real needs, not designed in the abstract and hoped to fit. If a
proposed API change would make any of that trace's call patterns (§1's `resolve_into`-before-compute
loop, §2's decode-time reactive lookup, §3's dtype contract) stop being expressible, either the change is
wrong or the trace needs updating to show why it no longer applies — never leave the two silently
inconsistent.

## 7. A new tier or policy must not change behavior for a caller that hasn't opted into it

There's no compile-time configuration system here the way Sub0Llm's `RunConfig` X-macros provide one, but
the same spirit applies: adding a new cache tier, a new eviction policy, or a new source-descriptor
variant should be additive — an existing caller's observed behavior (correctness, not necessarily
performance) must be unchanged unless they explicitly opt into the new thing. "It's strictly better" is
not a substitute for actually checking nothing regresses for a caller who didn't ask for the new
behavior.

## 8. Correctness before performance

A change is not "done" on a benchmark number alone. This project has essentially no correctness
infrastructure yet (no code has shipped), which makes this rule easy to skip past in the early stages —
resist that. A claimed hit-rate improvement or latency win needs the same rigor `docs/prior-art.md`
already models for citations: state what was actually measured, under what real (or realistically
simulated) access pattern, and whether it was checked against a correctness baseline first.

## 9. Observability ships with the feature, not after it (R10)

A new tier or eviction policy is not complete without corresponding `stats()` coverage. Retrofitting
observability after the fact tends to mean nobody actually knows whether the feature works as intended in
the meantime — R10 exists because "is the cache actually working" should never be a guess.

## 10. A concurrency claim needs an actual concurrent test

`docs/reference-consumer-sub0llm.md` §1 establishes a real number (~24 concurrent callers on the
reference machine) that R5's "concurrent same-row requests coalesce" requirement has to hold up under. A
change that "works" only exercised single-threaded has not actually verified the requirement it claims to
satisfy — test it under real concurrent load, matching the real consumer's actual shape, before trusting
it.
