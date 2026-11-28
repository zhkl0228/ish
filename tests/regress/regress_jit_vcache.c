// Regression test for the JIT's V register cache (ish -Djit=true; the
// results are checked with the gadgets too).
//
// Translated code ran each SIMD & FP instruction on host registers loaded
// from cpu->fp and stored its destination back, every source loaded again
// at every instruction: emitted twice, those loads made AES-128-GCM 22%
// slower. Within a translated segment a guest V register now stays in a host
// register once loaded, its every write stored through to cpu->fp at once.
// Forms that need consecutive registers (tbl's table, ld1-4/st1-4 of more
// than one, ld1's lane, ld1r) use scratch registers, copied from the held
// ones, and what they write is no longer held where it was.
//
// Checked, each probe run over and over (hot: translated), every result
// against C's:
// - twenty registers loaded and summed in one block, more than the cache
//   holds: the ones it let go are loaded again, as they were written;
// - a held register changed and stored by st1 (copied to the scratch
//   registers), then loaded again by ld1 of two, ld1's lane and ld1r, and
//   used: the new values; tbl's table from a held register; ldp and stp of
//   held registers; umov and fmov between held and general registers;
// - a held register written, then a load that misses the TLB (a page never
//   touched): the exit to the gadgets finds it in cpu->fp;
// - a loop block whose body carries two registers round its back edge;
// - scalar floating point: fadd, fmul, fmadd, fsub and fcmp one after the
//   other, the upper halves zeroed.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_jit_vcache regress_jit_vcache.c -lm

#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE 4096
#define ROUNDS 48   // calls per probe: well past the JIT's hot count (8 runs)

static int passed, failed;

static void check(const char *name, int ok, const char *detail) {
    if (ok) {
        passed++;
        printf("PASS %s\n", name);
    } else {
        failed++;
        printf("FAIL %s: %s\n", name, detail);
    }
    fflush(stdout);
}

typedef struct { uint32_t s[4]; } q_t;

