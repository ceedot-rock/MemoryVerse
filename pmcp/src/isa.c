/* isa.c — ISA interpreter: COPY, SLICE, CONCAT, REINTERPRET, HASH, FENCE.
 *
 * Every op follows the same two-phase shape:
 *   1. validate inputs, compute output bytes (malloc)
 *   2. reserve heap, journal intent (fsync) -> write heap (msync)
 *      -> journal commit (fsync) -> install directory entry
 * See op_transact() in region.c.
 */
#include "pmcp_internal.h"

#include <stdlib.h>
#include <string.h>

static void receipt_fail(pmcp_receipt_t *rc, int err) {
    memset(rc, 0, sizeof(*rc));
    rc->err = err;
}

static int check_dst(pmcp_region_t *r, const char *dst) {
    if (!dst || strlen(dst) == 0 || strlen(dst) >= PMCP_NAME_MAX)
        return PMCP_EINVAL;
    if (region_dir_find(r, dst))
        return PMCP_EEXIST;
    return PMCP_OK;
}

static int meta_bytes(pmcp_region_t *r, struct extent_meta *m,
                      const unsigned char **out, uint64_t *len_out) {
    const unsigned char *p;
    if (m->len == 0) {
        *out = NULL;
        *len_out = 0;
        return PMCP_OK;
    }
    p = (const unsigned char *)r->base + PMCP_HDR_SIZE + m->heap_off;
    *out = p;
    *len_out = m->len;
    return PMCP_OK;
}

static pmcp_receipt_t do_copy(pmcp_region_t *r, struct extent_meta *sm,
                              const char *dst) {
    pk_t ps, pl;
    unsigned char ohash[32], inputs[1][32];
    const unsigned char *sp;
    uint64_t doff, slen = sm->len;
    pmcp_receipt_t rc;
    int ok = 1;

    if (pk_init(&ps) || pk_init(&pl)) { receipt_fail(&rc, PMCP_ENOMEM); return rc; }
    pk_u32(&ps, J_COPY); pk_name(&ps, dst); pk_name(&ps, sm->name);
    if (region_heap_reserve(r, slen, &doff)) ok = 0;
    if (ok && (pk_name(&pl, dst) || pk_u64(&pl, sm->heap_off) ||
               pk_u64(&pl, slen) || pk_u64(&pl, doff))) ok = 0;
    if (ok) {
        meta_bytes(r, sm, &sp, &slen);
        pmcp_sha256(sp ? sp : (const unsigned char *)"", (size_t)slen, ohash);
        memcpy(inputs[0], sm->hash, 32);
        if (op_transact(r, J_COPY, ps.buf, ps.len, inputs, 1,
                        pl.buf, (uint32_t)pl.len,
                        sp, doff, slen, ohash, &rc) != 0 || !rc.ok)
            ok = 0;
    } else {
        receipt_fail(&rc, PMCP_EIO);
    }
    pk_free(&ps); pk_free(&pl);
    (void)ok;
    return rc;
}

static pmcp_receipt_t do_slice(pmcp_region_t *r, struct extent_meta *sm,
                               const char *dst, uint64_t off, uint64_t len) {
    pk_t ps, pl;
    unsigned char ohash[32], inputs[1][32];
    const unsigned char *sp;
    uint64_t slen, doff;
    pmcp_receipt_t rc;

    if (len > sm->len || off > sm->len - len) {
        receipt_fail(&rc, PMCP_ERANGE);
        return rc;
    }
    if (pk_init(&ps) || pk_init(&pl)) { receipt_fail(&rc, PMCP_ENOMEM); return rc; }
    pk_u32(&ps, J_SLICE); pk_name(&ps, dst); pk_name(&ps, sm->name);
    pk_u64(&ps, off); pk_u64(&ps, len);
    if (region_heap_reserve(r, len, &doff)) {
        receipt_fail(&rc, PMCP_EIO);
        goto out;
    }
    if (pk_name(&pl, dst) || pk_u64(&pl, sm->heap_off) || pk_u64(&pl, off) ||
        pk_u64(&pl, len) || pk_u64(&pl, doff)) {
        receipt_fail(&rc, PMCP_EIO);
        goto out;
    }
    meta_bytes(r, sm, &sp, &slen);
    (void)slen;
    pmcp_sha256(sp ? sp + off : (const unsigned char *)"", (size_t)len, ohash);
    memcpy(inputs[0], sm->hash, 32);
    if (op_transact(r, J_SLICE, ps.buf, ps.len, inputs, 1,
                    pl.buf, (uint32_t)pl.len,
                    sp ? sp + off : NULL, doff, len, ohash, &rc) != 0 || !rc.ok)
        receipt_fail(&rc, PMCP_EIO);
out:
    pk_free(&ps); pk_free(&pl);
    return rc;
}

