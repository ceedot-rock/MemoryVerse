/* pmcp.h — PmCp persistent memory core processing, milestone 1 (v0.1).
 *
 * Single-threaded, no network, no external dependencies.
 * Regions persist as "<name>.pmcp" (mmap'd heap) + "<name>.pmcp.journal"
 * (write-ahead log) in the current working directory.
 */
#ifndef PMCP_H
#define PMCP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pmcp_region pmcp_region_t;
typedef struct pmcp_extent pmcp_extent_t;
typedef uint64_t pmcp_snap_t;

/* ISA ops implemented in milestone 1 (spec section 3.3 subset). */
typedef enum {
    PMCP_OP_COPY        = 1,  /* dst = byte copy of extent */
    PMCP_OP_SLICE       = 2,  /* dst = extent[off : off+len] */
    PMCP_OP_CONCAT      = 3,  /* dst = extent ++ second */
    PMCP_OP_REINTERPRET = 4,  /* dst aliases same bytes, new dtype tag */
    PMCP_OP_HASH        = 5,  /* dst = SHA-256(extent) as 32-byte extent */
    PMCP_OP_FENCE       = 6   /* ordering barrier; e may be any extent of r */
} pmcp_op_t;

/* dtype tags for extents (REINTERPRET changes the tag, never the bytes). */
#define PMCP_DTYPE_RAW 0u
#define PMCP_DTYPE_U8  1u
#define PMCP_DTYPE_U16 2u
#define PMCP_DTYPE_U32 3u
#define PMCP_DTYPE_U64 4u
#define PMCP_DTYPE_F32 5u
#define PMCP_DTYPE_F64 6u

/* op params, passed as const void * to pmcp_exec(). */
typedef struct { const char *dst; } pmcp_p_copy_t;
typedef struct { const char *dst; uint64_t off; uint64_t len; } pmcp_p_slice_t;
typedef struct { const char *dst; const char *second; } pmcp_p_concat_t;
typedef struct { const char *dst; uint32_t dtype; } pmcp_p_reinterpret_t;
typedef struct { const char *dst; } pmcp_p_hash_t;

/* error codes */
#define PMCP_OK     0
#define PMCP_EINVAL 1
#define PMCP_EIO    2
#define PMCP_ENOMEM 3
#define PMCP_ENOENT 4
#define PMCP_EEXIST 5
#define PMCP_ERANGE 6

typedef struct {
    int ok;                       /* 1 on success, 0 on failure */
    int err;                      /* PMCP_E* code on failure */
    unsigned char out_hash[32];   /* SHA-256 of output extent bytes */
    uint64_t journal_seq;         /* seq of the commit record */
    unsigned char chamber_sig[64];/* milestone 1: zeros; hook for milestone 2 */
    uint64_t timestamp_ns;
} pmcp_receipt_t;

/* ---- region ---- */
pmcp_region_t *pmcp_open(const char *name);   /* map or create */
void           pmcp_close(pmcp_region_t *r);

/* ---- extents ---- */
pmcp_extent_t *pmcp_extent(pmcp_region_t *r, const char *name); /* lookup; NULL if absent */
void           pmcp_extent_free(pmcp_extent_t *e);               /* releases handle only */
pmcp_extent_t *pmcp_extent_create(pmcp_region_t *r, const char *name,
                                 const void *data, size_t len, uint32_t dtype);
int            pmcp_extent_delete(pmcp_region_t *r, const char *name);
int            pmcp_extent_resize(pmcp_region_t *r, pmcp_extent_t *e, size_t new_len);
const void    *pmcp_extent_ptr(const pmcp_extent_t *e);  /* NULL if extent gone */
size_t         pmcp_extent_len(const pmcp_extent_t *e);
uint32_t       pmcp_extent_dtype(const pmcp_extent_t *e);
const char    *pmcp_extent_name(const pmcp_extent_t *e);
int            pmcp_extent_hash(const pmcp_extent_t *e, unsigned char out[32]);

/* ---- ISA ---- */
/* For PMCP_OP_FENCE the extent's bytes are ignored (its region is used). */
pmcp_receipt_t pmcp_exec(pmcp_extent_t *e, pmcp_op_t op, const void *params);
int            pmcp_fence(pmcp_region_t *r);

/* ---- snapshots ---- */
pmcp_snap_t    pmcp_snapshot(pmcp_region_t *r);
int            pmcp_restore(pmcp_region_t *r, pmcp_snap_t id);

/* ---- SHA-256 (self-contained) ---- */
void pmcp_sha256(const void *data, size_t len, unsigned char out[32]);

#ifdef __cplusplus
}
#endif

#endif /* PMCP_H */