// The probes keep to AAPCS64: d8..d15 saved where v8..v15 are used
__asm__(".text\n"
        ".p2align 2\n"
        // x0: q[20], x1: out q[2]. out[0] = q0 + ... + q19, out[1] = (q0 ^ q19) + q10
        ".global probe_evict\n"
        "probe_evict:\n"
        "    stp d8, d9, [sp, #-64]!\n"
        "    stp d10, d11, [sp, #16]\n"
        "    stp d12, d13, [sp, #32]\n"
        "    stp d14, d15, [sp, #48]\n"
        "    ldr q0, [x0, #0]\n"
        "    ldr q1, [x0, #16]\n"
        "    ldr q2, [x0, #32]\n"
        "    ldr q3, [x0, #48]\n"
        "    ldr q4, [x0, #64]\n"
        "    ldr q5, [x0, #80]\n"
        "    ldr q6, [x0, #96]\n"
        "    ldr q7, [x0, #112]\n"
        "    ldr q8, [x0, #128]\n"
        "    ldr q9, [x0, #144]\n"
        "    ldr q10, [x0, #160]\n"
        "    ldr q11, [x0, #176]\n"
        "    ldr q12, [x0, #192]\n"
        "    ldr q13, [x0, #208]\n"
        "    ldr q14, [x0, #224]\n"
        "    ldr q15, [x0, #240]\n"
        "    ldr q16, [x0, #256]\n"
        "    ldr q17, [x0, #272]\n"
        "    ldr q18, [x0, #288]\n"
        "    ldr q19, [x0, #304]\n"
        "    add v20.4s, v0.4s, v1.4s\n"
        "    add v20.4s, v20.4s, v2.4s\n"
        "    add v20.4s, v20.4s, v3.4s\n"
        "    add v20.4s, v20.4s, v4.4s\n"
        "    add v20.4s, v20.4s, v5.4s\n"
        "    add v20.4s, v20.4s, v6.4s\n"
        "    add v20.4s, v20.4s, v7.4s\n"
        "    add v20.4s, v20.4s, v8.4s\n"
        "    add v20.4s, v20.4s, v9.4s\n"
        "    add v20.4s, v20.4s, v10.4s\n"
        "    add v20.4s, v20.4s, v11.4s\n"
        "    add v20.4s, v20.4s, v12.4s\n"
        "    add v20.4s, v20.4s, v13.4s\n"
        "    add v20.4s, v20.4s, v14.4s\n"
        "    add v20.4s, v20.4s, v15.4s\n"
        "    add v20.4s, v20.4s, v16.4s\n"
        "    add v20.4s, v20.4s, v17.4s\n"
        "    add v20.4s, v20.4s, v18.4s\n"
        "    add v20.4s, v20.4s, v19.4s\n"
        "    eor v21.16b, v0.16b, v19.16b\n"
        "    add v21.4s, v21.4s, v10.4s\n"
        "    str q20, [x1]\n"
        "    str q21, [x1, #16]\n"
        "    ldp d14, d15, [sp, #48]\n"
        "    ldp d12, d13, [sp, #32]\n"
        "    ldp d10, d11, [sp, #16]\n"
        "    ldp d8, d9, [sp], #64\n"
        "    ret\n"
        // x0: q[6], x1: out q[10], x2: a word, x3: tbl's indices
        ".global probe_scratch\n"
        "probe_scratch:\n"
        "    stp d8, d9, [sp, #-64]!\n"
        "    stp d10, d11, [sp, #16]\n"
        "    stp d12, d13, [sp, #32]\n"
        "    stp d14, d15, [sp, #48]\n"
        "    ldr q2, [x0]\n"
        "    ldr q3, [x0, #16]\n"
        "    add v2.4s, v2.4s, v3.4s\n"
        "    st1 {v2.4s, v3.4s}, [x1]\n"
        "    add x4, x0, #32\n"
        "    ld1 {v2.4s, v3.4s}, [x4]\n"
        "    add v4.4s, v2.4s, v3.4s\n"
        "    str q4, [x1, #32]\n"
        "    ld1 {v4.s}[1], [x2]\n"
        "    str q4, [x1, #48]\n"
        "    add v7.16b, v4.16b, v4.16b\n"
        "    ldr q9, [x3]\n"
        "    tbl v8.16b, {v7.16b}, v9.16b\n"
        "    str q8, [x1, #64]\n"
        "    ld1r {v10.4s}, [x2]\n"
        "    add v11.4s, v10.4s, v7.4s\n"
        "    str q11, [x1, #80]\n"
        "    ldp q12, q13, [x0, #64]\n"
        "    add v12.4s, v12.4s, v13.4s\n"
        "    stp q13, q12, [x1, #96]\n"
        "    umov w5, v12.s[2]\n"
        "    str x5, [x1, #128]\n"
        "    fmov d14, x5\n"
        "    str q14, [x1, #144]\n"
        "    ldp d14, d15, [sp, #48]\n"
        "    ldp d12, d13, [sp, #32]\n"
        "    ldp d10, d11, [sp, #16]\n"
        "    ldp d8, d9, [sp], #64\n"
        "    ret\n"
        // x0: q, x1: out (q[3]), x2: a page never touched
        ".global probe_exit\n"
        "probe_exit:\n"
        "    ldr q0, [x0]\n"
        "    add v0.4s, v0.4s, v0.4s\n"
        "    ldr x5, [x2]\n"
        "    add v1.4s, v0.4s, v0.4s\n"
        "    str q1, [x1]\n"
        "    str x5, [x1, #16]\n"
        "    str q0, [x1, #32]\n"
        "    ret\n"
        // x0: q[2], x1: out q[2], x2: iterations (> 0). The count in x9, not
        // a pinned register: the JIT makes the loop a loop block, x9 in a
        // pinned one's host register
        ".global probe_loop\n"
        "probe_loop:\n"
        "    ldr q0, [x0]\n"
        "    ldr q1, [x0, #16]\n"
        "    mov x9, x2\n"
        "1:  add v0.4s, v0.4s, v1.4s\n"
        "    eor v1.16b, v1.16b, v0.16b\n"
        "    subs x9, x9, #1\n"
        "    b.ne 1b\n"
        "    str q0, [x1]\n"
        "    str q1, [x1, #16]\n"
        "    ret\n"
        // x0: double[2], x1: out (q[2], then x[2])
        ".global probe_fp\n"
        "probe_fp:\n"
        "    ldr d0, [x0]\n"
        "    ldr d1, [x0, #8]\n"
        "    fadd d2, d0, d1\n"
        "    fmul d3, d2, d0\n"
        "    fmadd d4, d3, d1, d2\n"
        "    fsub d5, d4, d0\n"
        "    fcmp d5, d2\n"
        "    cset x6, gt\n"
        "    str q5, [x1]\n"
        "    str q4, [x1, #16]\n"
        "    str x6, [x1, #32]\n"
        "    fmov x7, d3\n"
        "    str x7, [x1, #40]\n"
        "    ret\n");
