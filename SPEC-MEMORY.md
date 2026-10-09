# PmCp as a Memory Core — Milestone 2 Spec

**Status:** concept spec, v0.1 (2026-10-08)
**Parent:** `SPEC.md` (PmCp base spec), pmcp-core v0.1.0 (shipped)

## 1. Goal

Make pmcp-core usable as the durable substrate for an agent's memory:
notes persist as extents, are searchable in place — including over
compressed bytes — ranked, with every write Chamber-signed into a
replayable journal.

The shape of the win: memory that stays PCC-compressed at rest,
searchable without decoding, crash-safe, with full provenance. Nobody's
memory works like that today.

## 2. Data model (all extents, all persistent)

- **Note extents** — one per memory note: header (id, timestamp, tags,
  source) + body (markdown/text).
- **Index extents** — the inverted index: term → postings list (note ids +
  token positions). Lives in the same address space, updated by ISA ops,
  journaled like everything else.
- **Meta extents** — registry (note-id → extent name), corpus stats
  (doc count, per-note lengths for BM25), sequence counters.

## 3. New ISA ops (milestone 2)

**Indexing**
- `TOKENIZE` — text extent → token-stream extent (tokens + offsets).
  Deterministic: lowercase, split on non-alphanumeric, keep positions.
  No stemming in v0.2 — exact match first, stemmer later.
- `INDEX_ADD` — token stream + note id → appends postings to index extents.
- `INDEX_DEL` — removes a note's postings (tombstone; see §7).
- `INDEX_REBUILD` — rebuilds the entire index from note extents.
  The recovery path: index is always derivable, never load-bearing.

**Retrieval**
- `POSTINGS` — term → extent of (note id, positions) pairs.
- `RANK` — query terms → top-k scored note ids. BM25, k1=1.2, b=0.75:
  standard, deterministic, no learned weights. Doc lengths and corpus
  stats come from meta extents.
- `SNIPPET` — note id + positions → context windows as an extent.

**Scan (the grep path)**
- `FIND` — byte-pattern search over extents, uncompressed in v0.2.
  Compressed-domain FIND (searching the PCC stream without full decode)
  is milestone 3; the postings interface doesn't change, only the scan.

Two retrieval paths, stated plainly:
1. **Index path** (primary): RANK over the inverted index. Fast, no decode
   ever needed — the index is built from tokens at write time, before
   the body is compressed.
2. **Scan path** (fallback): FIND sweeps extents directly. This is where
   compressed-domain search pays: scan compressed bytes, decode only
   the matched windows.

## 4. Write path

```
note body → TOKENIZE → INDEX_ADD (postings)
         → PCC_ENCODE → note extent (compressed at rest)
         → journal commit (op, note hash, timestamp, Chamber sig)
```

Index update is journal-first: intent (note hash + token list) is
fsync'd before postings are touched. A torn index update rolls back;
`INDEX_REBUILD` recovers from note extents alone.

## 5. Provenance (finishing the v0.1 stub)

Milestone 2 wires Chamber signing into the journal for real: every
commit carries a signature over (op, params hash, input/output extent
hashes, timestamp). The `chamber_sig` receipt field stops being 64 zero
bytes. Memory becomes replayable: the full history of what was learned,
when, and from what.

## 6. C API

```c
pmcp_mem_t *pmcp_mem_open(const char *region);
int         pmcp_mem_write(pmcp_mem_t *m, const char *id,
                           const char **tags, int ntags, const char *body);
pmcp_hits_t pmcp_mem_query(pmcp_mem_t *m, const char *query, int topk);
void        pmcp_mem_free_hits(pmcp_hits_t *h);
int         pmcp_mem_forget(pmcp_mem_t *m, const char *id);
```

`pmcp_mem_forget` tombstones the note and removes its postings — memory
must support forgetting, not just remembering.

## 7. Open questions

1. **Stemming:** exact tokens from day one, or Porter stemmer immediately?
2. **Forget path:** tombstone + lazy postings cleanup, or immediate
   postings rewrite? (Tombstone is simpler and crash-safer; rewrite keeps
   the index small.)
3. **Cutover:** shadow mode first — dual-write to the current memory
   system and pmcp, compare recall quality on real queries, then switch.
   No flag-day migrations on the thing that holds the lab's memory.

## 8. Deliberate non-goals

- No embeddings / vector search in this milestone. BM25 exact-match
  first; vectors later, as another index extent type.
- No automatic summarization or curation. The agent curates; the core
  stores and retrieves.
- No cross-region federation yet.

## 9. Build order

1. `TOKENIZE` + index extents + `INDEX_ADD` / `INDEX_DEL` + `POSTINGS`
2. `RANK` (BM25) + query API + `SNIPPET`
3. Chamber signing wired into the journal (finish the stub)
4. `INDEX_REBUILD` (recovery path)
5. Benchmarks: write throughput, query latency vs corpus size, index
   size overhead — numbers or it didn't happen
6. Milestone 3: PCC-compressed note bodies + compressed-domain FIND
