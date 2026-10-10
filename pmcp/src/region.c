/* region.c — region open/close, mmap heap, extent manager, journal replay. */
#include "pmcp_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------- little-endian payload helpers (pk_t in internal header) ---------- */

int pk_init(pk_t *p) {
    p->cap = 128;
    p->len = 0;
    p->buf = malloc(p->cap);
    return p->buf ? 0 : -1;
}

static int pk_reserve(pk_t *p, size_t n) {
    if (p->len + n <= p->cap)
        return 0;
    while (p->len + n > p->cap)
        p->cap *= 2;
    p->buf = realloc(p->buf, p->cap);
    return p->buf ? 0 : -1;
}

int pk_u32(pk_t *p, uint32_t v) {
    if (pk_reserve(p, 4)) return -1;
    p->buf[p->len++] = (unsigned char)v;
    p->buf[p->len++] = (unsigned char)(v >> 8);
    p->buf[p->len++] = (unsigned char)(v >> 16);
    p->buf[p->len++] = (unsigned char)(v >> 24);
    return 0;
}

int pk_u64(pk_t *p, uint64_t v) {
    int i;
    if (pk_reserve(p, 8)) return -1;
    for (i = 0; i < 8; i++)
        p->buf[p->len++] = (unsigned char)(v >> (8 * i));
    return 0;
}

int pk_name(pk_t *p, const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n >= PMCP_NAME_MAX)
        return -1;
    if (pk_u32(p, (uint32_t)n)) return -1;
    if (pk_reserve(p, n)) return -1;
    memcpy(p->buf + p->len, name, n);
    p->len += n;
    return 0;
}

void pk_free(pk_t *p) {
    free(p->buf);
    p->buf = NULL;
    p->len = p->cap = 0;
}

/* unpack cursor */
typedef struct {
    const unsigned char *p;
    size_t len, off;
    int err;
} upk_t;

static uint32_t upk_u32(upk_t *u) {
    uint32_t v;
    if (u->off + 4 > u->len) { u->err = 1; return 0; }
    v = (uint32_t)u->p[u->off] | ((uint32_t)u->p[u->off+1] << 8) |
        ((uint32_t)u->p[u->off+2] << 16) | ((uint32_t)u->p[u->off+3] << 24);
    u->off += 4;
    return v;
}

static uint64_t upk_u64(upk_t *u) {
    uint64_t v = 0;
    int i;
    if (u->off + 8 > u->len) { u->err = 1; return 0; }
    for (i = 0; i < 8; i++)
        v |= (uint64_t)u->p[u->off + i] << (8 * i);
    u->off += 8;
    return v;
}

/* returns pointer into payload; sets *n_out; NULL on error */
static const unsigned char *upk_name(upk_t *u, uint32_t *n_out) {
    uint32_t n = upk_u32(u);
    const unsigned char *s;
    if (u->err || n == 0 || n >= PMCP_NAME_MAX || u->off + n > u->len) {
        u->err = 1;
        return NULL;
    }
    s = u->p + u->off;
    u->off += n;
    *n_out = n;
    return s;
}

/* ---------- heap / mmap ---------- */

static int valid_name(const char *name) {
    size_t n;
    if (!name) return 0;
    n = strlen(name);
    return n > 0 && n < PMCP_NAME_MAX;
}

int region_ensure_heap(pmcp_region_t *r, uint64_t need_end) {
    uint64_t need = PMCP_HDR_SIZE + need_end;
    while (need > r->map_size) {
        size_t ns = r->map_size * 2;
        if (munmap(r->base, r->map_size) != 0)
            return -1;
        if (ftruncate(r->data_fd, (off_t)ns) != 0)
            return -1;
        r->base = mmap(NULL, ns, PROT_READ | PROT_WRITE, MAP_SHARED, r->data_fd, 0);
        if (r->base == MAP_FAILED)
            return -1;
        r->map_size = ns;
    }
    return 0;
}

int region_heap_reserve(pmcp_region_t *r, uint64_t len, uint64_t *off_out) {
    uint64_t off = r->heap_bump;
    if (len > 0) {
        if (region_ensure_heap(r, off + len) != 0)
            return -1;
        r->heap_bump = off + len;
    }
    *off_out = off;
    return 0;
}

