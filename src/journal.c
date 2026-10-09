/* journal.c — write-ahead log: append with fsync, scan with torn-tail stop. */
#include "pmcp_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void crash_hook(const char *point) {
    char env[64];
    snprintf(env, sizeof(env), "PMCP_CRASH_%s", point);
    if (getenv(env) != NULL) {
        fflush(NULL);
        _exit(42);
    }
}

static void put_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static void put_u64(unsigned char *p, uint64_t v) {
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static uint32_t get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const unsigned char *p) {
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* fixed header: magic(4) seq(8) op(1) committed(1) ts(8) phash(32) n_inputs(4) = 58 */
#define JHDR 58

int journal_append(pmcp_region_t *r, uint8_t op, uint8_t committed,
                   const unsigned char params_hash[32],
                   const unsigned char (*inputs)[32], uint32_t n_inputs,
                   const unsigned char *payload, uint32_t payload_len,
                   const unsigned char output_hash[32],
                   uint64_t *seq_out) {
    size_t total = JHDR + (size_t)n_inputs * 32 + 4 + payload_len + 32;
    unsigned char *buf = malloc(total);
    unsigned char *p;
    uint32_t i;
    ssize_t w;
    size_t done = 0;

    if (!buf)
        return -1;
    p = buf;
    put_u32(p, J_MAGIC); p += 4;
    put_u64(p, r->next_seq); p += 8;
    *p++ = op;
    *p++ = committed;
    put_u64(p, now_ns()); p += 8;
    memcpy(p, params_hash, 32); p += 32;
    put_u32(p, n_inputs); p += 4;
    for (i = 0; i < n_inputs; i++) {
        memcpy(p, inputs[i], 32);
        p += 32;
    }
    put_u32(p, payload_len); p += 4;
    if (payload_len > 0) {
        memcpy(p, payload, payload_len);
        p += payload_len;
    }
    memcpy(p, output_hash, 32); p += 32;

    while (done < total) {
        w = write(r->journal_fd, buf + done, total - done);
        if (w <= 0) {
            free(buf);
            return -1;
        }
        done += (size_t)w;
    }
    free(buf);
    if (fsync(r->journal_fd) != 0)
        return -1;
    if (seq_out)
        *seq_out = r->next_seq;
    r->next_seq++;
    return 0;
}

/* read exactly n bytes; returns 0 ok, 1 short (torn), -1 error */
static int read_full(int fd, unsigned char *buf, size_t n) {
    size_t done = 0;
    ssize_t rr;
    while (done < n) {
        rr = read(fd, buf + done, n - done);
        if (rr == 0)
            return 1; /* EOF: torn tail */
        if (rr < 0)
            return -1;
        done += (size_t)rr;
    }
    return 0;
}

uint64_t journal_scan(pmcp_region_t *r,
                      int (*cb)(const jrec_t *rec, void *ctx), void *ctx) {
    unsigned char hdr[JHDR];
    uint64_t count = 0;
    int rc;

    if (lseek(r->journal_fd, 0, SEEK_SET) < 0)
        return 0;
    for (;;) {
        jrec_t rec;
        memset(&rec, 0, sizeof(rec));
        rc = read_full(r->journal_fd, hdr, JHDR);
        if (rc != 0)
            break; /* torn tail or EOF: stop, do not trust further bytes */
        if (get_u32(hdr) != J_MAGIC)
            break; /* corruption: stop */
        rec.seq = get_u64(hdr + 4);
        rec.op = hdr[12];
        rec.committed = hdr[13];
        rec.timestamp_ns = get_u64(hdr + 14);
        memcpy(rec.params_hash, hdr + 22, 32);
        rec.n_inputs = get_u32(hdr + 54);
        if (rec.n_inputs > 16)
            break; /* sanity bound */
        if (rec.n_inputs > 0) {
            rec.inputs = malloc((size_t)rec.n_inputs * 32);
            if (!rec.inputs)
                break;
            rc = read_full(r->journal_fd, (unsigned char *)rec.inputs,
                           (size_t)rec.n_inputs * 32);
            if (rc != 0) {
                free(rec.inputs);
                break;
            }
        }
        {
            unsigned char pl[4];
            rc = read_full(r->journal_fd, pl, 4);
            if (rc != 0) {
                free(rec.inputs);
                break;
            }
            rec.payload_len = get_u32(pl);
        }
        if (rec.payload_len > (32u << 20)) {
            free(rec.inputs);
            break; /* sanity bound: 32MB */
        }
        if (rec.payload_len > 0) {
            rec.payload = malloc(rec.payload_len);
            if (!rec.payload) {
                free(rec.inputs);
                break;
            }
            rc = read_full(r->journal_fd, rec.payload, rec.payload_len);
            if (rc != 0) {
                free(rec.inputs);
                free(rec.payload);
                break;
            }
        }
        {
            unsigned char oh[32];
            rc = read_full(r->journal_fd, oh, 32);
            if (rc != 0) {
                free(rec.inputs);
                free(rec.payload);
                break;
            }
            memcpy(rec.output_hash, oh, 32);
        }
        /* keep next_seq ahead of everything seen */
        if (rec.seq >= r->next_seq)
            r->next_seq = rec.seq + 1;
        count++;
        if (cb(&rec, ctx) != 0) {
            free(rec.inputs);
            free(rec.payload);
            break;
        }
        free(rec.inputs);
        free(rec.payload);
    }
    return count;
}
