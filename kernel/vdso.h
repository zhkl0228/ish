#ifndef KERNEL_VDSO_H
#define KERNEL_VDSO_H

// Pages of the vdso, and of the "vvar" mapped after it (empty, as Linux's
// [vvar] is to a process: its data is the kernel's)
#define VDSO_PAGES 2
#define VVAR_PAGES 4

extern const char vdso_data[VDSO_PAGES * (1 << 12)] __asm__("vdso_data");
int vdso_symbol(const char *name);

#endif