/* write bytes + msync + advance durable header heap_size */
int region_heap_write(pmcp_region_t *r, uint64_t off, const void *data, uint64_t len) {
    unsigned char *hdr;
    if (len == 0)
        return 0;
    memcpy((unsigned char *)r->base + PMCP_HDR_SIZE + off, data, (size_t)len);
    if (msync(r->base, r->map_size, MS_SYNC) != 0)
        return -1;
    /* durable bump pointer in header */
    hdr = (unsigned char *)r->base;
    {
        uint64_t cur = 0;
        int i;
        for (i = 0; i < 8; i++)
            cur |= (uint64_t)hdr[8 + i] << (8 * i);
        if (r->heap_bump > cur) {
            for (i = 0; i < 8; i++)
                hdr[8 + i] = (unsigned char)(r->heap_bump >> (8 * i));
            if (msync(hdr, PMCP_HDR_SIZE, MS_SYNC) != 0)
                return -1;
        }
    }
    return 0;
}

static const unsigned char *heap_ptr(const pmcp_region_t *r, uint64_t off, uint64_t len) {
    if (off + len < off || PMCP_HDR_SIZE + off + len > r->map_size)
        return NULL;
    /* note: off+len <= heap_bump always holds for journal-referenced ranges */
    return (const unsigned char *)r->base + PMCP_HDR_SIZE + off;
}

/* ---------- directory ---------- */

struct extent_meta *region_dir_find(pmcp_region_t *r, const char *name) {
    size_t i;
    for (i = 0; i < r->dir_len; i++)
        if (strncmp(r->dir[i].name, name, PMCP_NAME_MAX) == 0)
            return &r->dir[i];
    return NULL;
}

static struct extent_meta *dir_insert(pmcp_region_t *r, const char *name) {
    struct extent_meta *m = region_dir_find(r, name);
    size_t n;
    if (m)
        return m; /* replace */
    if (r->dir_len == r->dir_cap) {
        size_t nc = r->dir_cap ? r->dir_cap * 2 : 16;
        struct extent_meta *nd = realloc(r->dir, nc * sizeof(*nd));
        if (!nd)
            return NULL;
        r->dir = nd;
        r->dir_cap = nc;
    }
    m = &r->dir[r->dir_len++];
    memset(m, 0, sizeof(*m));
    n = strlen(name);
    memcpy(m->name, name, n);
    m->name[n] = '\0';
    return m;
}

static void dir_remove(pmcp_region_t *r, const char *name) {
    size_t i;
    for (i = 0; i < r->dir_len; i++) {
        if (strncmp(r->dir[i].name, name, PMCP_NAME_MAX) == 0) {
            r->dir[i] = r->dir[r->dir_len - 1];
            r->dir_len--;
            return;
        }
    }
}

/* ---------- replay ---------- */

static int hash_eq(const unsigned char a[32], const unsigned char b[32]) {
    return memcmp(a, b, 32) == 0;
}

/* verify output bytes of a record against its recorded output_hash.
 * Returns 1 if verified, 0 if not. */
