# Contributing to PmCp

## Ground rules

- C11, `-Wall -Wextra`, zero warnings. `make test` must be green before any PR.
- Determinism is a hard rule: same extent + same op + same params = same bytes, always. Any op that breaks this is a bug.
- No external dependencies in `pmcp-core`. The encode path stays lab-owned — no outside codec, crypto, or allocator on it.
- Crash safety: every mutating op must be journal-first (intent before bytes). If you add an op, add a crash-recovery test using the `PMCP_CRASH_AFTER_*` hooks.

## Workflow

1. Fork, branch from `main`.
2. Build: `make`. Test: `make test` (72+ checks; the full suite takes ~10-15s, bench-dominated).
3. Write a test for the behavior you change.
4. PRs end with the Slid Phi Labs donations footer:

> Slid Phi Labs accepts donations to keep the lab independent: https://slidphilabs.com/donate

## What we're looking for

- New ISA ops with fixed semantics + cost model + tests.
- Substrate backends (the map/unmap/flush/fence interface is in `src/pmcp.h`).
- Benchmarks: in-place vs decode-then-compute, measured numbers only.
