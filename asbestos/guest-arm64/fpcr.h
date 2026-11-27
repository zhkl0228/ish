#ifndef ASBESTOS_GUEST_ARM64_FPCR_H
#define ASBESTOS_GUEST_ARM64_FPCR_H

// The FPCR bits a guest may set: AHP, DN, FZ, RMode and FZ16. Not the trap
// enables, nor FEAT_AFP's FIZ, AH and NEP, which are not offered to it. For
// the gadgets (gadgets.h) and the JIT alike.
#define FPCR_GUEST_BITS 0x07c80000

#endif