static int verify_output(pmcp_region_t *r, const jrec_t *rec) {
    upk_t u;
    uint32_t nn;
    unsigned char h[32];

    u.p = rec->payload; u.len = rec->payload_len; u.off = 0; u.err = 0;
    switch (rec->op) {
    case J_CREATE: {
        uint64_t off, len;
        const unsigned char *bp;
        (void)upk_name(&u, &nn); off = upk_u64(&u); len = upk_u64(&u);
        if (u.err) return 0;
        (void)nn;
        bp = heap_ptr(r, off, len);
        if (!bp) return 0;
        pmcp_sha256(bp, (size_t)len, h);
        return hash_eq(h, rec->output_hash);
    }
    case J_RESIZE: {
        uint64_t ooff, olen, noff, nlen;
        const unsigned char *bp;
        unsigned char *tmp;
        (void)upk_name(&u, &nn); ooff = upk_u64(&u); olen = upk_u64(&u);
        noff = upk_u64(&u); nlen = upk_u64(&u);
        if (u.err) return 0;
        (void)nn; (void)noff;
        bp = heap_ptr(r, ooff, olen);
        if (!bp) return 0;
        if (nlen <= olen) {
            pmcp_sha256(bp, (size_t)nlen, h);
        } else {
            tmp = malloc((size_t)nlen);
            if (!tmp) return 0;
            memcpy(tmp, bp, (size_t)olen);
            memset(tmp + olen, 0, (size_t)(nlen - olen));
            pmcp_sha256(tmp, (size_t)nlen, h);
            free(tmp);
        }
        return hash_eq(h, rec->output_hash);
    }
    case J_COPY: {
        uint64_t soff, slen, doff;
        const unsigned char *sp, *dp;
        (void)upk_name(&u, &nn); soff = upk_u64(&u); slen = upk_u64(&u); doff = upk_u64(&u);
        if (u.err) return 0;
        (void)nn;
        sp = heap_ptr(r, soff, slen); dp = heap_ptr(r, doff, slen);
        if (!sp || !dp) return 0;
        if (memcmp(sp, dp, (size_t)slen) != 0) return 0;
        pmcp_sha256(dp, (size_t)slen, h);
        return hash_eq(h, rec->output_hash);
    }
    case J_SLICE: {
        uint64_t soff, so, len, doff;
        const unsigned char *sp, *dp;
        (void)upk_name(&u, &nn); soff = upk_u64(&u); so = upk_u64(&u);
        len = upk_u64(&u); doff = upk_u64(&u);
        if (u.err) return 0;
        (void)nn;
        sp = heap_ptr(r, soff + so, len); dp = heap_ptr(r, doff, len);
        if (!sp || !dp) return 0;
        if (memcmp(sp, dp, (size_t)len) != 0) return 0;
        pmcp_sha256(dp, (size_t)len, h);
        return hash_eq(h, rec->output_hash);
    }
    case J_CONCAT: {
        uint64_t aoff, alen, boff, blen, doff;
        const unsigned char *ap, *bp2, *dp;
        (void)upk_name(&u, &nn);
        aoff = upk_u64(&u); alen = upk_u64(&u);
        boff = upk_u64(&u); blen = upk_u64(&u); doff = upk_u64(&u);
        if (u.err) return 0;
        (void)nn;
        ap = heap_ptr(r, aoff, alen); bp2 = heap_ptr(r, boff, blen);
        dp = heap_ptr(r, doff, alen + blen);
        if (!ap || !bp2 || !dp) return 0;
        if (memcmp(ap, dp, (size_t)alen) != 0) return 0;
        if (memcmp(bp2, dp + alen, (size_t)blen) != 0) return 0;
        pmcp_sha256(dp, (size_t)(alen + blen), h);
        return hash_eq(h, rec->output_hash);
    }
    case J_REINTERPRET: {
        uint64_t off, len;
        const unsigned char *bp;
        (void)upk_name(&u, &nn); off = upk_u64(&u); len = upk_u64(&u);
        if (u.err) return 0;
        (void)nn;
        bp = heap_ptr(r, off, len);
        if (!bp) return 0;
        pmcp_sha256(bp, (size_t)len, h);
        return hash_eq(h, rec->output_hash);
    }
    case J_HASH: {
        uint64_t soff, slen, doff;
        const unsigned char *sp, *dp;
        unsigned char hh[32];
        (void)upk_name(&u, &nn); soff = upk_u64(&u); slen = upk_u64(&u); doff = upk_u64(&u);
        if (u.err) return 0;
        (void)nn;
        sp = heap_ptr(r, soff, slen); dp = heap_ptr(r, doff, 32);
        if (!sp || !dp) return 0;
        pmcp_sha256(sp, (size_t)slen, hh);
        if (memcmp(hh, dp, 32) != 0) return 0;
        /* output extent holds the digest; its hash must equal output_hash */
        pmcp_sha256(dp, 32, h);
        return hash_eq(h, rec->output_hash);
    }
    default:
        return 0;
    }
}

