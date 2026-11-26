// Regression test for the JIT's scalar floating point (ish -Djit=true, the
// JIT position-independent, as on iOS; skips otherwise).
//
// The JIT left every scalar FP instruction to the gadgets: a block with one
// ran natively up to it and through the gadgets from there, each time round
// a loop. busybox awk, whose numbers are doubles, ran no faster with the JIT
// than with the AOT images alone (a loop of 400000 additions and modulos:
// 1.98 s and 2.00 s on the phone). It translates them now, the host's own
// instruction on the guest's registers, under the guest's FPCR, its flags
// into the guest's FPSR, as the gadgets run it: single and double precision,
// 1, 2 and 3 sources, fmov's immediate, fcmp, fccmp, fcsel, the conversions
// to and from integers and AdvSIMD's scalar forms of them (those the
// gadgets have: fcvtzs/fcvtzu, scvtf/ucvtf), fmov of a
// register's upper half, mrs of FPCR and FPSR.
//
// Checked: each instruction, put in a copy of a template made at run time,
// run once in a fresh copy (the gadgets: a block is translated once it is
// hot) and in a copy run hot (the JIT), for 8 sets of operands (normal,
// subnormal, zeros, infinities, quiet and signalling NaNs, values that
// overflow or saturate a conversion) under 4 FPCRs (round to nearest, up and
// to zero, and flush to zero with default NaN): q0..q3 whole, x9, x10, NZCV
// and FPSR the same. And some results against their known values.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o regress_jit_fp regress_jit_fp.c

#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

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

// What the template reads and writes
struct io {
    uint64_t q[4][2];    // q0..q3
    uint64_t x9, x10;
    uint64_t f1, f2;     // cmp f1, f2: the NZCV fccmp and fcsel read
    uint64_t oq[4][2];   // q0..q3 after
    uint64_t ox9, ox10;
    uint64_t nzcv, fpsr;
};
_Static_assert(offsetof(struct io, oq) == 96 && offsetof(struct io, nzcv) == 176, "the template's offsets");

// The template: the operands in, the instruction under test at its slot (a
// nop here), all it can change out. The JIT translates each of its other
// instructions, so in a hot copy the one under test runs natively.
__asm__(".text\n"
        ".p2align 2\n"
        ".global fp_template\n"
        "fp_template:\n"
        "    ldp q0, q1, [x0]\n"
        "    ldp q2, q3, [x0, #32]\n"
        "    ldp x9, x10, [x0, #64]\n"
        "    ldp x13, x14, [x0, #80]\n"
        "    cmp x13, x14\n"
        ".global fp_template_slot\n"
        "fp_template_slot:\n"
        "    nop\n"
        "    cset x11, mi\n"
        "    cset x12, eq\n"
        "    cset x13, hs\n"
        "    cset x14, vs\n"
        "    lsl x11, x11, #3\n"
        "    orr x11, x11, x12, lsl #2\n"
        "    orr x11, x11, x13, lsl #1\n"
        "    orr x11, x11, x14\n"
        "    mrs x15, fpsr\n"
        "    stp q0, q1, [x0, #96]\n"
        "    stp q2, q3, [x0, #128]\n"
        "    stp x9, x10, [x0, #160]\n"
        "    stp x11, x15, [x0, #176]\n"
        "    ret\n"
        ".global fp_template_end\n"
        "fp_template_end:\n");
extern const uint32_t fp_template[], fp_template_slot[], fp_template_end[];

// The guest's FPCR, FPSR cleared: in a function of its own, a block apart
// from the template's
static __attribute__((noinline)) void set_fp(uint64_t fpcr) {
    __asm__ volatile("msr fpcr, %0\n\tmsr fpsr, xzr" : : "r"(fpcr));
}

static uint32_t *code;
static size_t code_used, code_words;

// A copy of the template with insn in its slot, at an address of its own
#define COPY_STRIDE 32
static void *copy(uint32_t insn) {
    size_t n = (size_t) (fp_template_end - fp_template), slot = (size_t) (fp_template_slot - fp_template);
    if (code_used + COPY_STRIDE > code_words) {
        printf("FAIL the code buffer: %zu words, full\n", code_words);
        exit(1);
    }
    uint32_t *p = code + code_used;
    code_used += COPY_STRIDE;
    memcpy(p, fp_template, n * 4);
    p[slot] = insn;
    __builtin___clear_cache((char *) p, (char *) (p + n));
    return p;
}

