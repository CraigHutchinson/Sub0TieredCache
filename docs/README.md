# Reference material

Start with [integration-plan.md](integration-plan.md) for current responsibilities, contracts and standalone test gates.

These files say what was learned about how other people solved this problem and what the constraints
really are, so [`REQUIREMENTS.md`](../REQUIREMENTS.md) can be argued with on evidence rather than on
assertion. A conclusion with its own correction attached (see [prior-art.md](prior-art.md)'s "where the
literature disagrees or evidence is thin" section) does not invite a future reader to re-derive the same
error independently.

- **[prior-art.md](prior-art.md)** — real systems and papers researched for huge sparse embedding/lookup
  table serving under a memory budget (NVIDIA Merlin HugeCTR's Hierarchical Parameter Server, Bandana,
  TT-Rec, DeepSpeed ZeRO-Infinity, DLRM/MoE-Infinity/FlashMoe frequency-aware caching, LMDB vs. RocksDB),
  each with a stated confidence tag for how directly it was verified.
- **[tiered-storage-design.md](tiered-storage-design.md)** — the full design: the problem at real scale,
  how it reconciles with a real consuming engine's hot-path constraints, the concrete tiered-ladder
  proposal, and a staged implementation plan. This is also this project's own origin story — it started
  as [Sub0Llm](https://github.com/CraigHutchinson/Sub0Llm)'s internal design doc before being spun out.
- **[reference-consumer-sub0llm.md](reference-consumer-sub0llm.md)** — the API traced against Sub0Llm's
  real, already-merged n-gram embeddings implementation: real call patterns, real shapes, a real
  concurrency number, and two items explicitly left **OPEN** rather than silently resolved.
