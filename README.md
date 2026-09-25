# Sub0TieredCache — a tiered cache for huge frozen sparse-lookup tables

The [integration plan](docs/integration-plan.md) owns the current delivery sequence and the MemPage boundary. Historical design discussion below supplies motivation, not additional shipped capabilities.

Status: **SPEC / REQUIREMENTS DRAFT.** No implementation exists yet, in either Sub0Llm or this repo —
`include/sub0tieredcache/sub0tieredcache.hpp` is a skeleton. This document is the pitch and the concrete API surface;
[REQUIREMENTS.md](REQUIREMENTS.md) is the normative contract an implementation is checked against.

**Documentation map**:
- [REQUIREMENTS.md](REQUIREMENTS.md) — the normative contract (R1–R14), each a testable sentence.
- [AGENTS.md](AGENTS.md) — pre-flight checklist for anyone (human or agent) implementing against this spec.
- [STYLE_GUIDE.md](STYLE_GUIDE.md) — naming and code-style conventions.
- [docs/](docs/) — reference material: [tiered-storage-design.md](docs/tiered-storage-design.md) (the
  full design and why this should be a separate project — read this for the "why," this README is the
  "what"), [prior-art.md](docs/prior-art.md) (real cited systems/papers), and
  [reference-consumer-sub0llm.md](docs/reference-consumer-sub0llm.md) (the API traced against Sub0Llm's
  real, already-merged consumer code).

**Name**: **Sub0TieredCache** describes caching across memory, local storage and remote tiers.
It is an independent member of the `Sub0` project family.

- **Repository**: `Sub0TieredCache`.
- **C++ namespace**: `sub0tieredcache::` (lowercase, a sibling of `sub0::`).
- **Public include**: `<sub0tieredcache/sub0tieredcache.hpp>`.
- **CMake target**: `Sub0TieredCache`, with alias `Sub0TieredCache::Sub0TieredCache`.
  The current scaffold is header-only; it produces no compiled library artifact.

## 1. Scope

Sub0TieredCache serves **fixed-width rows from huge, sparsely-accessed, effectively-frozen lookup tables**
under a real memory budget — think "one embedding table with billions of rows, a few kilobytes of
working set touched per request, and nowhere near enough RAM or VRAM to hold it all." It answers exactly
one question, well: *given a table and a row index, get me that row's bytes, as fast as this machine's
actual memory hierarchy allows, without ever requiring the whole table to be resident anywhere.*

It exists because this shape of problem is genuinely generic, not specific to language-model n-gram
tables or to Sub0Llm: any application holding one or more huge, mostly-static, sparsely-touched
fixed-row-width tables — embedding tables (recommendation systems, LLM lookup tables), feature stores,
precomputed lookup/cache tables for any domain — hits the identical wall: too big for RAM, accessed too
sparsely and non-locally for the OS page cache alone to reliably help, updated rarely enough that a
write-optimized database is solving a problem that doesn't exist here.

### 1a. In scope

- **Row-granular random-access reads** from a table addressed by `(table_id, row_index)`, where
  `row_index` is an arbitrary, caller-computed integer (a hash bucket, a vocabulary id, a feature key —
  Sub0TieredCache does not care how it was derived).
- **A tiered cache hierarchy** (in-process hot cache → RAM working-set cache → local disk/NVMe cache →
  a pluggable remote/cold source) that a caller configures per deployment, not per call.
- **Two access shapes**, both first-class: a **precomputed working-set** mode (the caller already knows,
  or can cheaply compute, the exact — or a superset — set of rows a workload will ever touch, and wants
  it warmed once, ahead of time) and a **reactive** mode (rows are only known as they're requested, and
  the cache must adapt via an eviction policy).
- **An explicit, synchronous resolve step** a caller can insert into its own hot path's *setup* phase —
  never something that happens implicitly mid-computation — so a caller with its own no-heap-allocation
  or bounded-latency hot-path requirement can use Sub0TieredCache without violating it (see §3's concurrency
  model).
- **Pluggable table sources**: a local flat file, a local set of externally-formatted shard files (e.g.
  safetensors) with a caller-supplied offset-resolution callback, or a remote HTTP(S) Range-request
  source — see §3's source-descriptor interface.
- **Portability**: Linux, macOS, and Windows as first-class targets from day one — the *design*, not
  necessarily the first implementation, must not assume a single platform's filesystem/cache-directory
  conventions (contrast with Sub0Llm itself, which is Windows-first today; Sub0TieredCache is not).

