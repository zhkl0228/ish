// Instruction differential test: run each instruction word with deterministic
// register states and dump the resulting state. Run the same binary natively
// and under iSH and compare the dumps.
//
// usage: insndiff <insns.bin> <out.bin> <mode> <seeds> [start] [count]
//   mode 0: registers only (data processing)
//   mode 1: memory; every X register points into a window of a buffer mapped
//           at a fixed address, restored before and hashed after each run
// out.bin: one struct record per (instruction, seed).

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

struct record {
    uint32_t word;
    int32_t status;  // 0, or the signal number
    uint32_t seed;
    uint32_t pad;
    uint64_t x[27];
    uint64_t nzcv;
    uint64_t fpsr;
    uint64_t memhash;
    __uint128_t v[32];
};

void run_test(struct state *st);
extern char save_tmpl_start[], save_tmpl_end[];

#define BUF_ADDR 0x40000000UL
#define BUF_SIZE (192 * 1024)
#define WIN_OFF 0x10000      // pointers land in [WIN_OFF, WIN_OFF + 1024)
#define WIN_LO (WIN_OFF - 1024)
#define WIN_SIZE 4096        // restored and hashed: [WIN_LO, WIN_LO + WIN_SIZE)

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
static const uint64_t ints[] = {
    0, 1, 0xffffffffffffffff, 0x7fffffffffffffff, 0x8000000000000000,
    0x00000000ffffffff, 0x0000000080000000, 0x000000007fffffff, 0xffffffff80000000,
    0x8000, 0x7fff, 0xff, 0x80, 0x3f, 0x40, 0x20,
};

static uint64_t gpr(void) {
    switch (next() % 4) {
    case 0:
        return ints[next() % 16];
    case 1:
        return next() & 0xff;
    default:
        return next();
    }
}

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

static uint64_t fnv(const unsigned char *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}

int main(int argc, char **argv) {
    FILE *in = fopen(argv[1], "rb");
    fseek(in, 0, SEEK_END);
    long n = ftell(in) / 4;
    fseek(in, 0, SEEK_SET);
    uint32_t *words = malloc(n * 4);
    if (fread(words, 4, n, in) != (size_t) n)
        return 1;
    fclose(in);
    int mode = atoi(argv[3]);
    int seeds = atoi(argv[4]);
    long start = argc > 5 ? atol(argv[5]) : 0;
    long count = argc > 6 ? atol(argv[6]) : n - start;
    if (start + count > n)
        count = n - start;

    unsigned char *buf = mmap((void *) BUF_ADDR, BUF_SIZE, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (buf != (unsigned char *) BUF_ADDR) {
        perror("mmap buffer");
        return 1;
    }
    unsigned char *pristine = malloc(BUF_SIZE);
    rng = 0x1234567887654321ULL;
    for (size_t i = 0; i < BUF_SIZE; i += 8) {
        uint64_t v = next();
        memcpy(pristine + i, &v, 8);
    }
    memcpy(buf, pristine, BUF_SIZE);

    size_t tmpl = save_tmpl_end - save_tmpl_start;
    size_t stub = (4 + tmpl + 63) & ~(size_t) 63;
    char *code = mmap(NULL, stub * count, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        perror("mmap code");
        return 1;
    }
    for (long i = 0; i < count; i++) {
        memcpy(code + i * stub, &words[start + i], 4);
        memcpy(code + i * stub + 4, save_tmpl_start, tmpl);
    }
    __builtin___clear_cache(code, code + stub * count);

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);

    FILE *out = fopen(argv[2], "wb");
    static struct state st __attribute__((aligned(16)));
    for (long i = 0; i < count; i++) {
        for (int s = 0; s < seeds; s++) {
            rng = 0x9e3779b97f4a7c15ULL * (uint64_t) (start + i + 1) + 0x632be59bd9b4e019ULL * (uint64_t) s;
            for (int r = 0; r < 27; r++)
                st.x[r] = mode == 1 ? BUF_ADDR + WIN_OFF + (next() % 64) * 16 : gpr();
            for (int r = 0; r < 32; r++)
                st.v[r] = vec();
            st.nzcv = (next() & 0xf) << 28;
            st.fpsr = 0;
            st.target = (uint64_t) (code + i * stub);
            if (mode == 1)
                memcpy(buf + WIN_LO, pristine + WIN_LO, WIN_SIZE);
            struct record rec = {.word = words[start + i], .seed = s};
            int sig = sigsetjmp(env, 1);
            if (sig == 0) {
                run_test(&st);
                rec.status = 0;
                memcpy(rec.x, st.x, sizeof(rec.x));
                rec.nzcv = st.nzcv;
                rec.fpsr = st.fpsr;
                memcpy(rec.v, st.v, sizeof(rec.v));
                if (mode == 1)
                    rec.memhash = fnv(buf + WIN_LO, WIN_SIZE);
            } else {
                rec.status = sig;
            }
            fwrite(&rec, sizeof(rec), 1, out);
        }
    }
    fclose(out);
    fprintf(stderr, "ran %ld instructions x %d seeds\n", count, seeds);
    return 0;
}