static pmcp_receipt_t do_concat(pmcp_region_t *r, struct extent_meta *am,
                                struct extent_meta *bm, const char *dst) {
    pk_t ps, pl;
    unsigned char ohash[32], inputs[2][32];
    const unsigned char *ap, *bp;
    uint64_t alen, blen, doff, total;
    unsigned char *tmp = NULL;
    pmcp_receipt_t rc;

    alen = am->len; blen = bm->len;
    if (alen + blen < alen) { receipt_fail(&rc, PMCP_ERANGE); return rc; }
    total = alen + blen;
    if (pk_init(&ps) || pk_init(&pl)) { receipt_fail(&rc, PMCP_ENOMEM); return rc; }
    pk_u32(&ps, J_CONCAT); pk_name(&ps, dst);
    pk_name(&ps, am->name); pk_name(&ps, bm->name);
    if (total > 0) {
        tmp = malloc((size_t)total);
        if (!tmp) { receipt_fail(&rc, PMCP_ENOMEM); goto out; }
        meta_bytes(r, am, &ap, &alen);
        meta_bytes(r, bm, &bp, &blen);
        if (alen) memcpy(tmp, ap, (size_t)alen);
        if (blen) memcpy(tmp + alen, bp, (size_t)blen);
    }
    if (region_heap_reserve(r, total, &doff)) {
        receipt_fail(&rc, PMCP_EIO);
        goto out;
    }
    if (pk_name(&pl, dst) || pk_u64(&pl, am->heap_off) || pk_u64(&pl, am->len) ||
        pk_u64(&pl, bm->heap_off) || pk_u64(&pl, bm->len) || pk_u64(&pl, doff)) {
        receipt_fail(&rc, PMCP_EIO);
        goto out;
    }
    pmcp_sha256(tmp ? tmp : (const unsigned char *)"", (size_t)total, ohash);
    memcpy(inputs[0], am->hash, 32);
    memcpy(inputs[1], bm->hash, 32);
    if (op_transact(r, J_CONCAT, ps.buf, ps.len, inputs, 2,
                    pl.buf, (uint32_t)pl.len,
                    tmp, doff, total, ohash, &rc) != 0 || !rc.ok)
        receipt_fail(&rc, PMCP_EIO);
out:
    free(tmp);
    pk_free(&ps); pk_free(&pl);
    return rc;
}

static pmcp_receipt_t do_reinterpret(pmcp_region_t *r, struct extent_meta *sm,
                                     const char *dst, uint32_t dtype) {
    pk_t ps, pl;
    unsigned char inputs[1][32];
    pmcp_receipt_t rc;

    if (pk_init(&ps) || pk_init(&pl)) { receipt_fail(&rc, PMCP_ENOMEM); return rc; }
    pk_u32(&ps, J_REINTERPRET); pk_name(&ps, dst); pk_name(&ps, sm->name);
    pk_u32(&ps, dtype);
    /* no heap write: dst aliases the source heap range */
    if (pk_name(&pl, dst) || pk_u64(&pl, sm->heap_off) ||
        pk_u64(&pl, sm->len) || pk_u32(&pl, dtype)) {
        receipt_fail(&rc, PMCP_EIO);
        goto out;
    }
    memcpy(inputs[0], sm->hash, 32);
    /* output hash == input hash: zero byte change */
    if (op_transact(r, J_REINTERPRET, ps.buf, ps.len, inputs, 1,
                    pl.buf, (uint32_t)pl.len,
                    NULL, sm->heap_off, 0, sm->hash, &rc) != 0 || !rc.ok)
        receipt_fail(&rc, PMCP_EIO);
out:
    pk_free(&ps); pk_free(&pl);
    return rc;
}