void probe_evict(const q_t *in, q_t *out);
void probe_scratch(const q_t *in, q_t *out, const uint32_t *word, const uint8_t *idx);
void probe_exit(const q_t *in, q_t *out, const void *page);
void probe_loop(const q_t *in, q_t *out, uint64_t n);
void probe_fp(const double *in, uint64_t *out);

static uint64_t seed = 0x9E3779B97F4A7C15u;
static uint32_t rnd(void) {
    seed = seed * 6364136223846793005u + 1442695040888963407u;
    return (uint32_t) (seed >> 32);
}
static q_t q_rnd(void) {
    q_t q;
    for (int i = 0; i < 4; i++)
        q.s[i] = rnd();
    return q;
}
static q_t q_add(q_t a, q_t b) {
    for (int i = 0; i < 4; i++)
        a.s[i] += b.s[i];
    return a;
}
static q_t q_eor(q_t a, q_t b) {
    for (int i = 0; i < 4; i++)
        a.s[i] ^= b.s[i];
    return a;
}

// A probe's wrong results: how many, and the first's round and what it was
struct wrong {
    int n, round;
    char what[200];
};
static void wrong_q(struct wrong *w, int round, const char *name, q_t got, q_t want) {
    if (!memcmp(&got, &want, sizeof(got)) || w->n++)
        return;
    w->round = round;
    snprintf(w->what, sizeof(w->what), "%s %08x %08x %08x %08x, want %08x %08x %08x %08x", name, got.s[3], got.s[2],
             got.s[1], got.s[0], want.s[3], want.s[2], want.s[1], want.s[0]);
}
static void report(const char *name, struct wrong *w) {
    char detail[300];
    snprintf(detail, sizeof(detail), "%d results wrong in %d rounds; the first in round %d: %s", w->n, ROUNDS,
             w->round, w->what);
    check(name, w->n == 0, detail);
}

static void evict(void) {
    struct wrong w = {0};
    for (int r = 0; r < ROUNDS; r++) {
        q_t in[20], out[2], sum;
        for (int i = 0; i < 20; i++)
            in[i] = q_rnd();
        probe_evict(in, out);
        sum = in[0];
        for (int i = 1; i < 20; i++)
            sum = q_add(sum, in[i]);
        wrong_q(&w, r, "sum", out[0], sum);
        wrong_q(&w, r, "(q0 ^ q19) + q10", out[1], q_add(q_eor(in[0], in[19]), in[10]));
    }
    report("twenty registers in one block, more than the cache holds: every sum right", &w);
}

