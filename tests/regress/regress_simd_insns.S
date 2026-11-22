// Trampoline and save template for regress_simd_insns.c: run one instruction
// with a register state loaded from memory and store the result back.
//
// struct state layout (see regress_simd_insns.c):
//   x[27] at 0, v[32] at 224, nzcv at 736, fpsr at 744, ret at 752, target at 760
// x28 holds the state pointer and x27 the stub address; tested instructions
// never name registers 27 to 30.

.text
.global run_test
.type run_test, %function
// void run_test(struct state *st)
run_test:
    stp x29, x30, [sp, #-176]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x28, x0
    adr x1, 1f
    str x1, [x28, #752]
    add x1, x28, #224
    ldp q0, q1, [x1, #0]
    ldp q2, q3, [x1, #32]
    ldp q4, q5, [x1, #64]
    ldp q6, q7, [x1, #96]
    ldp q8, q9, [x1, #128]
    ldp q10, q11, [x1, #160]
    ldp q12, q13, [x1, #192]
    ldp q14, q15, [x1, #224]
    ldp q16, q17, [x1, #256]
    ldp q18, q19, [x1, #288]
    ldp q20, q21, [x1, #320]
    ldp q22, q23, [x1, #352]
    ldp q24, q25, [x1, #384]
    ldp q26, q27, [x1, #416]
    ldp q28, q29, [x1, #448]
    ldp q30, q31, [x1, #480]
    ldr x1, [x28, #736]
    msr nzcv, x1
    msr fpsr, xzr
    ldr x27, [x28, #760]
    ldp x0, x1, [x28, #0]
    ldp x2, x3, [x28, #16]
    ldp x4, x5, [x28, #32]
    ldp x6, x7, [x28, #48]
    ldp x8, x9, [x28, #64]
    ldp x10, x11, [x28, #80]
    ldp x12, x13, [x28, #96]
    ldp x14, x15, [x28, #112]
    ldp x16, x17, [x28, #128]
    ldp x18, x19, [x28, #144]
    ldp x20, x21, [x28, #160]
    ldp x22, x23, [x28, #176]
    ldp x24, x25, [x28, #192]
    ldr x26, [x28, #208]
    br x27
1:
    ldp d14, d15, [sp, #144]
    ldp d12, d13, [sp, #128]
    ldp d10, d11, [sp, #112]
    ldp d8, d9, [sp, #96]
    ldp x27, x28, [sp, #80]
    ldp x25, x26, [sp, #64]
    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #176
    ret
.size run_test, . - run_test

// Copied after each tested instruction: store every register into the state
// through x28, then return to run_test's continuation. Position independent.
.global save_tmpl_start
.global save_tmpl_end
save_tmpl_start:
    stp x0, x1, [x28, #0]
    stp x2, x3, [x28, #16]
    stp x4, x5, [x28, #32]
    stp x6, x7, [x28, #48]
    stp x8, x9, [x28, #64]
    stp x10, x11, [x28, #80]
    stp x12, x13, [x28, #96]
    stp x14, x15, [x28, #112]
    stp x16, x17, [x28, #128]
    stp x18, x19, [x28, #144]
    stp x20, x21, [x28, #160]
    stp x22, x23, [x28, #176]
    stp x24, x25, [x28, #192]
    str x26, [x28, #208]
    mrs x0, nzcv
    str x0, [x28, #736]
    mrs x0, fpsr
    str x0, [x28, #744]
    add x0, x28, #224
    stp q0, q1, [x0, #0]
    stp q2, q3, [x0, #32]
    stp q4, q5, [x0, #64]
    stp q6, q7, [x0, #96]
    stp q8, q9, [x0, #128]
    stp q10, q11, [x0, #160]
    stp q12, q13, [x0, #192]
    stp q14, q15, [x0, #224]
    stp q16, q17, [x0, #256]
    stp q18, q19, [x0, #288]
    stp q20, q21, [x0, #320]
    stp q22, q23, [x0, #352]
    stp q24, q25, [x0, #384]
    stp q26, q27, [x0, #416]
    stp q28, q29, [x0, #448]
    stp q30, q31, [x0, #480]
    ldr x0, [x28, #752]
    br x0
save_tmpl_end:
