// Regression test for arm64-guest SIMD/FP instructions that iSH got wrong.
//
// A differential run (every SIMD/FP data-processing and load/store word in
// Bun 1.4.4, executed natively and under iSH with the same register state)
// found these, all silently wrong rather than SIGILL:
//   - SRI, SLI, USRA, SSRA on 64-bit vectors left bits 127:64 of Vd as they
//     were instead of clearing them.
//   - USHL used host register shifts, which take the count modulo 64, so
//     shifts of +-esize and beyond did not give 0.
//   - PMULL (8B -> 8H) wrote results over a source register it had not read
//     yet when Vd was Vn or Vm.
//   - FCVTZS/FCVTZU to Wd did not saturate to 32 bits and sign-extended into
//     bits 63:32; the fixed-point FCVTZS to Wd sign-extended too.
//   - FNMSUB with bits[15:10]=101001 was decoded as SSHLL (mask missing bit 28).
//   - LD1/ST1..ST4 single structure to an H lane took the lane index from
//     size<0> instead of size<1>, so {Vt.h}[1] went to lane 0 and so on.
//   - RBIT (vector) was decoded as NOT: the mask left size out. libcrypto's
//     differential run found it.
// It also covers instructions that were not implemented at all (SIGILL):
//   - SQSHL, UQSHL and SQSHLU by immediate, vector and scalar.
//   - SCVTF and UCVTF (vector, fixed-point), single and double precision.
//   - BCAX and XAR (FEAT_SHA3), from libcrypto's differential run.
// And integer words it ran that are undefined, rather than raising SIGILL:
//   - a 32-bit logical immediate with N set, a bitfield with N not sf or
//     with a 6-bit immr or imms in a 32-bit form (libcrypto's data words,
//     from the same run), with valid neighbours; and logical immediates
//     whose bitmask is reserved, which native JIT must not hand the host.
// The H-lane decode broke the ML-KEM half of TLS's X25519MLKEM768 key exchange in
// Bun 1.4.4: every connection to api.anthropic.com failed with
// DECRYPTION_FAILED_OR_BAD_RECORD_MAC, and `claude -p` retried for minutes and
// gave up with EPROTO.
//
// Each case runs one instruction with a register state derived from the
// instruction word and a seed, then hashes x0-x26, NZCV, v0-v31 and, for
// loads/stores, a memory window. The expected hashes were produced by building
// this file with -DGENERATE and running it on aarch64 hardware.
//
// Build: aarch64-linux-musl-gcc -static -O1 -o regress_simd_insns
//            regress_simd_insns.c regress_simd_insns.S
// Run:   ish -r <rootfs> /tmp/regress_simd_insns

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

struct state {
    uint64_t x[27];
    uint64_t pad0;
    __uint128_t v[32];
    uint64_t nzcv;
    uint64_t fpsr;
    uint64_t ret;
    uint64_t target;
};
_Static_assert(__builtin_offsetof(struct state, v) == 224, "v offset");
_Static_assert(__builtin_offsetof(struct state, nzcv) == 736, "nzcv offset");
_Static_assert(__builtin_offsetof(struct state, target) == 760, "target offset");

void run_test(struct state *st);
extern char save_tmpl_start[], save_tmpl_end[];

struct test_case {
    uint32_t word;
    uint8_t mode;   // 0: registers only, 1: X registers point into the memory window
    uint8_t seed;
    int8_t status;  // signal number expected, 0 for none
    uint64_t hash;
};

#ifndef GENERATE
#include "regress_simd_insns_cases.h"
#endif

#define BUF_ADDR 0x40000000UL
#define BUF_SIZE (192 * 1024)
#define WIN_OFF 0x10000
#define WIN_LO (WIN_OFF - 1024)
#define WIN_SIZE 4096

static sigjmp_buf env;
static void on_signal(int sig) {
    siglongjmp(env, sig);
}