static void run(void *fn, const struct io *in, uint64_t fpcr, struct io *out) {
    *out = *in;
    set_fp(fpcr);
    ((void (*)(struct io *)) fn)(out);
    set_fp(0);
}

// ---- encoders (rd, rn, rm, ra: register numbers; t: 0 single, 1 double)
static uint32_t fp2(unsigned op, unsigned t, unsigned d, unsigned n, unsigned m) {
    return 0x1E200800u | t << 22 | m << 16 | op << 12 | n << 5 | d;
}
static uint32_t fp1(unsigned op, unsigned t, unsigned d, unsigned n) {
    return 0x1E204000u | t << 22 | op << 15 | n << 5 | d;
}
static uint32_t fp3(unsigned o1, unsigned o0, unsigned t, unsigned d, unsigned n, unsigned m, unsigned a) {
    return 0x1F000000u | t << 22 | o1 << 21 | m << 16 | o0 << 15 | a << 10 | n << 5 | d;
}
static uint32_t fimm(unsigned t, unsigned imm8, unsigned d) {
    return 0x1E201000u | t << 22 | imm8 << 13 | d;
}
static uint32_t fcmp(unsigned t, unsigned n, unsigned m, unsigned opc2) {
    return 0x1E202000u | t << 22 | m << 16 | n << 5 | opc2;
}
static uint32_t fccmp(unsigned t, unsigned n, unsigned m, unsigned cond, unsigned e, unsigned nzcv) {
    return 0x1E200400u | t << 22 | m << 16 | cond << 12 | n << 5 | e << 4 | nzcv;
}
static uint32_t fcsel(unsigned t, unsigned d, unsigned n, unsigned m, unsigned cond) {
    return 0x1E200C00u | t << 22 | m << 16 | cond << 12 | n << 5 | d;
}
static uint32_t cvt(unsigned sf, unsigned t, unsigned rmode, unsigned op, unsigned d, unsigned n) {
    return sf << 31 | 0x1E200000u | t << 22 | rmode << 19 | op << 16 | n << 5 | d;
}
static uint32_t scalar_misc(unsigned u, unsigned a, unsigned sz, unsigned op, unsigned d, unsigned n) {
    return 0x5E200800u | u << 29 | a << 23 | sz << 22 | op << 12 | n << 5 | d;
}

struct insn {
    const char *group;
    uint32_t w;
};
static struct insn insns[256];
static int ninsns;

static void add(const char *group, uint32_t w) {
    insns[ninsns].group = group;
    insns[ninsns].w = w;
    ninsns++;
}