/* apply one committed record to the in-memory directory (idempotent) */
int region_dir_apply(pmcp_region_t *r, const jrec_t *rec) {
    upk_t u;
    const unsigned char *np;
    uint32_t nn;
    char name[PMCP_NAME_MAX];
    struct extent_meta *m;
    uint64_t heap_off = 0, len = 0;
    uint32_t dtype = PMCP_DTYPE_RAW;

    if (!rec->committed)
        return 0;
    u.p = rec->payload; u.len = rec->payload_len; u.off = 0; u.err = 0;

    switch (rec->op) {
    case J_FENCE:
        return 0;
    case J_DELETE:
        np = upk_name(&u, &nn);
        if (u.err)
            return -1;
        memcpy(name, np, nn);
        name[nn] = '\0';
        dir_remove(r, name);
        return 0;
    case J_RESTORE:
        return 0; /* handled by the rebuild driver */
    default:
        break;
    }

    np = upk_name(&u, &nn);
    if (u.err)
        return -1;
    memcpy(name, np, nn);
    name[nn] = '\0';

    /* payload layouts (all LE, after the name):
     * CREATE:      heap_off u64, len u64, dtype u32
     * RESIZE:      old_off u64, old_len u64, new_off u64, new_len u64
     * COPY:        src_off u64, src_len u64, dst_off u64
     * SLICE:       src_off u64, src_start u64, len u64, dst_off u64
     * CONCAT:      a_off u64, a_len u64, b_off u64, b_len u64, dst_off u64
     * REINTERPRET: src_off u64, len u64, new_dtype u32
     * HASH:        src_off u64, src_len u64, dst_off u64 */
    switch (rec->op) {
    case J_CREATE:
        heap_off = upk_u64(&u);
        len = upk_u64(&u);
        dtype = upk_u32(&u);
        break;
    case J_RESIZE:
        (void)upk_u64(&u); /* old off */
        (void)upk_u64(&u); /* old len */
        heap_off = upk_u64(&u);
        len = upk_u64(&u);
        m = region_dir_find(r, name);
        dtype = m ? m->dtype : PMCP_DTYPE_RAW;
        break;
    case J_COPY: {
        uint64_t slen;
        (void)upk_u64(&u);
        slen = upk_u64(&u);
        heap_off = upk_u64(&u);
        len = slen;
        break;
    }
    case J_SLICE:
        (void)upk_u64(&u);
        (void)upk_u64(&u);
        len = upk_u64(&u);
        heap_off = upk_u64(&u);
        break;
    case J_CONCAT: {
        uint64_t alen, blen;
        (void)upk_u64(&u); alen = upk_u64(&u);
        (void)upk_u64(&u); blen = upk_u64(&u);
        heap_off = upk_u64(&u);
        len = alen + blen;
        break;
    }
    case J_REINTERPRET:
        heap_off = upk_u64(&u);
        len = upk_u64(&u);
        dtype = upk_u32(&u);
        break;
    case J_HASH:
        (void)upk_u64(&u);
        (void)upk_u64(&u);
        heap_off = upk_u64(&u);
        len = 32;
        break;
    default:
        return -1;
    }
    if (u.err)
        return -1;

    if (!verify_output(r, rec))
        return 0; /* unverifiable: skip, never install corrupt state */

    m = dir_insert(r, name);
    if (!m)
        return -1;
    m->heap_off = heap_off;
    m->len = len;
    m->dtype = dtype;
    memcpy(m->hash, rec->output_hash, 32);
    return 0;
}

/* ---------- rebuild driver ---------- */

typedef struct {
    uint64_t upto;
    uint64_t restore_seq;   /* seq of last committed RESTORE <= upto, or UINT64_MAX */
    uint64_t restore_snap;  /* its snap_seq */
} find_rs_t;

static int find_restore_cb(const jrec_t *rec, void *ctx) {
    find_rs_t *f = ctx;
    if (rec->op == J_RESTORE && rec->committed && rec->seq <= f->upto &&
        rec->payload_len == 8) {
        uint64_t s = 0;
        int i;
        for (i = 0; i < 8; i++)
            s |= (uint64_t)rec->payload[i] << (8 * i);
        if (rec->seq > f->restore_seq || f->restore_seq == UINT64_MAX) {
            f->restore_seq = rec->seq;
            f->restore_snap = s;
        }
    }
    return 0;
}

typedef struct {
    uint64_t upto;
    uint64_t after;   /* apply only records with seq > after */
    int have_after;   /* if 0, seq 0 is included */
    pmcp_region_t *r;
} apply_ctx_t;

static int apply_cb(const jrec_t *rec, void *ctx) {
    apply_ctx_t *a = ctx;
    if (rec->seq > a->upto)
        return 0;
    if (a->have_after && rec->seq <= a->after)
        return 0;
    if (!rec->committed || rec->op == J_RESTORE)
        return 0;
    region_dir_apply(a->r, rec);
    return 0;
}

int region_rebuild(pmcp_region_t *r, uint64_t upto_seq) {
    find_rs_t f;
    apply_ctx_t a;
    f.upto = upto_seq;
    f.restore_seq = UINT64_MAX;
    f.restore_snap = 0;
    journal_scan(r, find_restore_cb, &f);
    r->dir_len = 0;
    if (f.restore_seq != UINT64_MAX) {
        /* base state = rebuild(snap), then apply records after the restore */
        if (region_rebuild(r, f.restore_snap) != 0)
            return -1;
        a.upto = upto_seq;
        a.after = f.restore_seq;
        a.have_after = 1;
        a.r = r;
        journal_scan(r, apply_cb, &a);
        return 0;
    }
    a.upto = upto_seq;
    a.after = 0;
    a.have_after = 0;
    a.r = r;
    journal_scan(r, apply_cb, &a);
    return 0;
}

static int region_bump_scan(pmcp_region_t *r);

/* ---------- open / close ---------- */