static uint64_t rng;
static uint64_t next(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static const uint64_t f64s[] = {
    0x3ff0000000000000, 0xbff0000000000000, 0x0000000000000000, 0x8000000000000000,
    0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0x7ff4000000000000,
    0x0000000000000001, 0x7fefffffffffffff, 0x4059000000000000, 0xc0c3880000000000,
};
static const uint32_t f32s[] = {
    0x3f800000, 0xbf800000, 0x00000000, 0x80000000, 0x7f800000, 0xff800000,
    0x7fc00000, 0x7fa00000, 0x00000001, 0x7f7fffff, 0x42c80000, 0xc61c4000,
};

static __uint128_t vec(void) {
    uint64_t lo = next(), hi = next();
    switch (next() % 8) {
    case 5:
        lo = f64s[next() % 12];
        hi = f64s[next() % 12];
        break;
    case 6:
        lo = f32s[next() % 12] | (uint64_t) f32s[next() % 12] << 32;
        hi = f32s[next() % 12] | (uint64_t) f32s[next() % 12] << 32;
        break;
    case 7:
        lo &= 0x00ff00ff00ff00ffULL;
        hi = (hi & 0x8000800080008000ULL) | 0x0003000200010000ULL;
        break;
    }
    return (__uint128_t) hi << 64 | lo;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const unsigned char *c = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ c[i]) * 0x100000001b3ULL;
    return h;
}

static unsigned char *buf, *pristine;
static char *code;
static size_t stub;

// Run one case; returns the signal it raised (0 if none) and the state hash.
static int run_case(uint32_t word, int mode, int seed, uint64_t *hash) {
    static struct state st __attribute__((aligned(16)));
    rng = 0x9e3779b97f4a7c15ULL * ((uint64_t) word + 1) + 0x632be59bd9b4e019ULL * (uint64_t) (seed + 1);
    for (int r = 0; r < 27; r++)
        st.x[r] = mode == 1 ? BUF_ADDR + WIN_OFF + (next() % 64) * 16 : next();
    for (int r = 0; r < 32; r++)
        st.v[r] = vec();
    st.nzcv = (next() & 0xf) << 28;
    st.fpsr = 0;
    memcpy(code, &word, 4);
    memcpy(code + 4, save_tmpl_start, save_tmpl_end - save_tmpl_start);
    __builtin___clear_cache(code, code + stub);
    st.target = (uint64_t) code;
    memcpy(buf + WIN_LO, pristine + WIN_LO, WIN_SIZE);
    int sig = sigsetjmp(env, 1);
    if (sig != 0)
        return sig;
    run_test(&st);
    uint64_t h = 0xcbf29ce484222325ULL;
    h = fnv(h, st.x, sizeof(st.x));
    h = fnv(h, &st.nzcv, sizeof(st.nzcv));
    h = fnv(h, st.v, sizeof(st.v));
    if (mode == 1)
        h = fnv(h, buf + WIN_LO, WIN_SIZE);
    *hash = h;
    return 0;
}

int main(int argc, char **argv) {
    buf = mmap((void *) BUF_ADDR, BUF_SIZE, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    pristine = malloc(BUF_SIZE);
    rng = 0x1234567887654321ULL;
    for (size_t i = 0; i < BUF_SIZE; i += 8) {
        uint64_t v = next();
        memcpy(pristine + i, &v, 8);
    }
    memcpy(buf, pristine, BUF_SIZE);
    stub = (4 + (save_tmpl_end - save_tmpl_start) + 63) & ~(size_t) 63;
    code = mmap(NULL, stub, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

#ifdef GENERATE
    // argv[1..]: files of hex instruction words; mode 1 for files whose name contains "ldst"
    printf("static const struct test_case cases[] = {\n");
    for (int f = 1; f < argc; f++) {
        int mode = strstr(argv[f], "ldst") != NULL;
        FILE *in = fopen(argv[f], "r");
        unsigned word;
        while (fscanf(in, "%x", &word) == 1) {
            for (int s = 0; s < 4; s++) {
                uint64_t h = 0;
                int sig = run_case(word, mode, s, &h);
                printf("    {0x%08x, %d, %d, %d, 0x%016llxULL},\n", word, mode, s, sig, (unsigned long long) h);
            }
        }
        fclose(in);
    }
    printf("};\n");
    return 0;
#else
    (void) argc;
    (void) argv;
    int failed = 0;
    size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; i++) {
        uint64_t h = 0;
        int sig = run_case(cases[i].word, cases[i].mode, cases[i].seed, &h);
        if (sig != cases[i].status || (sig == 0 && h != cases[i].hash)) {
            if (failed < 40)
                printf("FAIL %08x seed %d: %s\n", cases[i].word, cases[i].seed,
                       sig != cases[i].status ? "signal differs" : "result differs");
            failed++;
        }
    }
    printf("%zu passed, %d failed\n", n - failed, failed);
    return failed != 0;
#endif
}