static void make_insns(void) {
    for (unsigned t = 0; t < 2; t++) {
        for (unsigned op = 0; op <= 8; op++)   // fmul fdiv fadd fsub fmax fmin fmaxnm fminnm fnmul
            add("2 sources", fp2(op, t, 3, 0, 1));
        add("2 sources", fp2(3, t, 0, 2, 3));   // fsub, other registers
        add("2 sources", fp2(1, t, 2, 3, 0));   // fdiv, other registers
        static const unsigned one[] = {0, 1, 2, 3, 8, 9, 10, 11, 12, 14, 15};   // fmov .. fsqrt, frint*
        for (unsigned i = 0; i < sizeof(one) / sizeof(one[0]); i++)
            add("1 source", fp1(one[i], t, 3, 0));
        add("1 source", fp1(3, t, 1, 2));        // fsqrt, other registers
        add("1 source", fp1(7, t, 3, 0));        // fcvt to half
        for (unsigned o = 0; o < 4; o++)         // fmadd fmsub fnmadd fnmsub
            add("3 sources", fp3(o >> 1, o & 1, t, 3, 0, 1, 2));
        add("3 sources", fp3(0, 1, t, 0, 3, 2, 1));
        add("fmov immediate", fimm(t, 0x70, 3));   // 1.0
        add("fmov immediate", fimm(t, 0x81, 1));
        static const unsigned opc2[] = {0x00, 0x10, 0x08, 0x18};   // fcmp, fcmpe, with #0.0
        for (unsigned i = 0; i < 4; i++)
            add("fcmp", fcmp(t, 0, 1, opc2[i]));
        add("fcmp", fcmp(t, 2, 3, 0));
        add("fccmp", fccmp(t, 0, 1, 0, 0, 0x5));    // eq
        add("fccmp", fccmp(t, 0, 1, 1, 1, 0xA));    // ne, fccmpe
        add("fccmp", fccmp(t, 2, 0, 8, 0, 0x3));    // hi
        add("fcsel", fcsel(t, 3, 0, 1, 0));         // eq
        add("fcsel", fcsel(t, 3, 0, 1, 8));         // hi
        add("fcsel", fcsel(t, 2, 1, 0, 11));        // lt
        for (unsigned sf = 0; sf < 2; sf++) {
            static const unsigned to[][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}, {2, 0}, {2, 1}, {3, 0}, {3, 1}, {0, 4}, {0, 5}};
            for (unsigned i = 0; i < 10; i++)   // fcvtns/nu, fcvtps/pu, fcvtms/mu, fcvtzs/zu, fcvtas/au
                add("to an integer", cvt(sf, t, to[i][0], to[i][1], 9, 0));
            add("to an integer", cvt(sf, t, 3, 0, 31, 2));   // fcvtzs to zr: its flags only
            add("to an integer", cvt(sf, t, 3, 1, 10, 1));
            add("from an integer", cvt(sf, t, 0, 2, 3, 9));    // scvtf
            add("from an integer", cvt(sf, t, 0, 3, 3, 10));   // ucvtf
            add("from an integer", cvt(sf, t, 0, 2, 1, 31));   // scvtf of zr
        }
        for (unsigned u = 0; u < 2; u++) {   // the forms the gadgets have
            add("AdvSIMD scalar conversions", scalar_misc(u, 1, t, 0x1B, 3, 0));   // fcvtzs/zu
            add("AdvSIMD scalar conversions", scalar_misc(u, 0, t, 0x1D, 3, 0));   // scvtf/ucvtf
        }
    }
    add("mrs FPCR, FPSR", 0xD53B4409u);   // mrs x9, fpcr
    add("mrs FPCR, FPSR", 0xD53B442Au);   // mrs x10, fpsr
    add("fmov of the upper half", 0x9EAF0123u);   // fmov v3.d[1], x9
    add("fmov of the upper half", 0x9EAE000Au);   // fmov x10, v0.d[1]
}

// ---- operands: q0..q3 as doubles and as singles (each with bits above it
// set), x9, x10, and cmp's pair
struct operands {
    uint64_t d[4];
    uint32_t s[4];
    uint64_t x9, x10, f1, f2;
};
static const struct operands sets[] = {
    // 1.5, -2.25, 0.1, 3.0; cmp 5, 5: eq
    {{0x3FF8000000000000, 0xC002000000000000, 0x3FB999999999999A, 0x4008000000000000},
     {0x3FC00000, 0xC0100000, 0x3DCCCCCD, 0x40400000}, (uint64_t) -7, 3, 5, 5},
    // 0.1, 3.0, -0.0, 0.001; cmp 1, 2: lt (N)
    {{0x3FB999999999999A, 0x4008000000000000, 0x8000000000000000, 0x3F50624DD2F1A9FC},
     {0x3DCCCCCD, 0x40400000, 0x80000000, 0x3A83126F}, INT64_MAX, 1, 1, 2},
    // inf, -inf, 1.0, 2.5; cmp 2, 1: hi (C)
    {{0x7FF0000000000000, 0xFFF0000000000000, 0x3FF0000000000000, 0x4004000000000000},
     {0x7F800000, 0xFF800000, 0x3F800000, 0x40200000}, 0x80000000, (uint64_t) -1, 2, 1},
    // a quiet NaN, 1.0, a signalling NaN, 0.0; cmp INT64_MIN, 1: V
    {{0x7FF8000000000123, 0x3FF0000000000000, 0x7FF0000000000456, 0},
     {0x7FC00123, 0x3F800000, 0x7F800456, 0}, 0, 0, (uint64_t) INT64_MIN, 1},
    // the least subnormal, 1e308, 2.0, -1e-310
    {{0x0000000000000001, 0x7FE1CCF385EBC8A0, 0x4000000000000000, 0x800012688B70E62B},
     {0x00000001, 0x7F7FFFFF, 0x40000000, 0x80000400}, 1, (1ull << 53) + 1, 0, 0},
    // -0.0, 0.0, 1e-310 (subnormal), 7.0
    {{0x8000000000000000, 0, 0x000012688B70E62B, 0x401C000000000000},
     {0x80000000, 0, 0x00000400, 0x40E00000}, (uint64_t) -1, 0xFFFFFFFF, 3, 7},
    // 1e300, -1e300 (overflow a conversion), 0.5, -2.5
    {{0x7E37E43C8800759C, 0xFE37E43C8800759C, 0x3FE0000000000000, 0xC004000000000000},
     {0x7F000000, 0xFF000000, 0x3F000000, 0xC0200000}, 123456789, UINT64_MAX, 9, 9},
    // 2.5, -3.5 (ties), 1e-320, 0.75
    {{0x4004000000000000, 0xC00C000000000000, 0x00000000000007E8, 0x3FE8000000000000},
     {0x40200000, 0xC0600000, 0x00000007, 0x3F400000}, 1ull << 31, 0, (uint64_t) -3, 4},
};
#define NSETS (int) (sizeof(sets) / sizeof(sets[0]))
static const uint64_t fpcrs[] = {0, 1u << 22, 3u << 22, 3u << 24};   // RN, RP, RZ, FZ + DN
#define NFPCRS (int) (sizeof(fpcrs) / sizeof(fpcrs[0]))

