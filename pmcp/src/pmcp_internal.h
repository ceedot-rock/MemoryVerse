/* pmcp_internal.h — shared internals for pmcp-core. Not public API. */
#ifndef PMCP_INTERNAL_H
#define PMCP_INTERNAL_H

#include "pmcp.h"

/* ---- data file layout ----
 * offset 0:  u32 magic 0x31504D50 ("PMP1")
 * offset 4:  u32 version (1)
 * offset 8:  u64 heap_size (durable bump pointer)
 * offset 16: u64 reserved
 * offset 32: heap bytes (append-only, bump allocator)
 */
#define PMCP_DATA_MAGIC 0x31504D50u
#define PMCP_DATA_VERSION 1u
#define PMCP_HDR_SIZE 32u
#define PMCP_NAME_MAX 64

/* journal record ops (superset of public pmcp_op_t) */
#define J_CREATE      1
#define J_RESIZE      2
#define J_DELETE      3
#define J_COPY        4
#define J_SLICE       5
#define J_CONCAT      6
#define J_REINTERPRET 7
#define J_HASH        8
#define J_FENCE       9
#define J_RESTORE     10

#define J_MAGIC 0x314A4D50u /* "PMJ1" */

struct extent_meta {
    char name[PMCP_NAME_MAX];
    uint64_t heap_off;
    uint64_t len;
    uint32_t dtype;
    unsigned char hash[32];
};

struct pmcp_region {
    char data_path[256];
    char journal_path[256];
    int data_fd;
    int journal_fd;
    void *base;             /* mmap base; heap at base + PMCP_HDR_SIZE */
    size_t map_size;
    uint64_t heap_bump;     /* in-memory bump pointer (>= durable heap_size) */
    uint64_t next_seq;
    struct extent_meta *dir;
    size_t dir_len;
    size_t dir_cap;
};

struct pmcp_extent {
    pmcp_region_t *region;
    char name[PMCP_NAME_MAX];
};

/* journal.c */
typedef struct {
    uint8_t op;
    uint8_t committed;
    uint64_t seq;
    uint64_t timestamp_ns;
    unsigned char params_hash[32];
    unsigned char (*inputs)[32];
    uint32_t n_inputs;
    unsigned char *payload;
    uint32_t payload_len;
    unsigned char output_hash[32];
} jrec_t;

/* Append one record; fsyncs. Returns 0 on success. */
int journal_append(pmcp_region_t *r, uint8_t op, uint8_t committed,
                   const unsigned char params_hash[32],
                   const unsigned char (*inputs)[32], uint32_t n_inputs,
                   const unsigned char *payload, uint32_t payload_len,
                   const unsigned char output_hash[32],
                   uint64_t *seq_out);

/* Scan all complete records in order; stops at first torn tail.
 * cb returns 0 to continue, nonzero to stop. Returns records seen. */
uint64_t journal_scan(pmcp_region_t *r,
                      int (*cb)(const jrec_t *rec, void *ctx), void *ctx);
void jrec_free_inputs(jrec_t *rec); /* frees inputs/payload owned by scan */

/* region.c internals */
int region_ensure_heap(pmcp_region_t *r, uint64_t need);
int region_heap_reserve(pmcp_region_t *r, uint64_t len, uint64_t *off_out);
int region_heap_write(pmcp_region_t *r, uint64_t off, const void *data, uint64_t len);
int region_dir_apply(pmcp_region_t *r, const jrec_t *rec); /* replay one committed record */
int region_rebuild(pmcp_region_t *r, uint64_t upto_seq);  /* full replay to seq */
struct extent_meta *region_dir_find(pmcp_region_t *r, const char *name);
uint64_t now_ns(void);
void crash_hook(const char *point); /* test hook: _exit if env var set */

/* payload packer (region.c) */
typedef struct {
    unsigned char *buf;
    size_t len, cap;
} pk_t;
int pk_init(pk_t *p);
void pk_free(pk_t *p);
int pk_u32(pk_t *p, uint32_t v);
int pk_u64(pk_t *p, uint64_t v);
int pk_name(pk_t *p, const char *name);

/* two-phase commit for mutating ops (region.c). heap_bytes may be NULL.
 * Computes nothing: caller reserves heap, builds payload, hashes. */
int op_transact(pmcp_region_t *r, uint8_t jop,
                const unsigned char *pser, size_t pser_len,
                const unsigned char (*inputs)[32], uint32_t n_inputs,
                const unsigned char *payload, uint32_t payload_len,
                const unsigned char *heap_bytes, uint64_t heap_off, uint64_t heap_len,
                const unsigned char out_hash[32],
                pmcp_receipt_t *rc);

#endif /* PMCP_INTERNAL_H */