pmcp_region_t *pmcp_open(const char *name) {
    pmcp_region_t *r;
    struct stat st;
    int is_new;
    unsigned char hdr[PMCP_HDR_SIZE];
    ssize_t n;

    if (!valid_name(name) || strchr(name, '/') != NULL)
        return NULL;
    r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    snprintf(r->data_path, sizeof(r->data_path), "%s.pmcp", name);
    snprintf(r->journal_path, sizeof(r->journal_path), "%s.pmcp.journal", name);

    is_new = stat(r->data_path, &st) != 0;
    r->data_fd = open(r->data_path, O_RDWR | O_CREAT, 0600);
    if (r->data_fd < 0)
        goto fail;
    r->journal_fd = open(r->journal_path, O_RDWR | O_CREAT | O_APPEND, 0600);
    if (r->journal_fd < 0)
        goto fail;

    if (is_new) {
        memset(hdr, 0, sizeof(hdr));
        hdr[0] = (unsigned char)(PMCP_DATA_MAGIC);
        hdr[1] = (unsigned char)(PMCP_DATA_MAGIC >> 8);
        hdr[2] = (unsigned char)(PMCP_DATA_MAGIC >> 16);
        hdr[3] = (unsigned char)(PMCP_DATA_MAGIC >> 24);
        hdr[4] = (unsigned char)(PMCP_DATA_VERSION);
        if (ftruncate(r->data_fd, 1 << 20) != 0)
            goto fail;
        if (pwrite(r->data_fd, hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr))
            goto fail;
        if (fsync(r->data_fd) != 0)
            goto fail;
    }
    r->map_size = 1 << 20;
    /* grow mapping to file size if larger */
    if (fstat(r->data_fd, &st) == 0 && (size_t)st.st_size > r->map_size)
        r->map_size = (size_t)st.st_size;
    r->base = mmap(NULL, r->map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   r->data_fd, 0);
    if (r->base == MAP_FAILED)
        goto fail;

    /* validate header */
    n = pread(r->data_fd, hdr, sizeof(hdr), 0);
    if (n != (ssize_t)sizeof(hdr))
        goto fail;
    {
        uint32_t magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                         ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        uint64_t hs = 0;
        int i;
        if (magic != PMCP_DATA_MAGIC)
            goto fail;
        for (i = 0; i < 8; i++)
            hs |= (uint64_t)hdr[8 + i] << (8 * i);
        r->heap_bump = hs;
    }

    r->next_seq = 0;
    /* rebuild directory from journal; also advances heap_bump past any
     * heap ranges referenced by complete records */
    if (region_rebuild(r, UINT64_MAX) != 0)
        goto fail;
    if (region_bump_scan(r) != 0)
        goto fail;
    return r;

fail:
    if (r->base && r->base != MAP_FAILED)
        munmap(r->base, r->map_size);
    if (r->data_fd >= 0)
        close(r->data_fd);
    if (r->journal_fd >= 0)
        close(r->journal_fd);
    free(r->dir);
    free(r);
    return NULL;
}

void pmcp_close(pmcp_region_t *r) {
    if (!r)
        return;
    msync(r->base, r->map_size, MS_SYNC);
    munmap(r->base, r->map_size);
    close(r->data_fd);
    close(r->journal_fd);
    free(r->dir);
    free(r);
}

/* recompute heap_bump = max(header heap_size, ends of heap ranges in journal) */
typedef struct {
    pmcp_region_t *r;
    uint64_t max_end;
} bump_ctx_t;

static int bump_cb(const jrec_t *rec, void *ctx) {
    bump_ctx_t *b = ctx;
    upk_t u;
    uint32_t nn;
    u.p = rec->payload; u.len = rec->payload_len; u.off = 0; u.err = 0;
#define TAKE_END(off, len) do { \
        if ((off) + (len) >= (off) && (off) + (len) > b->max_end) \
            b->max_end = (off) + (len); \
    } while (0)
    switch (rec->op) {
    case J_CREATE: {
        uint64_t off, len;
        (void)upk_name(&u, &nn); off = upk_u64(&u); len = upk_u64(&u);
        if (!u.err) TAKE_END(off, len);
        break;
    }
    case J_RESIZE: {
        uint64_t noff, nlen;
        (void)upk_name(&u, &nn);
        (void)upk_u64(&u); (void)upk_u64(&u);
        noff = upk_u64(&u); nlen = upk_u64(&u);
        if (!u.err) TAKE_END(noff, nlen);
        break;
    }
    case J_COPY: {
        uint64_t doff, slen;
        (void)upk_name(&u, &nn);
        (void)upk_u64(&u); slen = upk_u64(&u); doff = upk_u64(&u);
        if (!u.err) TAKE_END(doff, slen);
        break;
    }
    case J_SLICE: {
        uint64_t doff, len;
        (void)upk_name(&u, &nn);
        (void)upk_u64(&u); (void)upk_u64(&u); len = upk_u64(&u); doff = upk_u64(&u);
        if (!u.err) TAKE_END(doff, len);
        break;
    }
    case J_CONCAT: {
        uint64_t doff, alen, blen;
        (void)upk_name(&u, &nn);
        (void)upk_u64(&u); alen = upk_u64(&u);
        (void)upk_u64(&u); blen = upk_u64(&u); doff = upk_u64(&u);
        if (!u.err) TAKE_END(doff, alen + blen);
        break;
    }
    case J_HASH: {
        uint64_t doff;
        (void)upk_name(&u, &nn);
        (void)upk_u64(&u); (void)upk_u64(&u); doff = upk_u64(&u);
        if (!u.err) TAKE_END(doff, 32);
        break;
    }
    default:
        break;
    }
