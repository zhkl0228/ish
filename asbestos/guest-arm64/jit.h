#ifndef ASBESTOS_ARM64_JIT_H
#define ASBESTOS_ARM64_JIT_H

// Native JIT for the ARM64 guest (build option -Djit=true, defines ISH_JIT).
//
// A layer on top of the threaded-code blocks: after a block's gadget stream is
// generated, runs of supported guest instructions are translated to host code
// in a MAP_JIT region and the first gadget pointer of each run is replaced by
// a pointer to that code. Anything else still runs as gadgets, so block
// chaining, ret_cache and the fallback paths are those of the gadget engine.
//
// - The hottest guest registers live in fixed host registers across native
//   code and are stored back whenever control leaves it.
// - Loads/stores inline the TLB lookup; a miss resumes the unit's gadget. An
//   access through the register of the last one checks that one's entry first.
// - Block ends (b/bl/b.cond/cbz/tbz/ret/br/blr) are translated; chained
//   successors are reached through patched direct branches.
//
// Without ISH_JIT none of this is compiled and every hook below is absent, so
// the default build is the plain gadget engine. With it, ISH_JIT=0 in the
// environment turns the translator off at run time (the BLR code stream keeps
// its JIT layout). Tuning / diagnostics: ISH_JIT_PIN=0, ISH_JIT_LINK=0,
// ISH_JIT_LOOP=0, ISH_JIT_SIMD=0, ISH_JIT_STATS=1, ISH_JIT_DUMP=<file>,
// ISH_JIT_MAP=<file> (native code -> guest module and file offset),
// ISH_JIT_REGION_MB=<MB> and ISH_JIT_ARENA_MB=<MB> (jit_set_region_mb()).
//
// ISH_JIT_PIC=1 emits position-independent code (jit.c, "PIC"): nothing in it
// depends on where the guest module or its blocks live, and a registry reuses
// a translation for every block with the same key, across processes.
//
// AOT: in PIC mode ISH_JIT_RECORD=<file> writes the translations of one module
// (ISH_JIT_RECORD_MOD, default ld-musl) at exit, and tools/jit_aot turns them
// into an image that a target links in (aot.h). A build with -Djit_emit=false
// runs only such images and never maps executable memory; ISH_JIT_AOT_ONLY=1
// does the same in a JIT build.
//
// iOS runs the code region only for a process a debugger has had
// (CS_DEBUGGED). From iOS 26, on TXM devices (A15 and M2 and later), only one
// the debugger allocated: with a debugger attached as it starts, iSH asks for
// it by iOS 26's JIT protocol (jit.c, "JIT26": StikDebug's scripts answer
// it), then lets the debugger go. ISH_JIT_ASK_DEBUGGER=0 does not ask (lldb,
// which does not answer: Xcode's schemes set it). /proc/ish/jit says where
// the region came from, or why there is none.

#ifdef ISH_JIT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "asbestos/gen.h"

#define JIT_MAX_UNITS 1100

// One gen_step() worth of code-stream words and the guest code it covers.
struct jit_unit {
    unsigned start, end;   // [start, end) in block->code
    uint64_t pc;           // guest address of the first instruction
    uint8_t n;             // guest instructions consumed (0..2)
    bool follow;           // a forward `b` that was followed inline
    bool last;             // the block-ending unit
    uint32_t w[2];         // instruction words
};

struct jit_units {
    unsigned n;            // > JIT_MAX_UNITS: overflowed, block is not translated
    struct jit_unit u[JIT_MAX_UNITS];
};

struct fiber_block;
struct tlb;
struct cpu_state;

// Compile-side hooks (fiber_block_compile).
struct jit_units *jit_units_begin(void);   // NULL when the JIT is off
struct jit_step { unsigned size; addr_t ip; unsigned follow_depth; };
static inline struct jit_step jit_step_begin(const struct gen_state *state) {
    return (struct jit_step) {state->size, state->ip, state->b_follow_depth};
}
void jit_step_end(struct jit_units *units, const struct jit_step *step,
                  const struct gen_state *state, bool more, struct tlb *tlb);