static pmcp_receipt_t do_hash(pmcp_region_t *r, struct extent_meta *sm,
                              const char *dst) {
    pk_t ps, pl;
    unsigned char digest[32], ohash[32], inputs[1][32];
    const unsigned char *sp;
    uint64_t slen, doff;
    pmcp_receipt_t rc;

    if (pk_init(&ps) || pk_init(&pl)) { receipt_fail(&rc, PMCP_ENOMEM); return rc; }
    pk_u32(&ps, J_HASH); pk_name(&ps, dst); pk_name(&ps, sm->name);
    meta_bytes(r, sm, &sp, &slen);
    pmcp_sha256(sp ? sp : (const unsigned char *)"", (size_t)slen, digest);
    if (region_heap_reserve(r, 32, &doff)) {
        receipt_fail(&rc, PMCP_EIO);
        goto out;
    }
    if (pk_name(&pl, dst) || pk_u64(&pl, sm->heap_off) ||
        pk_u64(&pl, sm->len) || pk_u64(&pl, doff)) {
        receipt_fail(&rc, PMCP_EIO);
        goto out;
    }
    pmcp_sha256(digest, 32, ohash); /* hash of the 32-byte digest extent */
    memcpy(inputs[0], sm->hash, 32);
    if (op_transact(r, J_HASH, ps.buf, ps.len, inputs, 1,
                    pl.buf, (uint32_t)pl.len,
                    digest, doff, 32, ohash, &rc) != 0 || !rc.ok)
        receipt_fail(&rc, PMCP_EIO);
out:
    pk_free(&ps); pk_free(&pl);
    return rc;
}

pmcp_receipt_t pmcp_exec(pmcp_extent_t *e, pmcp_op_t op, const void *params) {
    pmcp_receipt_t rc;
    pmcp_region_t *r;
    struct extent_meta *sm;
    int d;

    memset(&rc, 0, sizeof(rc));
    if (!e) {
        receipt_fail(&rc, PMCP_EINVAL);
        return rc;
    }
    r = e->region;
    sm = region_dir_find(r, e->name);
    if (op != PMCP_OP_FENCE && !sm) {
        receipt_fail(&rc, PMCP_ENOENT);
        return rc;
    }

    switch (op) {
    case PMCP_OP_COPY: {
        const pmcp_p_copy_t *p = params;
        if (!p) { receipt_fail(&rc, PMCP_EINVAL); break; }
        d = check_dst(r, p->dst);
        if (d != PMCP_OK) { receipt_fail(&rc, d); break; }
        rc = do_copy(r, sm, p->dst);
        break;
    }
    case PMCP_OP_SLICE: {
        const pmcp_p_slice_t *p = params;
        if (!p) { receipt_fail(&rc, PMCP_EINVAL); break; }
        d = check_dst(r, p->dst);
        if (d != PMCP_OK) { receipt_fail(&rc, d); break; }
        rc = do_slice(r, sm, p->dst, p->off, p->len);
        break;
    }
    case PMCP_OP_CONCAT: {
        const pmcp_p_concat_t *p = params;
        struct extent_meta *bm;
        if (!p) { receipt_fail(&rc, PMCP_EINVAL); break; }
        d = check_dst(r, p->dst);
        if (d != PMCP_OK) { receipt_fail(&rc, d); break; }
        bm = region_dir_find(r, p->second);
        if (!bm) { receipt_fail(&rc, PMCP_ENOENT); break; }
        if (bm == sm) { receipt_fail(&rc, PMCP_EINVAL); break; } /* use COPY for self */
        rc = do_concat(r, sm, bm, p->dst);
        break;
    }
    case PMCP_OP_REINTERPRET: {
        const pmcp_p_reinterpret_t *p = params;
        if (!p) { receipt_fail(&rc, PMCP_EINVAL); break; }
        d = check_dst(r, p->dst);
        if (d != PMCP_OK) { receipt_fail(&rc, d); break; }
        rc = do_reinterpret(r, sm, p->dst, p->dtype);
        break;
    }
    case PMCP_OP_HASH: {
        const pmcp_p_hash_t *p = params;
        if (!p) { receipt_fail(&rc, PMCP_EINVAL); break; }
        d = check_dst(r, p->dst);
        if (d != PMCP_OK) { receipt_fail(&rc, d); break; }
        rc = do_hash(r, sm, p->dst);
        break;
    }
    case PMCP_OP_FENCE:
        if (pmcp_fence(r) != PMCP_OK)
            receipt_fail(&rc, PMCP_EIO);
        else {
            rc.ok = 1;
            rc.err = PMCP_OK;
            rc.timestamp_ns = now_ns();
        }
        break;
    default:
        receipt_fail(&rc, PMCP_EINVAL);
        break;
    }
    return rc;
}