#undef TAKE_END
    (void)nn;
    return 0;
}

static int region_bump_scan(pmcp_region_t *r) {
    bump_ctx_t b;
    b.r = r;
    b.max_end = r->heap_bump;
    journal_scan(r, bump_cb, &b);
    if (b.max_end > r->heap_bump) {
        if (region_ensure_heap(r, b.max_end) != 0)
            return -1;
        r->heap_bump = b.max_end;
    }
    return 0;
}

/* ---------- two-phase commit for mutating ops ---------- */

int op_transact(pmcp_region_t *r, uint8_t jop,
                const unsigned char *pser, size_t pser_len,
                const unsigned char (*inputs)[32], uint32_t n_inputs,
                const unsigned char *payload, uint32_t payload_len,
                const unsigned char *heap_bytes, uint64_t heap_off, uint64_t heap_len,
                const unsigned char out_hash[32],
                pmcp_receipt_t *rc) {
    unsigned char phash[32];
    unsigned char zero_sig[64];
    jrec_t applied;
    uint64_t cseq;
    int ok = 0;

    memset(rc, 0, sizeof(*rc));
    memset(zero_sig, 0, sizeof(zero_sig));
    pmcp_sha256(pser ? pser : (const unsigned char *)"", pser_len, phash);

    /* phase 1: intent (before touching any bytes) */
    if (journal_append(r, jop, 0, phash, inputs, n_inputs,
                       payload, payload_len, out_hash, NULL) != 0) {
        rc->err = PMCP_EIO;
        return -1;
    }
    crash_hook("AFTER_INTENT");

    /* phase 2: heap bytes */
    if (heap_bytes && heap_len > 0) {
        if (region_heap_write(r, heap_off, heap_bytes, heap_len) != 0) {
            rc->err = PMCP_EIO;
            return -1;
        }
    }
    crash_hook("AFTER_HEAP");

    /* phase 3: commit */
    if (journal_append(r, jop, 1, phash, inputs, n_inputs,
                       payload, payload_len, out_hash, &cseq) != 0) {
        rc->err = PMCP_EIO;
        return -1;
    }
    crash_hook("AFTER_COMMIT");

    /* apply to in-memory directory (re-derive record view) */
    memset(&applied, 0, sizeof(applied));
    applied.op = jop;
    applied.committed = 1;
    applied.seq = cseq;
    applied.payload = (unsigned char *)payload;
    applied.payload_len = payload_len;
    memcpy(applied.output_hash, out_hash, 32);
    if (region_dir_apply(r, &applied) != 0) {
        rc->err = PMCP_EIO;
        return -1;
    }

    ok = 1;
    rc->ok = ok;
    rc->err = PMCP_OK;
    memcpy(rc->out_hash, out_hash, 32);
    rc->journal_seq = cseq;
    memcpy(rc->chamber_sig, zero_sig, sizeof(zero_sig)); /* milestone 2 hook */
    rc->timestamp_ns = now_ns();
    return 0;
}

/* ---------- extent API ---------- */

static pmcp_extent_t *extent_handle(pmcp_region_t *r, const char *name) {
    pmcp_extent_t *e = malloc(sizeof(*e));
    if (!e)
        return NULL;
    e->region = r;
    strncpy(e->name, name, PMCP_NAME_MAX - 1);
    e->name[PMCP_NAME_MAX - 1] = '\0';
    return e;
}

pmcp_extent_t *pmcp_extent(pmcp_region_t *r, const char *name) {
    if (!r || !valid_name(name))
        return NULL;
    if (!region_dir_find(r, name))
        return NULL;
    return extent_handle(r, name);
}

void pmcp_extent_free(pmcp_extent_t *e) {
    free(e);
}

