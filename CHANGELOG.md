# Changelog

## v0.1.0 — 2026-10-08

First milestone: pmcp-core.

- Extent manager: mmap'd persistent regions, named byte-range extents, doubling growth from 1 MiB
- Crash-safe write-ahead journal: intent-fsync → heap-msync → commit-fsync; torn writes never visible; journal is the source of truth, directory rebuilt on open
- ISA subset: COPY, SLICE, CONCAT, REINTERPRET (zero-copy dtype views), HASH (self-contained SHA-256, no OpenSSL), FENCE
- Determinism: identical op sequences on two regions → byte-identical state
- Crash recovery: forked children killed at each commit phase; torn ops invisible, committed ops replay
- Snapshot (fence record) / restore (replay-to-seq + truncate), surviving reopen
- 72 checks, 0 failures. C11, `-Wall -Wextra`, zero warnings, zero external dependencies

Measured (lab VM, -O2): COPY 100 MiB ~22–49 MiB/s journaled+fsync'd; HASH 100 MiB ~28–68 MiB/s; full suite ~10–15 s.

Stubbed for later: Chamber signing (field plumbed, zeroed), heap GC (append-only for now), compressed-domain ops.
