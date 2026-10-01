# Sub0Llm consumer audit

Audited against Sub0Llm `5eede7d` on 2026-09-25; MoE row updated 2026-10-01 (Sub0Llm `6fbd413`).
See the [shared plan](../../Sub0Llm/docs/STORAGE_STACK_PLAN.md) for acceptance and sequencing.

| Current source seam | Responsibility and candidate integration | Required fixture |
|---|---|---|
| `Model::build_layout`, CPU `ngram_tab` access in `src/backends/cpu/backend.cpp` and `decode.cpp` | Current tables are mutable parameter/optimizer storage. Keep them resident; introduce a separate frozen imported-table adapter for TieredCache | Frozen duplicate IDs and version changes against resident oracle; unchanged mutable training |
| `moeq::Store`, `ExpertCache`, `ExpertCacheSource` in `include/sub0/moe_quant.hpp` | Llm owns encoded format and decoding. Raw encoded staging and decoded F32 pools remain distinct | Three-plane encoded parity, real model descriptors, decoded/fused output parity |
| `g_moe_decode_io`, `MoeIoStage`, `g_moe_io_stage` in CPU `internal.hpp` and `decode.cpp` | The MemPage byte adapter (`--moe-io-mode mempage`, S1b) is done. Next: an owned expert cache on one `Table` with `RowExtent::bounded`. Each row is one (layer, expert), the contiguous span of its three encoded planes, with identity representation. Plane sizes differ between layers, so rows are up to the largest expert. Leases are held until the layer's compute joins | Encoded-byte parity against `moeq::Store`, decode-output parity, leases never reused before the last reader |
| `g_backbone_quant` and `bbq::Store` | Native encoded embedding/head/GDN paths already exist. Measure residency and reuse before proposing paging | Native output parity; startup versus repeated streaming costs |
| `src/backends/cuda/backend.cu` capability assertions | CUDA currently rejects `NGRAM_EMBED` and `USE_MOE`. Storage support cannot implement these compute paths | Transport-only parity first; separate kernel and model integration gates |

Row identity and representation belong to TieredCache; model routing, hashing, formats, codec math and
compute scheduling remain in Llm. MemPage receives immutable byte ranges and explicit destination
claims without model or table knowledge. A fake transport proves lifetime/error contracts without
hardware; real-file and hardware-qualified gates add evidence independently.
