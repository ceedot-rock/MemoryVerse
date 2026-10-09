# pmcp-core — BUILD.md (milestone 1, v0.1)

## What works

- **Extent manager** (`src/region.c`): `pmcp_open` maps or creates a region
  file (`<name>.pmcp`, mmap'd, grows by doubling from 1 MiB). Named extents
  with create / read / resize / delete. Handles are stable snapshots of
  directory state; `pmcp_extent_ptr` returns a direct pointer into the mapping.
- **Write-ahead journal** (`src/journal.c`): every mutation follows
  intent-fsync → heap-write-msync → commit-fsync. On open, the journal is
  scanned, the torn tail is discarded, committed records forward-replay, and
  uncommitted intents are skipped. The in-memory directory is rebuilt purely
  from the journal — the journal is the source of truth.
- **ISA interpreter** (`src/isa.c`): `COPY`, `SLICE`, `CONCAT`,
  `REINTERPRET` (dtype view change, zero byte change — the new extent aliases
  the source's heap bytes), `HASH`, and `FENCE` via `pmcp_exec`.
  All mutating ops create *new* extents (functional style), so a torn write
  can never expose partial bytes.
- **SHA-256** (`src/sha256.c`): self-contained implementation, no OpenSSL.
  Verified against the standard test vectors and Python `hashlib`
  (byte-identical, including the 56-byte block-boundary case).
- **FENCE**: ordering barrier and the unit of crash-atomicity. A fence
  record in the journal marks a durable checkpoint.
- **Snapshot / restore**: `pmcp_snapshot` returns the journal sequence at a
  fence; `pmcp_restore` replays the journal to that sequence and truncates,
  discarding later extents. Restores survive reopen (journal-driven).
- **Provenance records**: every journal entry stores op, canonical param
  hash, input extent hashes, output extent hash, and a nanosecond timestamp.
  Receipts returned by `pmcp_exec` carry `out_hash`, `journal_seq`, and
  `chamber_sig`.
- **Determinism**: identical op sequences on two regions produce
  byte-identical directory state (tested by hashing full region state).
- **Crash-recovery tests**: forked children killed via `PMCP_CRASH_AFTER_INTENT`
  / `PMCP_CRASH_AFTER_HEAP` / `PMCP_CRASH_AFTER_COMMIT` hooks (simulating
  `kill -9` at each phase); reopen verifies torn ops are invisible and
  committed ops replay.

## What's stubbed (milestone 2)

- `receipt.chamber_sig` is 64 zero bytes. The journal and receipt carry the
  field; Chamber signing plugs in at milestone 2.
- Heap is append-only with no garbage collection: `DELETE` removes the
  directory entry, heap bytes are reclaimed only by `RESTORE`/replay
  truncation. A compactor is milestone-2 work.
- Single-threaded interpreter; no locking. No network.
- Only the move/shape ISA subset is implemented (COPY, SLICE, CONCAT,
  REINTERPRET, HASH, FENCE). The compressed-domain ops (AOS ring, etc.)
  from the spec are later milestones.

## Measured numbers

Machine: this lab VM (x86-64), `-O2`, ext4. Region files in `tests/crashwork`.

| Operation | Time | Throughput |
|---|---|---|
| COPY 100 MiB extent | ~2.0–2.7 s | ~38–49 MiB/s (includes journal fsync ×2 + heap msync) |
| HASH 100 MiB extent (SHA-256) | ~1.5–2.9 s | ~35–68 MiB/s |
| Full test suite (`make test`, 72 checks) | ~10–12 s | dominated by the 100 MiB bench |

Small ops (create/copy/slice of KB-scale extents) complete in well under a
millisecond; per-op cost is dominated by two `fsync`s on the journal.

## Layout

```
src/pmcp.h            public API (spec section 6 names)
src/pmcp_internal.h   region/journal/heap layout, op_transact
src/sha256.c          self-contained SHA-256
src/journal.c         append+fsync writer, torn-tail scan reader, crash hooks
src/region.c          mmap heap, extent CRUD, two-phase commit, replay, snapshot/restore
src/isa.c             COPY / SLICE / CONCAT / REINTERPRET / HASH / FENCE
tests/test_pmcp.c     72 checks: vectors, CRUD, ops, determinism, crash, snapshot, bench
Makefile              `make`, `make test`, `make clean`
```