static void scratch(void) {
    struct wrong w = {0};
    for (int r = 0; r < ROUNDS; r++) {
        q_t in[6], out[10];
        uint32_t word = rnd();
        uint8_t idx[16];
        for (int i = 0; i < 6; i++)
            in[i] = q_rnd();
        for (int i = 0; i < 16; i++)
            idx[i] = (uint8_t) (rnd() % 20);   // 16..19: out of the table, 0
        memset(out, 0, sizeof(out));
        probe_scratch(in, out, &word, idx);
        q_t v2 = q_add(in[0], in[1]);
        wrong_q(&w, r, "st1 of a held register changed", out[0], v2);
        wrong_q(&w, r, "st1's second", out[1], in[1]);
        q_t v4 = q_add(in[2], in[3]);
        wrong_q(&w, r, "after ld1 of two", out[2], v4);
        v4.s[1] = word;
        wrong_q(&w, r, "after ld1's lane", out[3], v4);
        q_t v7;
        for (int i = 0; i < 16; i++)
            ((uint8_t *) &v7)[i] = (uint8_t) (2 * ((uint8_t *) &v4)[i]);
        q_t v8;
        for (int i = 0; i < 16; i++)
            ((uint8_t *) &v8)[i] = idx[i] < 16 ? ((uint8_t *) &v7)[idx[i]] : 0;
        wrong_q(&w, r, "tbl of a held register", out[4], v8);
        q_t v10 = {{word, word, word, word}};
        wrong_q(&w, r, "after ld1r", out[5], q_add(v10, v7));
        q_t v12 = q_add(in[4], in[5]);
        wrong_q(&w, r, "stp's first", out[6], in[5]);
        wrong_q(&w, r, "stp's second", out[7], v12);
        q_t x5 = {{v12.s[2], 0, 0, 0}};
        wrong_q(&w, r, "umov", out[8], x5);
        wrong_q(&w, r, "fmov d, x", out[9], x5);
    }
    report("st1 of a held register, ld1 of two, ld1's lane, ld1r, tbl, ldp, stp, umov, fmov: the values as written",
           &w);
}

static void exits(void) {
    enum { PAGES = ROUNDS };
    uint8_t *pages = mmap(NULL, PAGES * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED) {
        perror("mmap");
        failed++;
        return;
    }
    struct wrong w = {0};
    for (int r = 0; r < ROUNDS; r++) {
        q_t in = q_rnd(), out[3];
        probe_exit(&in, out, pages + r * PAGE);
        q_t v0 = q_add(in, in);
        wrong_q(&w, r, "after the exit", out[0], q_add(v0, v0));
        q_t zero = {{0}};
        out[1].s[2] = out[1].s[3] = 0;
        wrong_q(&w, r, "the untouched page's word", out[1], zero);
        wrong_q(&w, r, "the register written before the exit", out[2], v0);
    }
    report("a held register written, then a load that misses the TLB: the gadgets find it in cpu->fp", &w);
}

static void loop(void) {
    struct wrong w = {0};
    for (int r = 0; r < ROUNDS; r++) {
        q_t in[2] = {q_rnd(), q_rnd()}, out[2];
        uint64_t n = 1 + rnd() % 200;
        probe_loop(in, out, n);
        q_t v0 = in[0], v1 = in[1];
        for (uint64_t i = 0; i < n; i++) {
            v0 = q_add(v0, v1);
            v1 = q_eor(v1, v0);
        }
        wrong_q(&w, r, "v0", out[0], v0);
        wrong_q(&w, r, "v1", out[1], v1);
    }
    report("a loop block carrying two registers round its back edge: both right", &w);
}

static uint64_t bits(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

static void fp(void) {
    struct wrong w = {0};
    for (int r = 0; r < ROUNDS; r++) {
        double in[2] = {(double) (int32_t) rnd() / 7.0, (double) (int32_t) rnd() / 3.0};
        uint64_t out[6];
        probe_fp(in, out);
        double d2 = in[0] + in[1], d3 = d2 * in[0], d4 = fma(d3, in[1], d2), d5 = d4 - in[0];
        q_t got5, got4, want5 = {{(uint32_t) bits(d5), (uint32_t) (bits(d5) >> 32), 0, 0}},
                         want4 = {{(uint32_t) bits(d4), (uint32_t) (bits(d4) >> 32), 0, 0}};
        memcpy(&got5, out, 16);
        memcpy(&got4, out + 2, 16);
        wrong_q(&w, r, "fsub's d5 (q5)", got5, want5);
        wrong_q(&w, r, "fmadd's d4 (q4)", got4, want4);
        q_t gotx = {{(uint32_t) out[4], (uint32_t) (out[4] >> 32), (uint32_t) out[5], (uint32_t) (out[5] >> 32)}};
        q_t wantx = {{d5 > d2, 0, (uint32_t) bits(d3), (uint32_t) (bits(d3) >> 32)}};
        wrong_q(&w, r, "fcmp's cset, fmov x, d3", gotx, wantx);
    }
    report("fadd, fmul, fmadd, fsub, fcmp one after the other: every result, the upper halves zeroed", &w);
}

int main(void) {
    evict();
    scratch();
    exits();
    loop();
    fp();
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