pmcp_extent_t *pmcp_extent_create(pmcp_region_t *r, const char *name,
                                 const void *data, size_t len, uint32_t dtype) {
    pk_t pk, ps;
    unsigned char ohash[32];
    unsigned char phash_in[1] = {0};
    uint64_t off;
    pmcp_receipt_t rc;
    int err_code = PMCP_EINVAL;

    if (!r || !valid_name(name) || (len > 0 && !data))
        return NULL;
    if (region_dir_find(r, name))
        return NULL; /* EEXIST */

    if (pk_init(&pk) != 0 || pk_init(&ps) != 0) {
        free(pk.buf);
        return NULL;
    }
    /* canonical params for hashing: "create" + name + len + dtype */
    pk_u32(&ps, J_CREATE); pk_name(&ps, name);
    pk_u64(&ps, (uint64_t)len); pk_u32(&ps, dtype);
    /* payload: name, heap_off, len, dtype */
    if (region_heap_reserve(r, (uint64_t)len, &off) != 0) {
        err_code = PMCP_ENOMEM;
        goto out;
    }
    if (pk_name(&pk, name) != 0 || pk_u64(&pk, off) != 0 ||
        pk_u64(&pk, (uint64_t)len) != 0 || pk_u32(&pk, dtype) != 0) {
        err_code = PMCP_EINVAL;
        goto out;
    }
    pmcp_sha256(data ? data : phash_in, len, ohash);
    if (op_transact(r, J_CREATE, ps.buf, ps.len, NULL, 0,
                    pk.buf, (uint32_t)pk.len,
                    data, off, (uint64_t)len, ohash, &rc) != 0 || !rc.ok) {
        err_code = rc.err ? rc.err : PMCP_EIO;
        goto out;
    }
    free(pk.buf); free(ps.buf);
    return extent_handle(r, name);

out:
    free(pk.buf); free(ps.buf);
    (void)err_code;
    return NULL;
}

int pmcp_extent_delete(pmcp_region_t *r, const char *name) {
    pk_t pk, ps;
    unsigned char ohash[32];
    pmcp_receipt_t rc;

    if (!r || !valid_name(name))
        return PMCP_EINVAL;
    if (!region_dir_find(r, name))
        return PMCP_ENOENT;
    if (pk_init(&pk) != 0 || pk_init(&ps) != 0) {
        free(pk.buf);
        return PMCP_ENOMEM;
    }
    pk_u32(&ps, J_DELETE); pk_name(&ps, name);
    if (pk_name(&pk, name) != 0) {
        free(pk.buf); free(ps.buf);
        return PMCP_EINVAL;
    }
    memset(ohash, 0, sizeof(ohash));
    if (op_transact(r, J_DELETE, ps.buf, ps.len, NULL, 0,
                    pk.buf, (uint32_t)pk.len, NULL, 0, 0, ohash, &rc) != 0) {
        free(pk.buf); free(ps.buf);
        return PMCP_EIO;
    }
    free(pk.buf); free(ps.buf);
    return PMCP_OK;
}

int pmcp_extent_resize(pmcp_region_t *r, pmcp_extent_t *e, size_t new_len) {
    struct extent_meta *m;
    pk_t pk, ps;
    unsigned char ohash[32];
    unsigned char *nbuf = NULL;
    uint64_t noff = 0;
    pmcp_receipt_t rc;
    int ret = PMCP_EINVAL;

    if (!r || !e || e->region != r)
        return PMCP_EINVAL;
    m = region_dir_find(r, e->name);
    if (!m)
        return PMCP_ENOENT;
    if ((uint64_t)new_len == m->len)
        return PMCP_OK;

    if (pk_init(&pk) != 0 || pk_init(&ps) != 0) {
        free(pk.buf);
        return PMCP_ENOMEM;
    }
    pk_u32(&ps, J_RESIZE); pk_name(&ps, e->name); pk_u64(&ps, (uint64_t)new_len);

    if ((uint64_t)new_len > m->len) {
        /* grow: copy to new heap range, zero-fill */
        nbuf = malloc(new_len);
        if (!nbuf) {
            ret = PMCP_ENOMEM;
            goto out;
        }
        memcpy(nbuf, (unsigned char *)r->base + PMCP_HDR_SIZE + m->heap_off,
               (size_t)m->len);
        memset(nbuf + m->len, 0, new_len - (size_t)m->len);
        if (region_heap_reserve(r, (uint64_t)new_len, &noff) != 0) {
            ret = PMCP_ENOMEM;
            goto out;
        }
        pmcp_sha256(nbuf, new_len, ohash);
    } else {
        /* shrink: no heap write, same offset */
        noff = m->heap_off;
        pmcp_sha256((unsigned char *)r->base + PMCP_HDR_SIZE + m->heap_off,
                    new_len, ohash);
    }
    if (pk_name(&pk, e->name) != 0 || pk_u64(&pk, m->heap_off) != 0 ||
        pk_u64(&pk, m->len) != 0 || pk_u64(&pk, noff) != 0 ||
        pk_u64(&pk, (uint64_t)new_len) != 0) {
        ret = PMCP_EINVAL;
        goto out;
    }
    if (op_transact(r, J_RESIZE, ps.buf, ps.len, NULL, 0,
                    pk.buf, (uint32_t)pk.len,
                    nbuf, noff, (uint64_t)new_len, ohash, &rc) != 0) {
        ret = PMCP_EIO;
        goto out;
    }
    ret = PMCP_OK;
out:
    free(nbuf);
    free(pk.buf); free(ps.buf);
    return ret;
}

