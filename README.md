# pmcp-core

PmCp (Persistent Memory Core Processing) is an experiment in moving compute
into persistent memory: instead of loading bytes out of storage, operating on
them, and writing them back, operations execute against named byte ranges
("extents") that live in a memory-mapped region file, with a write-ahead
journal making every mutation crash-atomic. This repository is `pmcp-core`
milestone 1 (v0.1): the extent manager, the journaled crash-safe commit path,
the move/shape ISA subset, and a self-contained SHA-256 — all in C11 with
zero external dependencies.

The full design is in [SPEC.md](SPEC.md).

## Build

Requirements: a C11 compiler (`gcc` or `clang`) and `make`. No libraries,
no network, no threads.

```sh
make          # builds libpmcp.a
make test     # builds and runs the test suite (72 checks)
make clean    # removes build artifacts and test region files
```

Compiler flags: `-std=c11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L`.
The build is warning-free.

## ISA ops (milestone 1)

Executed via `pmcp_exec(extent, op, params)`:

| Op | Params struct | Effect |
|---|---|---|
| `PMCP_OP_COPY` | `pmcp_p_copy_t { dst }` | Byte-identical copy of an extent into new extent `dst` |
| `PMCP_OP_SLICE` | `pmcp_p_slice_t { dst, off, len }` | Byte range `[off, off+len)` into new extent `dst` |
| `PMCP_OP_CONCAT` | `pmcp_p_concat_t { dst, second }` | `extent ‖ second` into new extent `dst` |
| `PMCP_OP_REINTERPRET` | `pmcp_p_reinterpret_t { dst, dtype }` | New dtype view of the same bytes; zero byte change |
| `PMCP_OP_HASH` | `pmcp_p_hash_t { dst }` | SHA-256 of the extent into a new 32-byte extent `dst` |
| `PMCP_OP_FENCE` | — | Ordering barrier; the unit of crash-atomicity |

Every mutating op creates a *new* extent and returns a `pmcp_receipt_t`
carrying the output hash, the journal sequence number, a timestamp, and a
`chamber_sig` field (reserved for Chamber signing in milestone 2).

## API

```c
pmcp_region_t *pmcp_open(const char *name);          /* map or create region */
pmcp_extent_t *pmcp_extent(pmcp_region_t *r, const char *name);
pmcp_receipt_t pmcp_exec(pmcp_extent_t *e, pmcp_op_t op, const void *params);
pmcp_snap_t    pmcp_snapshot(pmcp_region_t *r);
int            pmcp_restore(pmcp_region_t *r, pmcp_snap_t id);
void           pmcp_close(pmcp_region_t *r);
```

Plus extent management: `pmcp_extent_create`, `pmcp_extent_delete`,
`pmcp_extent_resize`, `pmcp_extent_len/ptr/hash/dtype`, `pmcp_extent_free`,
and `pmcp_fence`. See [src/pmcp.h](src/pmcp.h) for the full declarations.

Regions persist as `<name>.pmcp` (mmap'd heap) plus `<name>.pmcp.journal`
(write-ahead log) in the working directory.

## Crash safety

Two-phase commit per op: journal intent (fsync) → heap write (msync) →
journal commit (fsync). On open, committed records forward-replay and torn
or uncommitted records are discarded. The test suite kills forked writers
mid-op at each phase and verifies fence-atomicity on reopen.

## License

Apache-2.0. See [LICENSE](LICENSE).
