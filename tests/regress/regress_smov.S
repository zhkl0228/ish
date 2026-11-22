#ifdef __APPLE__
#define NAME(x) _##x
#else
#define NAME(x) x
#endif
// SMOV (to general) cases for regress_smov.c. Each case first fills x2 with
// all ones, so a 32-bit form that fails to clear the upper half shows up.
// Kept out of inline asm so the compiler cannot reorder or fold anything.
.text
.global NAME(smov_table)
.align 4
// void smov_table(const uint8_t vec[16], uint64_t *out)
NAME(smov_table):
  ld1 {v0.16b}, [x0]
  mov x2, #-1
  smov w2, v0.b[0]
  str x2, [x1, #0]
  mov x2, #-1
  smov w2, v0.b[7]
  str x2, [x1, #8]
  mov x2, #-1
  smov w2, v0.b[8]
  str x2, [x1, #16]
  mov x2, #-1
  smov w2, v0.b[15]
  str x2, [x1, #24]
  mov x2, #-1
  smov x2, v0.b[0]
  str x2, [x1, #32]
  mov x2, #-1
  smov x2, v0.b[7]
  str x2, [x1, #40]
  mov x2, #-1
  smov x2, v0.b[8]
  str x2, [x1, #48]
  mov x2, #-1
  smov x2, v0.b[15]
  str x2, [x1, #56]
  mov x2, #-1
  smov w2, v0.h[0]
  str x2, [x1, #64]
  mov x2, #-1
  smov w2, v0.h[3]
  str x2, [x1, #72]
  mov x2, #-1
  smov w2, v0.h[4]
  str x2, [x1, #80]
  mov x2, #-1
  smov w2, v0.h[7]
  str x2, [x1, #88]
  mov x2, #-1
  smov x2, v0.h[0]
  str x2, [x1, #96]
  mov x2, #-1
  smov x2, v0.h[3]
  str x2, [x1, #104]
  mov x2, #-1
  smov x2, v0.h[4]
  str x2, [x1, #112]
  mov x2, #-1
  smov x2, v0.h[7]
  str x2, [x1, #120]
  mov x2, #-1
  smov x2, v0.s[0]
  str x2, [x1, #128]
  mov x2, #-1
  smov x2, v0.s[1]
  str x2, [x1, #136]
  mov x2, #-1
  smov x2, v0.s[2]
  str x2, [x1, #144]
  mov x2, #-1
  smov x2, v0.s[3]
  str x2, [x1, #152]
  ret