// What a block got as it was compiled: native code (an image's, another's
// translation, or one of its own), gadgets for good, or gadgets till it is hot
// (JIT_LATER: fiber_block_compile puts gadget_jit_count at its code[0])
enum jit_outcome { JIT_GADGETS, JIT_NATIVE, JIT_LATER };
enum jit_outcome jit_block(struct fiber_block *block, struct jit_units *units);
// Hot translation. A block no image and no translation of another's had is
// translated only once it has run jit_hot_runs times (gadget_jit_count): code
// run once or twice, as a program's start, takes no room in the code region,
// and its translating no time. 0 translates each as it is compiled.
// ISH_JIT_HOT=<runs> sets it, as /proc/ish/jit's "hot <runs>" does.
extern uint32_t jit_hot_runs;
// The block hot (INT_JIT_HOT), compiled again to give units (the same code
// makes the same stream): translated in place. Whether it got native code.
bool jit_block_hot(struct fiber_block *block, struct jit_units *units);

// Per-block state, set up by gen_start().
void jit_block_init(struct fiber_block *block);
// The block is about to be freed (no thread runs it): its translation's slot
// is free for another block. A slot has one owner at a time, so a direct link
// into its translation, made while the owner was the successor, always finds
// the owner there.
void jit_block_free(struct fiber_block *block);
struct asbestos;
// The address space is going away: release its module contexts.
void jit_asbestos_free(struct asbestos *asbestos);

// What an AOT image must have been made with to be used (asbestos/guest-arm64/aot.h):
// this ish's abi, and the version of the code it emits, which the abi covers
uint32_t jit_abi(void);
unsigned jit_code_version(void);

// Block chaining (asbestos lock held): patch / restore the direct branch in
// `from`'s native exit for jump_ip[i]. No-ops for blocks without one.
void jit_link(struct fiber_block *from, int i, struct fiber_block *to);
void jit_unlink(struct fiber_block *from, int i);
// May jump_ip[i] of `from` be chained to `to`? PIC code branches directly to
// the successor it was linked to first (an AOT image: when it was recorded),
// so its slot may only ever hold that. A refused successor is marked in the
// slot instead (jit_chain_refused()): from's native code then enters it the
// generic way, through the block cache, rather than leaving to the run loop.
bool jit_chain_ok(struct fiber_block *from, int i, struct fiber_block *to);
// true: from's native code now enters `to` itself; the caller records the
// relation so that `to` leaving the cache unlinks it (jit_unlink()).
bool jit_chain_refused(struct fiber_block *from, int i, struct fiber_block *to);

// Leave the thread's JIT write mode (if a compile or link entered it) before
// running guest code.
void jit_exec_ready(void);

// Module and file offset of guest address pc in the address space of tlb, the
// same in every process (-1: not in a module), and a module's path: the block
// profiler (ISH_BLOCK_PROF_FILE) counts by these.
int jit_prof_locate(struct tlb *tlb, addr_t pc, uint64_t *off);
void jit_module_path(int mod, char *buf, size_t size);

// A host fault at address addr in guest code (asbestos/guest-arm64/host_fault.c;
// see "host faults" in jit.c). as_unknown: take it for one not at a guest
// access (ISH_JIT_FAULT_TEST=unknown).
enum jit_fault {
    JIT_FAULT_NONE,       // not in native code
    JIT_FAULT_RESUME,     // at a guest access: go on at *resume, its TLB-miss exit
    JIT_FAULT_UNKNOWN,    // elsewhere in native code: no position to retry from
};
enum jit_fault jit_native_fault(void *ucontext, uintptr_t addr, bool as_unknown, uintptr_t *resume);
// After a JIT_FAULT_UNKNOWN, on the thread it happened on: the run loop ends
// with INT_JIT_NATIVE_FAULT, and handle_interrupt logs what was recorded
// (written into buf) and kills the process.
bool jit_unknown_fault_pending(void);
size_t jit_take_unknown_fault(char *buf, size_t size, const struct cpu_state *cpu);