// The operands of set k for an instruction of precision t (0 single, 1
// double, as encoded; the others take the doubles)
static void operands(struct io *in, int k, unsigned t) {
    memset(in, 0, sizeof(*in));
    for (int i = 0; i < 4; i++) {
        in->q[i][0] = t == 0 ? 0xA5A5A5A500000000ull | sets[k].s[i] : sets[k].d[i];
        in->q[i][1] = 0x5A5A5A5A5A5A5A50ull + (uint64_t) i;
    }
    in->x9 = sets[k].x9;
    in->x10 = sets[k].x10;
    in->f1 = sets[k].f1;
    in->f2 = sets[k].f2;
}

// An instruction's precision: its type field for the FP forms, its sz for
// AdvSIMD's scalar ones, double for the rest
static unsigned precision(uint32_t w) {
    if ((w & 0xDF3E0C00u) == 0x5E200800u)
        return (w >> 22) & 1;
    if ((w & 0x5F000000u) == 0x1E000000u || (w & 0xFF000000u) == 0x1F000000u)
        return (w >> 22) & 3;
    return 1;
}

static void describe(char *buf, size_t size, const struct io *o) {
    snprintf(buf, size, "q0 %016llx:%016llx q1 %016llx:%016llx q2 %016llx:%016llx q3 %016llx:%016llx "
                        "x9 %llx x10 %llx nzcv %llx fpsr %llx",
             (unsigned long long) o->oq[0][1], (unsigned long long) o->oq[0][0], (unsigned long long) o->oq[1][1],
             (unsigned long long) o->oq[1][0], (unsigned long long) o->oq[2][1], (unsigned long long) o->oq[2][0],
             (unsigned long long) o->oq[3][1], (unsigned long long) o->oq[3][0], (unsigned long long) o->ox9,
             (unsigned long long) o->ox10, (unsigned long long) o->nzcv, (unsigned long long) o->fpsr);
}

static int same(const struct io *a, const struct io *b) {
    return memcmp(&a->oq, &b->oq, sizeof(struct io) - offsetof(struct io, oq)) == 0;
}

// The JIT's mode and how many runs make a block hot, from /proc/ish/jit;
// 0 (and why in why) if the gadgets can not be told from the JIT here
static unsigned hot_runs(char *why, size_t size) {
    FILE *f = fopen("/proc/ish/jit", "r");
    if (f == NULL) {
        snprintf(why, size, "no /proc/ish/jit");
        return 0;
    }
    char line[512], mode[512] = "";
    unsigned runs = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "mode: ", 6) == 0)
            snprintf(mode, sizeof(mode), "%s", line + 6);
        sscanf(line, "hot translation: after %u runs", &runs);
    }
    fclose(f);
    mode[strcspn(mode, "\n")] = '\0';
    if (strcmp(mode, "JIT, position-independent") != 0) {
        snprintf(why, size, "the JIT's mode is \"%s\": a block is translated as it is compiled, or never", mode);
        return 0;
    }
    if (runs == 0)
        snprintf(why, size, "blocks are translated as they are compiled (echo hot 0 > /proc/ish/jit)");
    return runs;
}