### 1b. Explicitly out of scope (non-goals)

- **Not a general vector database.** No similarity search, no approximate nearest-neighbor index, no
  distance metrics. A row is an opaque fixed-width byte blob to Sub0TieredCache; if a caller wants ANN search
  over the same data, that's a different (and welcome to be built on top) tool.
- **Not a training framework.** Sub0TieredCache never computes gradients, never updates a row's value as part
  of an optimization loop. It may support row *replacement* at defined update boundaries (§3), but that
  is a bulk/administrative operation, not a training-time write path.
- **Not a general-purpose caching library.** It is deliberately scoped to fixed-row-width,
  huge-cardinality, sparse-access tables — not a drop-in replacement for `memcached`/`redis`-shaped
  general key-value caching, and not competing with those for variable-size or write-heavy workloads.
- **Not responsible for computing which rows are needed.** Hash formulas, routing decisions, and
  vocabulary/id derivation are entirely the caller's concern (Sub0Llm's own n-gram hash formula, or a
  MoE router's expert-selection logic, or anything else) — Sub0TieredCache only ever receives already-computed
  row indices.
- **Not a distributed-consensus or multi-writer-conflict-resolution system.** The tables Sub0TieredCache serves
  are either genuinely frozen (an imported checkpoint) or updated only at well-defined bulk boundaries
  (e.g. a training checkpoint) — never concurrently mutated by multiple writers mid-flight. Single-writer,
  many-reader is the entire concurrency model this project needs to get right (§3); anything requiring
  real multi-writer conflict resolution is out of scope. (This is per-source, not a ceiling on a
  logical table having more than one source — see `REQUIREMENTS.md`'s DR1: an overlay source is still a
  single administrative writer, just a second one layered over the base's.)
- **Not a model file format, and not a model-serving system.** Sub0TieredCache does not know what a "layer" or
  a "checkpoint" is. It serves rows; what those rows mean is entirely up to the caller.

## 2. What would make this broadly useful beyond Sub0Llm

The problem Sub0TieredCache solves recurs anywhere a sparse lookup table has outgrown its host's memory —
stated generically because that genericity is the actual argument for spinning this out as its own
project rather than keeping it Sub0Llm-internal:

- **Any large frozen embedding table an inference/distillation pipeline wants to reuse without re-hosting
  it fully resident** — the motivating case here (a real, huge n-gram embedding table from an
  open-weight model release), but structurally identical to reusing any other released model's huge
  embedding/lookup component.
- **Recommendation-system-style embedding tables** — the exact problem shape the prior-art research
  (`docs/prior-art.md`) is drawn from (HugeCTR, Bandana, DLRM hot/cold splitting, the
  frequency-aware GPU cache). Sub0TieredCache is not a recommendation-system library, but the underlying
  row-serving problem is the same one those systems solve, minus everything about online training that
  they also need to handle and Sub0TieredCache deliberately does not.
- **Sparse mixture-of-experts weight serving** — MoE-Infinity/FlashMoe (`docs/prior-art.md`) solve a structurally identical problem (huge sparse table, tiny active subset per request,
  resource-constrained hardware) for expert weights rather than embedding rows; nothing in Sub0TieredCache's
  design is embedding-specific, so the same library could plausibly serve MoE expert weight blocks too,
  if a caller shaped the request that way.
- **Any precomputed feature/cache table too large to keep resident** in a resource-constrained deployment
  (edge devices, personal workstations, anywhere the "63GB RAM, 8GB VRAM" constraint that motivated this
  project generalizes to "less RAM than the data").

## 3. Proposed interface and ownership

The [integration plan](docs/integration-plan.md) defines revision S1. These are conceptual
operations, not frozen C++ declarations or implemented capabilities.

- Registration supplies immutable source identity/generation, row count, encoded row width and
  layout, output representation/domain, bounded budgets and an optional caller codec. Flat-file
  offsets use the encoded width; shard resolvers supply checked byte ranges. GPU contexts are explicit.
- `prefetch` submits bounded work and returns a completion ticket. `wait` observes completion only;
  it does not pin a row or guarantee a later cache hit.
- `try_get` returns an optional move-only `RowLease`, never an unowned row view. It does not wait
  for I/O or locks. A lease protects its generation and representation until released; GPU consumers
  keep it through their final event using bounded retirement storage.
- `resolve_into` uses a caller-owned destination claim and explicit completion. Failed or short fills
  never publish usable rows. Destination memory survives every outstanding writer and consumer.
- `stats` reports bounded resource use, row hits and actual transport/fallback routes.
- `invalidate` is an administrative generation transition. New requests bind to a new immutable snapshot; old leases retain their generation. Drain old
  writers and leases before retiring or reusing old source/storage. Reject a transition that exceeds budgets.

### 3a. Consistency

Sources are immutable throughout a live generation. Disk entries include source identity, generation
and representation; publication is atomic. No TTL, file mtime or advisory callback substitutes for
identity. A concurrent reader cannot observe a partially written row or silently cross generations.
Remote acquisition and persistent publication belong here; local byte movement uses Sub0MemPage.

### 3b. Concurrency

Coalesce requests only within the same generation, representation and device/context. Admission and
result storage are bounded and preallocated. Batch acquisition pins all requested results or none;
requests too large for the configured capacity fail before waiting. Blocking resolve/wait never hold
state locks while awaiting I/O. Administrative registration/drain may block. Portable host behavior
is the baseline; optional GPU backends explicitly report unsupported configurations.

## 4. What Sub0TieredCache stands on — prior art, as this project's own bootstrapping reference

The full research behind this list — direct quotes, confidence tags, and the reasoning for why each one
matters — lives in `docs/prior-art.md`; restated here in the compressed form a project
README's own "prior art" section would carry, since that document doubles as this project's research
foundation per the task that produced it:

- **NVIDIA Merlin HugeCTR's Hierarchical Parameter Server** — the closest existing system to Sub0TieredCache's
  own shape (GPU → CPU → SSD/distributed-DB tiers, RocksDB/Redis as concrete disk/distributed backends,
  LRU + a hit-rate-threshold-gated sync/async insert policy). Deprecated as a standalone module since
  v25.03 — read as a caution about scope creep (a fully general, multi-backend, always-on serving layer
  is expensive to keep alive), not as evidence against tiering itself.
- **Bandana (Meta, MLSys 2019)** — the load-bearing citation. Small-DRAM-cache-in-front-of-NVM,
  co-access-aware physical row placement (hypergraph partitioning), and cache-size-by-simulation rather
  than by guess. Sub0TieredCache's disk-tier design should eventually adopt both techniques once a real access
  trace exists to drive them (not designed yet — see `docs/tiered-storage-design.md` §6).
- **TT-Rec (Meta, MLSys 2021)** — an orthogonal axis (compress the table) rather than a competing one
  (tier its serving); relevant mainly if Sub0TieredCache is ever asked to serve a table its *owner* is willing
  to re-factorize, not for serving an already-dense frozen checkpoint as-is.
- **DeepSpeed ZeRO-Infinity's NVMe offload** — a deliberate negative precedent: built for bulk, sequential
  parameter/optimizer-state streaming, not row-sparse random lookup. Cited to rule out "just offload like
  ZeRO does" as a template, not to reuse.
- **Frequency-aware GPU software caching for DLRM (arXiv:2208.05321) and DLRM hot/cold splitting
  (Mahajan et al., VLDB 2022)** — evidence for a frequency-leaning (not plain-recency) eviction policy as
  the better default for this access shape, and a concrete anchor number (~1.5% GPU residency sufficient
  for "decent" throughput on Zipfian recommendation-embedding access) for sizing a hot tier.
- **MoE-Infinity (arXiv:2401.14361) and FlashMoe (2026)** — the most structurally similar recent systems:
  huge sparse table, tiny per-request active subset, resource-constrained personal hardware, disk-tier
  caching, and a shared finding that structure-aware prediction beats plain LRU for this shape of access.
  Confidence on their specific quantitative claims is lower (see the companion doc's confidence tags) —
  cited for the qualitative direction, not the exact numbers.
- **`mmap` + OS page cache** — the honest null hypothesis worth re-litigating for any specific deployment
  before building a bespoke tier: for large-file random access, the kernel's own LRU-based page cache
  already does real work for free. It stops being sufficient specifically when the caller's row size is
  small relative to the page size (this problem's real row is 320–640 bytes against a 4KB page) *and*
  physically adjacent rows are not access-correlated (true here, since row placement is hash-bucket
  order, not co-access order) — Bandana's own contribution is precisely closing that gap.
- **LMDB vs. RocksDB** as the disk-tier backend shape — LMDB's B+tree-over-`mmap` design (zero-copy reads,
  MVCC, no compaction-induced latency spikes) is the better fit for Sub0TieredCache's read-dominated, rare-bulk-write
  access pattern than RocksDB's LSM-tree (write-optimized, real compaction stalls) — a genuine,
  reasoned disagreement with HugeCTR's own choice of RocksDB, justified by the fact that HugeCTR's tier
  also serves continuously-online-updating models, a write pattern Sub0TieredCache's tables (§1b) never have.

## 5. Integration seam: how Sub0Llm (or any consumer) would use this

Three real options, weighed rather than picked by default, per the task brief's explicit ask:

1. **Vendored header-only client, in-process, with a small internal background thread pool for
   prefetch/disk/network I/O.** The simplest option: no new deployment shape, no IPC, works identically
   whether the process is a training run or a `sub0llm-gen` decode session. Matches this engine's own
   existing precedent (`gguf.hpp` is exactly this shape — header-only, engine-free, in-process). **This
   is the recommended starting point** (§7's Stage 3 in the companion doc targets exactly this) — every
   staged exit condition through decode integration is achievable this way, with zero new operational
   complexity.
2. **A linked (static or shared) library**, once the disk/remote tiers need genuinely independent
   background threads/async I/O machinery substantial enough that "header-only" stops being the right
   packaging (large template-heavy header-only libraries have real compile-time costs too) — a natural
   graduation point for Sub0TieredCache once it has real users beyond Sub0Llm, not a day-one requirement.
3. **An out-of-process service, reached over a local Unix domain socket / named pipe / shared memory
   segment.** Only worth its real complexity once "system-level cache shared across independent
   processes" is an actual goal — e.g. two separate Sub0Llm runs (a training job and a concurrent
   `sub0llm-gen` session) sharing one already-warmed RAM/disk cache without each paying its own
   independent warm-up cost, or a genuinely multi-tenant deployment. **Not recommended before that need
   is real** — it adds a lifecycle-management problem (what starts the service, what happens if it's not
   running, versioning the wire protocol) that the vendored-client option simply doesn't have, and
   nothing in the companion doc's staged plan needs it to reach a working decode-integration end state.

**What stays in the Sub0Llm repo vs. what moves to `Sub0TieredCache`**: the thin `sub0::` -namespaced glue that
adapts Sub0TieredCache's `resolve_into`/`try_get` calls into `op_embed`'s expected input shape (tiny,
Sub0Llm-specific, belongs in Sub0Llm) stays put; the tiered cache engine itself — the tier
implementations, the eviction policies, the disk-format code, the HTTP Range client, the
offset-resolver-callback contract generalized beyond any one file format — is exactly what moves to
`Sub0TieredCache`. `docs/tiered-storage-design.md` §2f already draws the matching line for on-disk *location*
conventions (Sub0Llm's own `out/build/<name>/generated/`-relative paths vs. Sub0TieredCache's own
platform-appropriate user/system cache directory default) — the same split applies to code, not just
paths: nothing in `Sub0TieredCache` should ever need to know what `out/build` or `generated/` mean.

## 6. What "system, user, or project-level cache" means as configuration

Reusing the same three-way split most portable caching tools already converge on, since inventing a
fourth would just be a worse version of an already-solved problem:

- **Project-level**: a cache directory scoped to one specific workload/deployment (Sub0Llm's own
  `out/build/<name>/generated/`-relative path is exactly a project-level cache from Sub0TieredCache's point of
  view) — explicitly passed in via the `local_disk_cache_dir` source-descriptor field (§3), never
  inferred.
- **User-level**: the platform's per-user cache directory (`$XDG_CACHE_HOME` or `~/.cache` on Linux,
  `%LOCALAPPDATA%` on Windows, `~/Library/Caches` on macOS) — Sub0TieredCache's own default when a caller does
  not specify a `local_disk_cache_dir` explicitly, namespaced under a `Sub0TieredCache/<table identity>`
  subdirectory so multiple unrelated callers on the same machine don't collide.
- **System-level**: a machine-wide, multi-user-shared cache directory (e.g. `/var/cache/Sub0TieredCache` on
  Linux, `%ProgramData%` on Windows) — never Sub0TieredCache's own default (writing there typically needs
  elevated privileges and shared-ownership semantics this spec does not want to assume), but a caller may
  point `local_disk_cache_dir` at one explicitly, in which case §3a's per-entry version-tagging
  requirement is what keeps multiple, possibly-different-versioned, concurrent consumers correct.

None of these three is a distinct code path inside Sub0TieredCache — they are all just different values for the
same `local_disk_cache_dir` configuration, which is the entire point: "system/user/project-level cache"
is a **deployment configuration choice**, not an architectural axis Sub0TieredCache's own code needs to branch
on.

