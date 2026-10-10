# PmCp — Persistent Memory Core Processing

**Status:** concept spec, v0.1 (2026-10-08)
**Author:** Slid Phi Labs

## 1. What it is

A memory-resident compute runtime. One persistent address space where data
lives permanently, plus a small core that executes transforms *in place*.
There is no serialize/deserialize boundary between "storage" and "compute":
the bytes you compute on are the bytes that persist.

The pitch in one line: data at rest is data at work.

## 2. Why

Every pipeline in the lab currently pays the same tax: load bytes from
persistent storage, deserialize/decompress, compute, re-serialize,
write back. For the common ops — search, hash, filter, slice — the compute
is trivial and the movement is the entire cost. PmCp deletes the movement.

It also fits the lab's shape: aos-ring is already a persistent
(optical) memory substrate with no compute story. PmCp is the compute
layer that substrate has been missing.

## 3. Architecture

### 3.1 Substrate

- Persistent memory regions: large, byte-addressable, crash-consistent.
- Near-term backend: memory-mapped files with a write-ahead journal on
  local NVMe. The substrate API is a narrow interface (map/unmap/flush/
  fence), so the aos-ring optical backend slots in later without touching
  the core.
- Every region has an identity. Every mutation is journaled.

### 3.2 The core

- A small, deterministic instruction set (~20 ops, "PmCp-ISA") executed by
  memory-side workers.
- Instructions operate on **extents** (named byte ranges), never on copies:
  `MAP extent -> transform -> COMMIT`.
- Determinism is a hard rule: same extent + same op + same params = same
  bytes, always. The journal makes every transform replayable.

### 3.3 Instruction set (draft v0.1)

**Move / shape**
- `COPY`, `SLICE`, `CONCAT`
- `REINTERPRET` — change the dtype view of an extent, zero byte changes
- `TRANSPOSE2D` — for matrix-shaped extents

**Compute (element-wise / reductions)**
- `MAP` — apply a small fused kernel (add/mul/clamp per element)
- `REDUCE` — sum/min/max/hash over an extent
- `DOT` — dot product of two extents

**Search**
- `FIND` — byte-pattern search, returns match offsets as an extent
- `TOPK` — indices of the k largest elements

**Provenance (Chamber)**
- `HASH` — SHA-256 of an extent into a new extent
- `SIGN` — Chamber-sign an extent's hash; signature stored as extent metadata
- `VERIFY` — check an extent against its signed hash

**Codec (PCC)**
- `PCC_ENCODE` / `PCC_DECODE` — extent to extent, lab codec
- `TRANSCODE` — re-encode between formats

**Control**
- `FENCE` — ordering barrier; the unit of crash-atomicity
- `SNAPSHOT` — copy-on-write snapshot, returns an id
- `RESTORE` — roll a region to a snapshot id

Deliberately ~20 ops. Each op has fixed semantics and a fixed cost model.
No general-purpose compute: if you need a for-loop, that's the host's job.

### 3.4 Provenance — Chamber integration

Every committed transform appends a journal entry: op, params hash, input
extent hashes, output extent hash, timestamp, Chamber signature. The
journal *is* the audit trail — any extent's full lineage replays from
genesis. This is where Chamber stops being a signing sidecar and becomes
the memory's conscience.

### 3.5 Ring integration (aos-ring)

- aos-ring exposes persistent extents with optical-grade durability; PmCp
  treats a ring volume as a substrate backend behind the same narrow
  map/unmap/flush/fence interface.
- Tiering: cold extents live on the ring, hot extents in local persistent
  memory; the core migrates by access heat (simple LRU — least recently
  used — journaled so migration itself is replayable).
- `SNAPSHOT`/`RESTORE` map onto ring generations where the ring supports
  them; otherwise copy-on-write locally.

## 4. Compressed-domain ops — where it actually wins

Thesis: most bytes the lab touches rest PCC-compressed. The standard
pipeline — decode, operate, discard (or recompress) — pays full decode
for ops that never needed the plaintext. PmCp runs those ops in place.

**Honestly viable:**
- **Search over compressed bytes.** `FIND` on the encoded stream without
  full decode. Compressed pattern matching is real literature (searching
  inside LZ-family streams without expanding them); the same shape holds
  for PCC's front ends.
- **Reductions over run-length structure.** Count/sum over RLE
  (run-length encoded) regions without expanding runs.
- **Hash / sign.** Integrity never needs the plaintext — `HASH` and `SIGN`
  run on the compressed extent directly.
- **Slice / concat.** Byte-range surgery that doesn't care about content.

**Not viable (stated plainly):**
- Arithmetic on compressed values. Needs decode, full stop.
- Anything requiring per-element semantics of the decoded domain.

**The win condition:** pipelines shaped decode → filter/search/hash →
discard. That's log analysis, integrity sweeps, dataset triage — PmCp
does them without the decode step at all.

## 5. Consistency and crash safety

- Journal-first: every op writes intent + input hashes before touching a
  byte. On crash, replay forward or roll back to the last `FENCE`.
- The extent is the unit of atomicity.
- Snapshots are copy-on-write and cheap; retention policy is per region.

## 6. API sketch (C first — lab-owned, then bindings)

```c
pmcp_region_t *pmcp_open(const char *name);          /* map or create */
pmcp_extent_t *pmcp_extent(pmcp_region_t *r, const char *name);
pmcp_receipt_t pmcp_exec(pmcp_extent_t *e, pmcp_op_t op, const void *params);
/* receipt: out_hash, journal_seq, chamber_sig */
pmcp_snap_t   pmcp_snapshot(pmcp_region_t *r);
int           pmcp_restore(pmcp_region_t *r, pmcp_snap_t id);
```

## 7. Build order (no hardware required)

1. **pmcp-core** — C library: extent manager (mmap + journal), ISA
   interpreter, determinism tests (same op twice → identical bytes).
2. **Chamber journal signing** — every commit signed, lineage replayable.
3. **PCC codec ops** — `PCC_ENCODE`/`PCC_DECODE` on extents.
4. **Compressed `FIND` + `HASH`** — the two honest wins, benchmarked.
5. **aos-ring substrate backend** — when the ring service API firms up.
6. **Benchmarks** — decode→op→discard vs in-place, on Silesia files and
   model-weight files. Numbers or it didn't happen.

## 8. Non-goals

- Not a database. No query planner, no SQL, no indexes beyond extents.
- Not a GPU. No massive-parallelism story in v0.1.
- Not in-SRAM compute. Near-memory, stated honestly.

## 9. Open questions

1. Substrate priority: local NVMe first for speed of iteration, or go
   straight at ring integration?
2. ISA governance: freeze v0.1, or allow lab-signed op packs later?
3. First workload to prove it: integrity sweeps (hash/sign at rest) or
   compressed search?