int main(void) {
    char why[200];
    unsigned hot = hot_runs(why, sizeof(why));
    if (hot == 0) {
        printf("SKIP %s\n", why);
        return 0;
    }
    make_insns();
    code_words = (size_t) (ninsns * (NSETS * NFPCRS + 1) + 16) * COPY_STRIDE;
    code = mmap(NULL, code_words * 4, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    static struct io cold[NSETS][NFPCRS];
    static struct {
        const char *name;
        int runs, wrong;
        char first[1200];
    } groups[16];
    int ngroups = 0;
    for (int i = 0; i < ninsns; i++) {
        int g = 0;
        while (g < ngroups && strcmp(groups[g].name, insns[i].group) != 0)
            g++;
        if (g == ngroups)
            groups[ngroups++].name = insns[i].group;
        uint32_t w = insns[i].w;
        unsigned t = precision(w);
        struct io in, out;
        // the gadgets: each run in a copy run no other time
        for (int k = 0; k < NSETS; k++)
            for (int c = 0; c < NFPCRS; c++) {
                operands(&in, k, t);
                run(copy(w), &in, fpcrs[c], &cold[k][c]);
            }
        // the JIT: a copy run till it is hot, then for each
        void *fn = copy(w);
        operands(&in, 0, t);
        for (unsigned r = 0; r < hot + 4; r++)
            run(fn, &in, 0, &out);
        for (int k = 0; k < NSETS; k++)
            for (int c = 0; c < NFPCRS; c++) {
                operands(&in, k, t);
                run(fn, &in, fpcrs[c], &out);
                groups[g].runs++;
                if (same(&cold[k][c], &out))
                    continue;
                if (groups[g].wrong++ == 0) {
                    char gd[400], jd[400];
                    describe(gd, sizeof(gd), &cold[k][c]);
                    describe(jd, sizeof(jd), &out);
                    snprintf(groups[g].first, sizeof(groups[g].first), "%08x, operands %d, FPCR %#llx: gadgets %s; JIT %s",
                             w, k, (unsigned long long) fpcrs[c], gd, jd);
                }
            }
    }
    for (int g = 0; g < ngroups; g++) {
        char name[100], detail[1300];
        snprintf(name, sizeof(name), "%s: the gadgets and the JIT alike, %d runs", groups[g].name, groups[g].runs);
        snprintf(detail, sizeof(detail), "%d of %d runs differ; the first: %s", groups[g].wrong, groups[g].runs,
                 groups[g].first);
        check(name, groups[g].wrong == 0, detail);
    }

    // Known values, run hot
    struct {
        const char *name;
        uint32_t w;
        int set;
        int field;   // 0: q3's low half, 1: x9, 2: NZCV
        uint64_t want;
    } known[] = {
        {"fadd d3, d0, d1: 1.5 + -2.25", fp2(2, 1, 3, 0, 1), 0, 0, 0xBFE8000000000000},
        {"fdiv d3, d0, d1: 1.5 / -2.25", fp2(1, 1, 3, 0, 1), 0, 0, 0xBFE5555555555555},
        {"fmadd s3, s0, s1, s2: 1.5 * -2.25 + 0.1", fp3(0, 0, 0, 3, 0, 1, 2), 0, 0, 0xC051999A},
        {"scvtf d3, x9: -7", cvt(1, 1, 0, 2, 3, 9), 0, 0, 0xC01C000000000000},
        {"fcvtzs x9, d0: 1.5", cvt(1, 1, 3, 0, 9, 0), 0, 1, 1},
        {"fcvtzs w9, d0: 1e300, saturated", cvt(0, 1, 3, 0, 9, 0), 6, 1, 0x7FFFFFFF},
        {"fcmp d0, d1: 1.5 > -2.25 (C)", fcmp(1, 0, 1, 0), 0, 2, 0x2},
        {"fcmp d0, d1: a NaN, unordered (C, V)", fcmp(1, 0, 1, 0), 3, 2, 0x3},
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        void *fn = copy(known[i].w);
        struct io in, out;
        operands(&in, known[i].set, precision(known[i].w));
        for (unsigned r = 0; r < hot + 4; r++)
            run(fn, &in, 0, &out);
        uint64_t got = known[i].field == 0 ? out.oq[3][0] : known[i].field == 1 ? out.ox9 : out.nzcv;
        if (known[i].field == 0 && precision(known[i].w) == 0)
            got &= 0xFFFFFFFF;
        char detail[100];
        snprintf(detail, sizeof(detail), "%#llx, want %#llx", (unsigned long long) got,
                 (unsigned long long) known[i].want);
        check(known[i].name, got == known[i].want, detail);
    }

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