// Guest init is exiting: ISH_JIT_STATS=1 prints counters, ISH_JIT_DUMP=<file>
// writes the code region.
void jit_report(void);

// Human-readable state for /proc/ish/jit (mode, AOT images, hits per module).
size_t jit_describe(char *buf, size_t size);
// Blocks compiled since the start (jit_block), and of them those that got
// native code: an AOT image's, the JIT's, or one shared from another process's
// translation; the rest run as gadgets. The meters' JIT gauge is the share of
// those compiled lately (app/MonitorSampler.m). Native is read before blocks
// and counted after it, so a reading never has more native than blocks.
void jit_block_counts(uint64_t *blocks, uint64_t *native);
// Whether the code region has filled up: blocks compiled since run as gadgets
// (unless an AOT image has them), as /proc/ish/jit says "(full: ...)". The
// meters' JIT gauge is red then.
bool jit_region_is_full(void);
// Whether the JIT translates (it has its code region), and if so how much of
// the region its code takes, all of it once full, and its size: the meters'
// details show it (app/MonitorDetailViewController.m)
bool jit_region_usage(size_t *used, size_t *size);
// The flush of a full code region, or the eviction of its oldest arena
// (asbestos.c's asbestos_jit_flush drives them). Wanted: the region filled
// (not while recording an image), or the arena in use nears its end with the
// next one holding code. Claim: one caller gets one, none while one runs, and
// from then no translation is made or reused (blocks run as gadgets till
// hot); JIT_FLUSH_ALL, the arena to evict, or JIT_FLUSH_NONE. End: once every
// block with code there was invalidated (all, or those with the arena's bit
// in jit_arenas) and no thread can be running it, the region is taken from
// its start (its shared stubs kept) and the registry of translations emptied,
// or the arena is emptied, its translations taken from the registry and the
// direct links into it undone; and translating goes on. jit_flushes: how
// many ended, of both; a thread that sees it change runs an isb before
// native code, whose addresses held other code.
#define JIT_FLUSH_NONE (-2)
#define JIT_FLUSH_ALL (-1)
// Whether one is claimed and not ended: nothing is translated meanwhile
bool jit_paused(void);
bool jit_flush_wanted(void);
int jit_flush_claim(void);
void jit_flush_end(int what);
uint64_t jit_flushes(void);
// Writes to /proc/ish/jit: "off" / "on" stop / resume installing AOT images
// in blocks compiled afterwards (for A/B timing on a device); "region" maps
// the code region now, if the JIT has none (jit_region_now(false)). -1: an
// unknown command; -2: no code region (/proc/ish/jit says why).
int jit_control(const char *cmd, size_t len);

// The code region, for a JIT that started without one (iOS: no debugger then):
// from the debugger that has iSH now, by iOS 26's JIT protocol (JIT26, which
// the caller knows it answers: StikJIT's, where TXM is), or mapped here (iOS:
// a debugger has had iSH, and there is no TXM). Blocks compiled from then on
// are translated. Whether the JIT translates now; /proc/ish/jit says why not.
bool jit_region_now(bool from_debugger);

// Whether an AOT image in use was recorded from the guest module at path
// (exec.c: node's JS stays in V8's interpreter, which node's image runs as
// native code, only when there is one).
bool jit_aot_serves(const char *path);
// Whether the JIT translates now, with room in its code region: code a
// program writes at run time becomes native code too (exec.c: node then gets
// V8's tiers, though an image serves its interpreter)
bool jit_translates_with_room(void);

// The size of the code region to make, in MB (the app's setting; 0: the
// default, 128): it applies when the region is made, as iSH starts or by
// jit_region_now(), and ISH_JIT_REGION_MB comes before it. Under TXM the
// debugger makes it once, so a new size waits for iSH's next start. The
// region is cut into arenas of 128 MB at most, each with its own stubs, as a
// `b` reaches +-128MB (ISH_JIT_ARENA_MB: smaller ones, for a test); with more
// than one, the oldest is evicted as the one in use nears its end, rather
// than the whole region flushed when it is full.
void jit_set_region_mb(unsigned mb);

#endif
#endif
