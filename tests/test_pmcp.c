/* test_pmcp.c — pmcp-core milestone 1 tests.
 * Run from the tests/ directory: region files land in a scratch dir.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../src/pmcp.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { \
        printf("FAIL line %d: %s\n", __LINE__, msg); \
        failures++; \
    } \
} while (0)

static void to_hex(const unsigned char h[32], char out[65]) {
    int i;
    for (i = 0; i < 32; i++)
        sprintf(out + 2 * i, "%02x", h[i]);
    out[64] = '\0';
}

static int hex_is(const unsigned char h[32], const char *expect) {
    char s[65];
    to_hex(h, s);
    return strcmp(s, expect) == 0;
}

/* ---------- 1. SHA-256 known vectors ---------- */
static void test_sha256(void) {
    unsigned char h[32];
    printf("[sha256]\n");
    pmcp_sha256("", 0, h);
    CHECK(hex_is(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
          "sha256(\"\")");
    pmcp_sha256("abc", 3, h);
    CHECK(hex_is(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "sha256(\"abc\")");
    pmcp_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
    CHECK(hex_is(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
          "sha256(56-byte vector)");
}

/* ---------- 2. extent CRUD + persistence ---------- */
static void test_crud(void) {
    pmcp_region_t *r;
    pmcp_extent_t *e, *e2;
    const char *msg = "hello persistent world";
    unsigned char h1[32], h2[32];

    printf("[crud]\n");
    r = pmcp_open("t_crud");
    CHECK(r != NULL, "open");
    e = pmcp_extent_create(r, "greet", msg, strlen(msg), PMCP_DTYPE_U8);
    CHECK(e != NULL, "create");
    CHECK(pmcp_extent_len(e) == strlen(msg), "len");
    CHECK(pmcp_extent_dtype(e) == PMCP_DTYPE_U8, "dtype");
    CHECK(memcmp(pmcp_extent_ptr(e), msg, strlen(msg)) == 0, "bytes");
    CHECK(pmcp_extent_hash(e, h1) == 0, "hash rc");

    /* duplicate create must fail */
    e2 = pmcp_extent_create(r, "greet", msg, 1, 0);
    CHECK(e2 == NULL, "duplicate create rejected");

    /* grow */
    CHECK(pmcp_extent_resize(r, e, 64) == PMCP_OK, "grow rc");
    CHECK(pmcp_extent_len(e) == 64, "grown len");
    CHECK(memcmp(pmcp_extent_ptr(e), msg, strlen(msg)) == 0, "grow preserves");
    {
        const unsigned char *p = pmcp_extent_ptr(e);
        size_t i;
        int zeros = 1;
        for (i = strlen(msg); i < 64; i++)
            if (p[i] != 0) zeros = 0;
        CHECK(zeros, "grow zero-fills");
    }
    /* shrink */
    CHECK(pmcp_extent_resize(r, e, 5) == PMCP_OK, "shrink rc");
    CHECK(pmcp_extent_len(e) == 5, "shrunk len");
    CHECK(memcmp(pmcp_extent_ptr(e), "hello", 5) == 0, "shrink keeps prefix");

    /* delete */
    CHECK(pmcp_extent_delete(r, "greet") == PMCP_OK, "delete rc");
    CHECK(pmcp_extent(r, "greet") == NULL, "gone after delete");
    CHECK(pmcp_extent_delete(r, "greet") == PMCP_ENOENT, "double delete ENOENT");

    /* persistence across close/reopen */
    e2 = pmcp_extent_create(r, "keep", "ABCDEF", 6, PMCP_DTYPE_RAW);
    CHECK(e2 != NULL, "create keep");
    pmcp_extent_free(e); pmcp_extent_free(e2);
    pmcp_close(r);

    r = pmcp_open("t_crud");
    CHECK(r != NULL, "reopen");
    e = pmcp_extent(r, "keep");
    CHECK(e != NULL, "keep survives reopen");
    CHECK(e && memcmp(pmcp_extent_ptr(e), "ABCDEF", 6) == 0, "keep bytes intact");
    CHECK(e && pmcp_extent_hash(e, h2) == 0 && memcmp(h1, h2, 32) != 0,
          "hash differs from old greet (sanity)");
    pmcp_extent_free(e);
    pmcp_close(r);
}

/* ---------- 3. ISA ops ---------- */
static void test_ops(void) {
    pmcp_region_t *r;
    pmcp_extent_t *a, *e;
    pmcp_receipt_t rc;
    unsigned char h[32];

    printf("[ops]\n");
    r = pmcp_open("t_ops");
    CHECK(r != NULL, "open");
    a = pmcp_extent_create(r, "a", "abcdefghij", 10, PMCP_DTYPE_U8);
    CHECK(a != NULL, "create a");

    /* COPY */
    {
        pmcp_p_copy_t p = { "b" };
        rc = pmcp_exec(a, PMCP_OP_COPY, &p);
        CHECK(rc.ok, "copy ok");
        e = pmcp_extent(r, "b");
        CHECK(e && pmcp_extent_len(e) == 10, "copy len");
        CHECK(e && memcmp(pmcp_extent_ptr(e), "abcdefghij", 10) == 0, "copy bytes");
        pmcp_extent_free(e);
    }
    /* SLICE */
    {
        pmcp_p_slice_t p = { "s", 2, 4 };
        rc = pmcp_exec(a, PMCP_OP_SLICE, &p);
        CHECK(rc.ok, "slice ok");
        e = pmcp_extent(r, "s");
        CHECK(e && pmcp_extent_len(e) == 4, "slice len");
        CHECK(e && memcmp(pmcp_extent_ptr(e), "cdef", 4) == 0, "slice bytes");
        pmcp_extent_free(e);
    }
    /* slice out of range */
    {
        pmcp_p_slice_t p = { "bad", 8, 5 };
        rc = pmcp_exec(a, PMCP_OP_SLICE, &p);
        CHECK(!rc.ok, "oob slice rejected");
    }
    /* CONCAT */
    {
        pmcp_p_concat_t p = { "c", "s" };
        rc = pmcp_exec(a, PMCP_OP_CONCAT, &p);
        CHECK(rc.ok, "concat ok");
        e = pmcp_extent(r, "c");
        CHECK(e && pmcp_extent_len(e) == 14, "concat len");
        CHECK(e && memcmp(pmcp_extent_ptr(e), "abcdefghijcdef", 14) == 0,
              "concat bytes");
        pmcp_extent_free(e);
    }
    /* REINTERPRET: no byte change, new dtype */
    {
        pmcp_p_reinterpret_t p = { "r", PMCP_DTYPE_U32 };
        const unsigned char *pa, *pr;
        pmcp_extent_hash(a, h);
        rc = pmcp_exec(a, PMCP_OP_REINTERPRET, &p);
        CHECK(rc.ok, "reinterpret ok");
        e = pmcp_extent(r, "r");
        CHECK(e && pmcp_extent_dtype(e) == PMCP_DTYPE_U32, "reinterpret dtype");
        pa = pmcp_extent_ptr(a); pr = pmcp_extent_ptr(e);
        CHECK(pa && pr && memcmp(pa, pr, 10) == 0, "reinterpret bytes identical");
        CHECK(memcmp(rc.out_hash, h, 32) == 0, "reinterpret hash == input hash");
        pmcp_extent_free(e);
    }
    /* HASH */
    {
        pmcp_p_hash_t p = { "h" };
        unsigned char expect[32];
        rc = pmcp_exec(a, PMCP_OP_HASH, &p);
        CHECK(rc.ok, "hash ok");
        e = pmcp_extent(r, "h");
        CHECK(e && pmcp_extent_len(e) == 32, "hash extent is 32 bytes");
        pmcp_sha256("abcdefghij", 10, expect);
        CHECK(e && memcmp(pmcp_extent_ptr(e), expect, 32) == 0,
              "hash bytes == sha256");
        pmcp_extent_free(e);
    }
    /* FENCE */
    CHECK(pmcp_fence(r) == PMCP_OK, "fence ok");
    {
        pmcp_receipt_t fr = pmcp_exec(a, PMCP_OP_FENCE, NULL);
        CHECK(fr.ok, "fence via exec ok");
    }
    /* receipt carries journal seq + zeroed chamber_sig (milestone 2 hook) */
    {
        pmcp_p_copy_t p = { "b2" };
        unsigned char z[64];
        memset(z, 0, sizeof(z));
        rc = pmcp_exec(a, PMCP_OP_COPY, &p);
        CHECK(rc.ok && rc.journal_seq > 0, "receipt has seq");
        CHECK(memcmp(rc.chamber_sig, z, 64) == 0, "chamber_sig zeroed (stub)");
        CHECK(rc.timestamp_ns > 0, "receipt timestamp");
    }
    pmcp_extent_free(a);
    pmcp_close(r);
}

/* ---------- 4. determinism ---------- */
static void build_seq(const char *region) {
    pmcp_region_t *r = pmcp_open(region);
    pmcp_extent_t *a;
    pmcp_receipt_t rc;
    pmcp_p_copy_t pc = { "c1" };
    pmcp_p_slice_t ps = { "c2", 1, 6 };
    pmcp_p_hash_t ph = { "c3" };
    int i;
    unsigned char *buf;

    buf = malloc(4096);
    for (i = 0; i < 4096; i++)
        buf[i] = (unsigned char)((i * 31 + 7) & 0xff);
    a = pmcp_extent_create(r, "base", buf, 4096, PMCP_DTYPE_U8);
    free(buf);
    rc = pmcp_exec(a, PMCP_OP_COPY, &pc);
    if (!rc.ok) { printf("build copy failed\n"); exit(1); }
    {
        pmcp_extent_t *c1 = pmcp_extent(r, "c1");
        rc = pmcp_exec(c1, PMCP_OP_SLICE, &ps);
        pmcp_extent_free(c1);
        if (!rc.ok) { printf("build slice failed\n"); exit(1); }
    }
    {
        pmcp_extent_t *c2 = pmcp_extent(r, "c2");
        rc = pmcp_exec(c2, PMCP_OP_HASH, &ph);
        pmcp_extent_free(c2);
        if (!rc.ok) { printf("build hash failed\n"); exit(1); }
    }
    pmcp_fence(r);
    pmcp_extent_free(a);
    pmcp_close(r);
}

/* digest of a region's whole directory state */
static void region_digest(const char *region, unsigned char out[32]) {
    /* reopen read-only-ish and hash (name,len,bytes-hash) for each extent.
     * We enumerate via a fixed name list used by build_seq. */
    static const char *names[] = { "base", "c1", "c2", "c3" };
    pmcp_region_t *r = pmcp_open(region);
    unsigned char acc[32];
    size_t i;
    memset(acc, 0, sizeof(acc));
    for (i = 0; i < 4; i++) {
        pmcp_extent_t *e = pmcp_extent(r, names[i]);
        unsigned char h[32], mix[128];
        size_t nl;
        if (!e) {
            printf("region_digest: missing %s\n", names[i]);
            exit(1);
        }
        pmcp_extent_hash(e, h);
        nl = strlen(names[i]);
        memcpy(mix, acc, 32);
        memcpy(mix + 32, names[i], nl);
        memcpy(mix + 32 + nl, h, 32);
        {
            uint64_t L = pmcp_extent_len(e);
            int k;
            for (k = 0; k < 8; k++)
                mix[32 + nl + 32 + k] = (unsigned char)(L >> (8 * k));
        }
        pmcp_sha256(mix, 32 + nl + 32 + 8, acc);
        pmcp_extent_free(e);
    }
    pmcp_close(r);
    memcpy(out, acc, 32);
}

static void test_determinism(void) {
    unsigned char d1[32], d2[32];
    printf("[determinism]\n");
    build_seq("t_det_a");
    build_seq("t_det_b");
    region_digest("t_det_a", d1);
    region_digest("t_det_b", d2);
    CHECK(memcmp(d1, d2, 32) == 0, "identical op sequences -> identical state");
}

/* ---------- 5. crash recovery ---------- */

static void child_try_crash(const char *region, const char *hook) {
    pid_t pid;
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        pmcp_region_t *r;
        pmcp_extent_t *e;
        pmcp_p_copy_t p = { "victim" };
        pmcp_receipt_t rc;
        setenv(hook, "1", 1);
        r = pmcp_open(region);
        if (!r)
            _exit(10);
        e = pmcp_extent(r, "base");
        if (!e)
            _exit(11);
        rc = pmcp_exec(e, PMCP_OP_COPY, &p);
        (void)rc;
        pmcp_extent_free(e);
        pmcp_close(r);
        _exit(12); /* hook did not fire: unexpected */
    } else if (pid > 0) {
        int st;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42, "child died via crash hook");
    } else {
        CHECK(0, "fork failed");
    }
}

static void test_crash(void) {
    pmcp_region_t *r;
    pmcp_extent_t *e;
    printf("[crash]\n");

    /* crash AFTER_INTENT: torn op must be invisible, prior state intact */
    r = pmcp_open("t_crash1");
    e = pmcp_extent_create(r, "base", "0123456789", 10, 0);
    CHECK(e != NULL, "crash1 base");
    pmcp_extent_free(e);
    pmcp_close(r);
    child_try_crash("t_crash1", "PMCP_CRASH_AFTER_INTENT");
    r = pmcp_open("t_crash1");
    CHECK(pmcp_extent(r, "victim") == NULL, "intent-crash: torn op invisible");
    e = pmcp_extent(r, "base");
    CHECK(e && memcmp(pmcp_extent_ptr(e), "0123456789", 10) == 0,
          "intent-crash: prior bytes intact");
    pmcp_extent_free(e);
    pmcp_close(r);

    /* crash AFTER_HEAP: same expectation */
    r = pmcp_open("t_crash2");
    e = pmcp_extent_create(r, "base", "0123456789", 10, 0);
    pmcp_extent_free(e);
    pmcp_close(r);
    child_try_crash("t_crash2", "PMCP_CRASH_AFTER_HEAP");
    r = pmcp_open("t_crash2");
    CHECK(pmcp_extent(r, "victim") == NULL, "heap-crash: torn op invisible");
    e = pmcp_extent(r, "base");
    CHECK(e && memcmp(pmcp_extent_ptr(e), "0123456789", 10) == 0,
          "heap-crash: prior bytes intact");
    pmcp_extent_free(e);
    pmcp_close(r);

    /* crash AFTER_COMMIT: op must survive via replay */
    r = pmcp_open("t_crash3");
    e = pmcp_extent_create(r, "base", "0123456789", 10, 0);
    pmcp_extent_free(e);
    pmcp_close(r);
    child_try_crash("t_crash3", "PMCP_CRASH_AFTER_COMMIT");
    r = pmcp_open("t_crash3");
    e = pmcp_extent(r, "victim");
    CHECK(e != NULL, "commit-crash: op replayed");
    CHECK(e && memcmp(pmcp_extent_ptr(e), "0123456789", 10) == 0,
          "commit-crash: replayed bytes correct");
    pmcp_extent_free(e);
    pmcp_close(r);
}

/* ---------- 6. snapshot / restore ---------- */
static void test_snapshot(void) {
    pmcp_region_t *r;
    pmcp_extent_t *e;
    pmcp_snap_t s;
    printf("[snapshot]\n");
    r = pmcp_open("t_snap");
    e = pmcp_extent_create(r, "a", "aaa", 3, 0);
    pmcp_extent_free(e);
    s = pmcp_snapshot(r);
    CHECK(s > 0, "snapshot returns seq");
    e = pmcp_extent_create(r, "b", "bbb", 3, 0);
    pmcp_extent_free(e);
    CHECK(pmcp_extent(r, "b") != NULL, "b visible pre-restore");
    CHECK(pmcp_restore(r, s) == PMCP_OK, "restore rc");
    {
        pmcp_extent_t *b = pmcp_extent(r, "b");
        CHECK(b == NULL, "b gone after restore");
        if (b) pmcp_extent_free(b);
    }
    e = pmcp_extent(r, "a");
    CHECK(e && memcmp(pmcp_extent_ptr(e), "aaa", 3) == 0, "a intact after restore");
    pmcp_extent_free(e);
    pmcp_close(r);
    /* restore survives reopen (journal-driven) */
    r = pmcp_open("t_snap");
    {
        pmcp_extent_t *b = pmcp_extent(r, "b");
        CHECK(b == NULL, "b still gone after reopen");
        if (b) pmcp_extent_free(b);
    }
    e = pmcp_extent(r, "a");
    CHECK(e && memcmp(pmcp_extent_ptr(e), "aaa", 3) == 0, "a intact after reopen");
    pmcp_extent_free(e);
    pmcp_close(r);
}

/* ---------- 7. benchmark: COPY 100MB ---------- */
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void test_bench(void) {
    pmcp_region_t *r;
    pmcp_extent_t *big, *e;
    pmcp_receipt_t rc;
    unsigned char *buf;
    double t0, t1, secs, mbs;
    const size_t N = 100 * 1024 * 1024;
    size_t i;

    printf("[bench]\n");
    buf = malloc(N);
    CHECK(buf != NULL, "bench malloc");
    if (!buf) return;
    for (i = 0; i < N; i++)
        buf[i] = (unsigned char)(i * 2654435761u >> 16);
    r = pmcp_open("t_bench");
    big = pmcp_extent_create(r, "big", buf, N, PMCP_DTYPE_U8);
    free(buf);
    CHECK(big != NULL, "bench create 100MB");

    t0 = now_s();
    {
        pmcp_p_copy_t p = { "big2" };
        rc = pmcp_exec(big, PMCP_OP_COPY, &p);
    }
    t1 = now_s();
    CHECK(rc.ok, "bench copy ok");
    secs = t1 - t0;
    mbs = (double)N / (1024.0 * 1024.0) / (secs > 0 ? secs : 1e-9);
    printf("  COPY 100MB: %.3f s  (%.1f MB/s, journaled+fsync'd)\n", secs, mbs);

    t0 = now_s();
    {
        pmcp_p_hash_t p = { "bigh" };
        rc = pmcp_exec(big, PMCP_OP_HASH, &p);
    }
    t1 = now_s();
    CHECK(rc.ok, "bench hash ok");
    secs = t1 - t0;
    mbs = (double)N / (1024.0 * 1024.0) / (secs > 0 ? secs : 1e-9);
    printf("  HASH 100MB: %.3f s  (%.1f MB/s)\n", secs, mbs);

    e = pmcp_extent(r, "big2");
    CHECK(e && pmcp_extent_len(e) == N, "bench copy len");
    pmcp_extent_free(e);
    pmcp_extent_free(big);
    pmcp_close(r);
}

int main(void) {
    /* fresh scratch dir for region files every run */
    {
        int sret = system("rm -rf crashwork");
        (void)sret;
    }
    mkdir("crashwork", 0700);
    if (chdir("crashwork") != 0) {
        printf("cannot chdir to crashwork\n");
        return 1;
    }

    test_sha256();
    test_crud();
    test_ops();
    test_determinism();
    test_crash();
    test_snapshot();
    test_bench();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