const void *pmcp_extent_ptr(const pmcp_extent_t *e) {
    struct extent_meta *m;
    if (!e)
        return NULL;
    m = region_dir_find(e->region, e->name);
    if (!m)
        return NULL;
    return (const unsigned char *)e->region->base + PMCP_HDR_SIZE + m->heap_off;
}

size_t pmcp_extent_len(const pmcp_extent_t *e) {
    struct extent_meta *m;
    if (!e)
        return 0;
    m = region_dir_find(e->region, e->name);
    return m ? (size_t)m->len : 0;
}

uint32_t pmcp_extent_dtype(const pmcp_extent_t *e) {
    struct extent_meta *m;
    if (!e)
        return PMCP_DTYPE_RAW;
    m = region_dir_find(e->region, e->name);
    return m ? m->dtype : PMCP_DTYPE_RAW;
}

const char *pmcp_extent_name(const pmcp_extent_t *e) {
    return e ? e->name : NULL;
}

int pmcp_extent_hash(const pmcp_extent_t *e, unsigned char out[32]) {
    struct extent_meta *m;
    if (!e)
        return -1;
    m = region_dir_find(e->region, e->name);
    if (!m)
        return -1;
    memcpy(out, m->hash, 32);
    return 0;
}

/* ---------- fence / snapshot / restore ---------- */

int pmcp_fence(pmcp_region_t *r) {
    unsigned char ohash[32];
    unsigned char ps[4] = { J_FENCE, 0, 0, 0 };
    pmcp_receipt_t rc;
    if (!r)
        return PMCP_EINVAL;
    memset(ohash, 0, sizeof(ohash));
    if (msync(r->base, r->map_size, MS_SYNC) != 0)
        return PMCP_EIO;
    if (op_transact(r, J_FENCE, ps, sizeof(ps), NULL, 0,
                    NULL, 0, NULL, 0, 0, ohash, &rc) != 0)
        return PMCP_EIO;
    return PMCP_OK;
}

pmcp_snap_t pmcp_snapshot(pmcp_region_t *r) {
    unsigned char ohash[32];
    unsigned char ps[4] = { J_FENCE, 0, 0, 0 };
    pmcp_receipt_t rc;
    if (!r)
        return 0;
    memset(ohash, 0, sizeof(ohash));
    msync(r->base, r->map_size, MS_SYNC);
    if (op_transact(r, J_FENCE, ps, sizeof(ps), NULL, 0,
                    NULL, 0, NULL, 0, 0, ohash, &rc) != 0 || !rc.ok)
        return 0;
    return rc.journal_seq;
}

int pmcp_restore(pmcp_region_t *r, pmcp_snap_t id) {
    pk_t ps;
    unsigned char ohash[32];
    unsigned char pl[8];
    pmcp_receipt_t rc;
    int i;
    if (!r || id >= r->next_seq)
        return PMCP_EINVAL;
    if (pk_init(&ps) != 0)
        return PMCP_ENOMEM;
    pk_u32(&ps, J_RESTORE);
    pk_u64(&ps, id);
    for (i = 0; i < 8; i++)
        pl[i] = (unsigned char)(id >> (8 * i));
    memset(ohash, 0, sizeof(ohash));
    if (op_transact(r, J_RESTORE, ps.buf, ps.len, NULL, 0,
                    pl, sizeof(pl), NULL, 0, 0, ohash, &rc) != 0) {
        free(ps.buf);
        return PMCP_EIO;
    }
    free(ps.buf);
    /* rebuild visible state through the restore record */
    return region_rebuild(r, UINT64_MAX) == 0 ? PMCP_OK : PMCP_EIO;
}
