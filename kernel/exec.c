#include "kernel/signal.h"
#include "task.h"
#include "util/signpost.h"
#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "misc.h"
#include "kernel/calls.h"
#include "kernel/random.h"
#include "kernel/errno.h"
#include "fs/fd.h"
#include "fs/path.h"
#include "kernel/elf.h"
#include "kernel/vdso.h"
#include "kernel/mm.h"
#include "kernel/native_offload.h"
#include "platform/platform.h"
#ifdef ISH_JIT
#include "asbestos/guest-arm64/jit.h"
#endif

#define ARGV_MAX 32 * PAGE_SIZE

// Use architecture-appropriate ELF structures
typedef struct elf_header64 exec_elf_header;
typedef struct prg_header64 exec_prg_header;

struct exec_args {
    // number of arguments
    size_t count;
    // series of count null-terminated strings, plus an extra null for good measure
    const char *args;
};

static inline addr_t align_stack(addr_t sp);
static inline ssize_t user_strlen(addr_t p);
static inline int user_memset(addr_t start, byte_t val, dword_t len);
static inline addr_t copy_string(addr_t sp, const char *string);
static inline addr_t args_copy(addr_t sp, struct exec_args args);
static size_t args_size(struct exec_args args);

static int read_header(struct fd *fd, exec_elf_header *header) {
    int err;
    if (fd->ops->lseek(fd, 0, SEEK_SET))
        return _EIO;
    if ((err = fd->ops->read(fd, header, sizeof(*header))) != sizeof(*header)) {
        if (err < 0)
            return _EIO;
        return _ENOEXEC;
    }
    if (memcmp(&header->magic, ELF_MAGIC, sizeof(header->magic)) != 0
            || (header->type != ELF_EXECUTABLE && header->type != ELF_DYNAMIC)
            || header->bitness != ELF_CLASS
            || header->endian != ELF_LITTLEENDIAN
            || header->elfversion1 != 1
            || header->machine != ELF_MACHINE) {
        return _ENOEXEC;
    }
    return 0;
}

static int read_prg_headers(struct fd *fd, exec_elf_header header, exec_prg_header **ph_out) {
    ssize_t ph_size = sizeof(exec_prg_header) * header.phent_count;
    exec_prg_header *ph = malloc(ph_size);
    if (ph == NULL)
        return _ENOMEM;

    if (fd->ops->lseek(fd, header.prghead_off, SEEK_SET) < 0) {
        free(ph);
        return _EIO;
    }
    if (fd->ops->read(fd, ph, ph_size) != ph_size) {
        free(ph);
        if (errno != 0)
            return _EIO;
        return _ENOEXEC;
    }

    *ph_out = ph;
    return 0;
}

static int load_entry(exec_prg_header ph, addr_t bias, struct fd *fd) {
    int err;

    addr_t addr = ph.vaddr + bias;
    addr_t offset = ph.offset;
    addr_t memsize = ph.memsize;
    addr_t filesize = ph.filesize;

    // the segment's permissions, as Linux maps it: the executable's and the
    // interpreter's code had no P_EXEC, and /proc/<pid>/maps said r--p of it
    // where Linux says r-xp (what reads maps for code went without: a JVM's,
    // a debugger's, an unwinder's)
    int flags = 0;
    if (ph.flags & PH_R) flags |= P_READ;
    if (ph.flags & PH_W) flags |= P_WRITE;
    if (ph.flags & PH_X) flags |= P_EXEC;

    if ((err = fd->ops->mmap(fd, current->mem, PAGE(addr),
                    PAGE_ROUND_UP(filesize + PGOFFSET(addr)),
                    offset - PGOFFSET(addr), flags, MMAP_PRIVATE)) < 0)
        return err;
    // TODO find a better place for this to avoid code duplication
    pt_set_file(current->mem, PAGE(addr), PAGE_ROUND_UP(filesize + PGOFFSET(addr)),
                fd, offset - PGOFFSET(addr));

    if (memsize > filesize) {
        // put zeroes between addr + filesize and addr + memsize, call that bss
        dword_t bss_size = memsize - filesize;

        // first zero the tail from the end of the file mapping to the end
        // of the load entry or the end of the page, whichever comes first
        addr_t file_end = addr + filesize;
        dword_t tail_size = PAGE_SIZE - PGOFFSET(file_end);
        if (tail_size == PAGE_SIZE)
            // if you can calculate tail_size better and not have to do this please let me know
            tail_size = 0;

        if (tail_size != 0) {
            // Unlock and lock the mem because the user functions must be
            // called without locking mem.
            write_wrunlock(&current->mem->lock);
            user_memset(file_end, 0, tail_size);
            write_wrlock(&current->mem->lock);
        }
        if (tail_size > bss_size)
            tail_size = bss_size;

        // then map the pages from after the file mapping up to and including the end of bss
        if (bss_size - tail_size != 0) {
            if ((err = pt_map_nothing(current->mem, PAGE_ROUND_UP(addr + filesize),
                    PAGE_ROUND_UP(bss_size - tail_size), flags)) < 0)
                return err;
        }
    }
    return 0;
}

// A segment exec could not map, for the report beyond the point of no return
static void describe_segment(char *buf, size_t size, const char *whose, exec_prg_header ph, addr_t bias) {
    snprintf(buf, size, "%s at %#llx (%#llx bytes of the file at %#llx, %#llx in memory, flags %#x)", whose,
             (unsigned long long) (ph.vaddr + bias), (unsigned long long) ph.filesize,
             (unsigned long long) ph.offset, (unsigned long long) ph.memsize, (unsigned) ph.flags);
}

static addr_t find_hole_for_elf(exec_elf_header *header, exec_prg_header *ph) {
    exec_prg_header *first = NULL, *last = NULL;
    for (int i = 0; i < header->phent_count; i++) {
        if (ph[i].type == PT_LOAD) {
            if (first == NULL)
                first = &ph[i];
            last = &ph[i];
        }
    }
    pages_t size = 0;
    if (first != NULL) {
        pages_t a = PAGE_ROUND_UP(last->vaddr + last->memsize);
        pages_t b = PAGE(first->vaddr);
        size = a - b;
    }
    return pt_find_hole(current->mem, size) << PAGE_BITS;
}

static int elf_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp) {
    int err = 0;
    // what failed past the point of no return, for beyond_hope to say
    const char *lost = NULL;
    char lost_segment[160];

    // read the headers
    exec_elf_header header;
    if ((err = read_header(fd, &header)) < 0)
        return err;
    exec_prg_header *ph;
    if ((err = read_prg_headers(fd, header, &ph)) < 0)
        return err;

    // look for an interpreter
    char *interp_name = NULL;
    struct fd *interp_fd = NULL;
    exec_elf_header interp_header;
    exec_prg_header *interp_ph = NULL;
    for (unsigned i = 0; i < header.phent_count; i++) {
        if (ph[i].type != PT_INTERP)
            continue;
        if (interp_name) {
            // can't have two interpreters
            err = _EINVAL;
            goto out_free_interp;
        }

        interp_name = malloc(ph[i].filesize);
        err = _ENOMEM;
        if (interp_name == NULL)
            goto out_free_ph;

        // read the interpreter name out of the file
        err = _EIO;
        if (fd->ops->lseek(fd, ph[i].offset, SEEK_SET) < 0)
            goto out_free_interp;
        if (fd->ops->read(fd, interp_name, ph[i].filesize) != (ssize_t) ph[i].filesize)
            goto out_free_interp;

        // open interpreter and read headers
        interp_fd = generic_open(interp_name, O_RDONLY, 0);
        if (IS_ERR(interp_fd)) {
            err = PTR_ERR(interp_fd);
            goto out_free_interp;
        }
        if ((err = read_header(interp_fd, &interp_header)) < 0) {
            if (err == _ENOEXEC) err = _ELIBBAD;
            goto out_free_interp;
        }
        if ((err = read_prg_headers(interp_fd, interp_header, &interp_ph)) < 0) {
            if (err == _ENOEXEC) err = _ELIBBAD;
            goto out_free_interp;
        }
    }

    // free the process's memory.
    // from this point on, if any error occurs the process will have to be
    // killed before it even starts. please don't be too sad about it, it's
    // just a process.
    //
    // general_lock protects current->mm. otherwise procfs might read the
    // pointer before it's released and then try to lock it after it's
    // released.
    struct mm *new_mm = mm_new();
    if (new_mm == NULL)
        return _ENOMEM;
    lock(&current->general_lock);
    struct mm *old_mm = current->mm;
    task_set_mm(current, new_mm);
    mm_release_from(old_mm, "exec");
    unlock(&current->general_lock);
    write_wrlock(&current->mem->lock);

    current->mm->exefile = fd_retain(fd);

    addr_t load_addr = 0; // used for AX_PHDR
    bool load_addr_set = false;
    addr_t bias = 0; // offset for loading shared libraries as executables

    // map dat shit!
    for (unsigned i = 0; i < header.phent_count; i++) {
        if (ph[i].type != PT_LOAD)
            continue;

        if (!load_addr_set && header.type == ELF_DYNAMIC) {
            // see giant comment in linux/fs/binfmt_elf.c, around line 950
            // ARM64: Always use dynamic placement to avoid conflicts with
            // V8's CodeRange hint at 0x574c0000 (the x86 bias 0x56555000
            // places large binaries like node right on top of it)
            bias = find_hole_for_elf(&header, ph);
        }

        if ((err = load_entry(ph[i], bias, fd)) < 0) {
            describe_segment(lost_segment, sizeof(lost_segment), "the segment", ph[i], bias);
            lost = lost_segment;
            goto beyond_hope;
        }

        // load_addr is used to get a value for AX_PHDR et al
        if (!load_addr_set) {
            load_addr = bias + ph[i].vaddr - ph[i].offset;
            load_addr_set = true;
        }

        // we have to know where the brk starts
        addr_t brk = bias + ph[i].vaddr + ph[i].memsize;
        if (brk > current->mm->start_brk)
            current->mm->start_brk = current->mm->brk = BYTES_ROUND_UP(brk);
    }

    addr_t entry = bias + header.entry_point;
    addr_t interp_base = 0;
    current->mm->exe_bias = bias;
    current->mm->exe_entry = entry;

    if (interp_name) {
        // map dat shit! interpreter edition
        interp_base = find_hole_for_elf(&interp_header, interp_ph);
        for (int i = interp_header.phent_count - 1; i >= 0; i--) {
            if (interp_ph[i].type != PT_LOAD)
                continue;
            if ((err = load_entry(interp_ph[i], interp_base, interp_fd)) < 0) {
                describe_segment(lost_segment, sizeof(lost_segment), "the interpreter's segment", interp_ph[i],
                                 interp_base);
                lost = lost_segment;
                goto beyond_hope;
            }
        }
        entry = interp_base + interp_header.entry_point;
    }


    // map vdso
    err = _ENOMEM;
    lost = "the vdso";
    pages_t vdso_pages = sizeof(vdso_data) >> PAGE_BITS;
    // FIXME disgusting hack: musl's dynamic linker has a one-page hole, and
    // I'd rather not put the vdso in that hole. so find a two-page hole and
    // add one.
    page_t vdso_page = pt_find_hole(current->mem, vdso_pages + 1);
    if (vdso_page == BAD_PAGE)
        goto beyond_hope;
    vdso_page += 1;
    // r-x, as Linux maps it: read and run, now that a page none of read,
    // write and exec allows does not read (mem_ptr_nofault)
    if ((err = pt_map(current->mem, vdso_page, vdso_pages, (void *) vdso_data, 0, P_READ | P_EXEC)) < 0)
        goto beyond_hope;
    mem_pt(current->mem, vdso_page)->data->name = "[vdso]";
    current->mm->vdso = vdso_page << PAGE_BITS;

    // the empty "vvar" pages after it, as Linux maps [vvar]
    lost = "the vvar pages";
    page_t vvar_page = pt_find_hole(current->mem, VVAR_PAGES);
    if (vvar_page == BAD_PAGE)
        goto beyond_hope;
    if ((err = pt_map_nothing(current->mem, vvar_page, VVAR_PAGES, 0)) < 0)
        goto beyond_hope;
    mem_pt(current->mem, vvar_page)->data->name = "[vvar]";

    // STACK TIME!
    lost = "the stack";

    // ARM64: stack near top of 48-bit address space. Linux maps 128 KB of
    // stack below the arguments at exec (setup_arg_pages' stack_expand), and
    // musl's pthread_getattr_np measures the main thread's stack by probing
    // that mapping, so mapping one page made it report a 4 KB stack to
    // programs that size their stack checks by it.
    // readable and writable, as Linux's (maps said -w-p of it)
    if ((err = pt_map_nothing(current->mem, STACK_INIT_PAGE - (STACK_INIT_PAGES - 1), STACK_INIT_PAGES,
                    P_READ | P_WRITE | P_GROWSDOWN)) < 0)
        goto beyond_hope;
    if ((err = pt_map_nothing(current->mem, STACK_TOP_PAGE, 1, P_READ)) < 0)
        goto beyond_hope;
    write_wrunlock(&current->mem->lock);
    addr_t sp = STACK_TOP_ADDR;
    // 8 empty bytes at the very bottom of the stack, as 64-bit Linux leaves
    sp -= sizeof(void *);

    err = _EFAULT;
    // first, copy stuff pointed to by argv/envp/auxv
    // filename, argc, argv
    // Note: lock is already released at this point
    addr_t file_addr = sp = copy_string(sp, file);
    if (sp == 0)
        goto out_free_interp;
    addr_t envp_addr = sp = args_copy(sp, envp);
    if (sp == 0)
        goto out_free_interp;
    // The strings of the environment and of the arguments, as
    // /proc/<pid>/environ and cmdline say them: each a NUL after it, and no
    // more. args_copy copies the list's own NUL after them too, which ends
    // either range here (cmdline said one NUL too many, and environ there was
    // none of: env_start and env_end were never set).
    current->mm->env_start = envp_addr;
    current->mm->env_end = file_addr - 1;
    current->mm->argv_end = sp - 1;
    addr_t argv_addr = sp = args_copy(sp, argv);
    if (sp == 0)
        goto out_free_interp;
    current->mm->argv_start = sp;
    sp = align_stack(sp);

    addr_t platform_addr = sp = copy_string(sp, "aarch64");
    if (sp == 0)
        goto out_free_interp;
    // 16 random bytes so no system call is needed to seed a userspace RNG
    char random[16] = {};
    get_random(random, sizeof(random)); // if this fails, eh, no one's really using it
    addr_t random_addr = sp -= sizeof(random);
    if (user_put(sp, random))
        goto out_free_interp;

    // the way linux aligns the stack at this point is kinda funky
    // calculate how much space is needed for argv, envp, and auxv, subtract
    // that from sp, then align, then copy argv/envp/auxv from that down

    // declare elf aux now so we can know how big it is
    struct aux_ent aux[] = {
        {AX_SYSINFO_EHDR, current->mm->vdso},
        // FP|ASIMD|EVTSTRM|AES|PMULL|SHA1|SHA2|CRC32|ATOMICS. AES+PMULL enables
        // ring's gcm_ghash_clmul (64-bit CLMUL) path. SHA1, SHA2: OpenSSL's
        // SHA-1 and SHA-256 run sha1c, sha256h and the rest natively (the JIT
        // translates them, its V registers held in host registers), on an M2
        // 2.3 and 22 times as fast as their software paths; SHA-1 with the
        // gadgets alone 10 times.
        {AX_HWCAP, 0x1ff},
        {AX_PAGESZ, PAGE_SIZE},
        {AX_CLKTCK, 0x64},
        {AX_PHDR, load_addr + header.prghead_off},
        {AX_PHENT, sizeof(exec_prg_header)},
        {AX_PHNUM, header.phent_count},
        {AX_BASE, interp_base},
        {AX_FLAGS, 0},
        {AX_ENTRY, bias + header.entry_point},
        {AX_UID, 0},
        {AX_EUID, 0},
        {AX_GID, 0},
        {AX_EGID, 0},
        {AX_SECURE, 0},
        {AX_RANDOM, random_addr},
        {AX_HWCAP2, 0}, // suck that too
        {AX_EXECFN, file_addr},
        {AX_PLATFORM, platform_addr},
        {0, 0}
    };
    sp -= ((argv.count + 1) + (envp.count + 1) + 1) * ELF_PTR_SIZE;
    sp -= sizeof(aux);
    sp &=~ 0xf;

    // now copy down, start using p so sp is preserved
    addr_t p = sp;

    // argc
    {uint64_t argc64 = argv.count; if (user_put(p, argc64)) return _EFAULT;}
    p += ELF_PTR_SIZE;

    // argv
    size_t argc = argv.count;
    while (argc-- > 0) {
        {uint64_t ptr64 = argv_addr; if (user_put(p, ptr64)) return _EFAULT;}
        argv_addr += user_strlen(argv_addr) + 1;
        p += ELF_PTR_SIZE;
    }
    p += ELF_PTR_SIZE; // null terminator

    // envp
    size_t envc = envp.count;
    while (envc-- > 0) {
        {uint64_t ptr64 = envp_addr; if (user_put(p, ptr64)) return _EFAULT;}
        envp_addr += user_strlen(envp_addr) + 1;
        p += ELF_PTR_SIZE;
    }
    p += ELF_PTR_SIZE; // null terminator

    // copy auxv
    current->mm->auxv_start = p;
    lost = "the auxv";
    if (user_put(p, aux))
        goto beyond_hope;
    p += sizeof(aux);
    current->mm->auxv_end = p;

    current->mm->stack_start = sp;
    current->cpu.sp = sp;
    current->cpu.pc = entry;
    // Zero all general-purpose registers
    memset(current->cpu.regs, 0, sizeof(current->cpu.regs));
    current->cpu.nzcv = 0;
    current->cpu.nf = 0;
    current->cpu.zf = 0;
    current->cpu.cf = 0;
    current->cpu.vf = 0;
    current->cpu.fpcr = 0;
    current->cpu.fpsr = 0;


    err = 0;
out_free_interp:
    if (interp_name != NULL)
        free(interp_name);
    if (interp_fd != NULL && !IS_ERR(interp_fd))
        fd_close(interp_fd);
    if (interp_ph != NULL)
        free(interp_ph);
out_free_ph:
    free(ph);
    return err;

beyond_hope:
    // The old memory is gone, so the process has nothing to go back to: it
    // dies of SIGSEGV, as Linux's does (force_fatal_sig). It died so before,
    // but of a fault at its old pc and without a word: when claude.exe, as
    // the shell's grep, died of SIGSEGV every time with two Gradle daemons
    // running, nothing told whether this was why. What failed is said now,
    // with the host's memory (as /proc/ish/host has it).
    write_wrunlock(&current->mem->lock);
    struct host_memory host = get_host_memory();
    printk("exec %s: %s failed after the point of no return (%d), SIGSEGV; the OS lets the app take %llu MB "
           "more, footprint %llu MB, %llu mappings, malloc in use %llu MB\n", file, lost, err,
           (unsigned long long) host.available >> 20, (unsigned long long) host.footprint >> 20,
           (unsigned long long) host.mappings, (unsigned long long) host.malloc_in_use >> 20);
    force_signal(current, SIGSEGV_, SIGINFO_NIL, true);
    goto out_free_interp;
}

static size_t args_size(struct exec_args args) {
    const char *args_end = args.args;
    for (size_t i = 0; i < args.count; i++) {
        args_end += strlen(args_end) + 1;
    }
    // don't forget the very last null terminator
    assert(args_end[0] == '\0');
    args_end++;
    return args_end - args.args;
}

static inline addr_t align_stack(addr_t sp) {
    return sp &~ (addr_t)0xf;
}

static inline addr_t copy_string(addr_t sp, const char *string) {
    sp -= strlen(string) + 1;
    if (user_write_string(sp, string))
        return 0;
    return sp;
}

static inline addr_t args_copy(addr_t sp, struct exec_args args) {
    size_t size = args_size(args);
    sp -= size;
    if (user_write(sp, args.args, size))
        return 0;
    return sp;
}

static inline ssize_t user_strlen(addr_t p) {
    size_t i = 0;
    char c;
    do {
        if (user_get(p + i, c))
            return -1;
        i++;
    } while (c != '\0');
    return i - 1;
}

static inline int user_memset(addr_t start, byte_t val, dword_t len) {
    while (len--)
        if (user_put(start++, val))
            return 1;
    return 0;
}

static int format_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp) {
    int err = elf_exec(fd, file, argv, envp);
    if (err != _ENOEXEC)
        return err;
    // other formats would go here
    return _ENOEXEC;
}

static int shebang_exec(struct fd *fd, const char *file, struct exec_args argv, struct exec_args envp) {
    // read the first 128 bytes to get the shebang line out of
    if (fd->ops->lseek(fd, 0, SEEK_SET))
        return _EIO;
    char header[128];
    int size = fd->ops->read(fd, header, sizeof(header) - 1);
    if (size < 0)
        return _EIO;
    header[size] = '\0';

    // only look at the first line
    char *newline = strchr(header, '\n');
    if (newline == NULL)
        return _ENOEXEC;
    *newline = '\0';

    // format: #![spaces]interpreter[spaces]argument[spaces]
    char *p = header;
    if (p[0] != '#' || p[1] != '!')
        return _ENOEXEC;
    p += 2;
    while (*p == ' ')
        p++;
    if (*p == '\0')
        return _ENOEXEC;

    char *interpreter = p;
    while (*p != ' ' && *p != '\0')
        p++;
    if (*p != '\0') {
        *p++ = '\0';
        while (*p == ' ')
            p++;
    }

    char *argument = p;
    // strip trailing whitespace
    p = strchr(p, '\0') - 1;
    while (*p == ' ')
        *p-- = '\0';
    if (*argument == '\0')
        argument = NULL;

    struct exec_args argv_rest = {
        .count = argv.count - 1,
        .args = argv.args + strlen(argv.args) + 1,
    };
    size_t args_rest_size = args_size(argv_rest);
    size_t extra_args_size = strlen(interpreter) + 1 + strlen(file) + 1;
    if (argument)
        extra_args_size += strlen(argument) + 1;
    if (args_rest_size + extra_args_size >= ARGV_MAX)
        return _E2BIG;

    // On the heap: up to ARGV_MAX, 128KB, and on the stack it was in every
    // exec's frame (inlined into __do_execve), which overflowed a musl host's
    // 128KB thread stack in any exec from a guest thread but the first
    char *new_argv_buf = malloc(extra_args_size + args_rest_size);
    if (new_argv_buf == NULL)
        return _ENOMEM;
    struct exec_args new_argv = {.args = new_argv_buf};
    size_t n = 0;
    strcpy(new_argv_buf, interpreter);
    new_argv.count++;
    n += strlen(interpreter) + 1;
    if (argument) {
        strcpy(new_argv_buf + n, argument);
        new_argv.count++;
        n += strlen(argument) + 1;
    }
    strcpy(new_argv_buf + n, file);
    n += strlen(file) + 1;
    new_argv.count++;
    memcpy(new_argv_buf + n, argv_rest.args, args_rest_size);
    new_argv.count += argv_rest.count;

    struct fd *interpreter_fd = generic_open(interpreter, O_RDONLY_, 0);
    if (IS_ERR(interpreter_fd)) {
        free(new_argv_buf);
        return PTR_ERR(interpreter_fd);
    }
    int err = format_exec(interpreter_fd, interpreter, new_argv, envp);
    fd_close(interpreter_fd);
    free(new_argv_buf);
    return err;
}

// The name of a node or V8 option, to tell whether a program already gave one
// that would be added: no leading dashes, no "no-" (--opt and --no-opt set
// the same flag), no value, and '_' as '-' (V8 takes both).
static void node_option_name(const char *opt, size_t len, char *name, size_t size) {
    while (len > 0 && *opt == '-') {
        opt++;
        len--;
    }
    if (len >= 3 && strncmp(opt, "no-", 3) == 0) {
        opt += 3;
        len -= 3;
    }
    size_t n = 0;
    for (; len > 0 && *opt != '=' && n + 1 < size; opt++, len--)
        name[n++] = *opt == '_' ? '-' : *opt;
    name[n] = '\0';
}

static bool node_option_matches(const char *given, size_t len, const char *inject, bool exact_only) {
    if (len == strlen(inject) && strncmp(given, inject, len) == 0)
        return true;
    if (exact_only)
        return false;
    char a[64], b[64];
    node_option_name(given, len, a, sizeof(a));
    node_option_name(inject, strlen(inject), b, sizeof(b));
    return strcmp(a, b) == 0;
}

// Whether node is given option inject already, by the program: among node's
// own options on the command line (before the script) or in NODE_OPTIONS,
// which node reads first, so an option added to the command line would
// override the one there. --require may be given any number of times: only
// the very same one counts.
static bool node_option_given(const char *argv, const char *envp, const char *inject) {
    bool exact_only = strncmp(inject, "--require", 9) == 0;
    for (const char *a = argv + strlen(argv) + 1; *a != '\0'; a += strlen(a) + 1) {
        if (a[0] != '-' || strcmp(a, "--") == 0)
            break;
        if (node_option_matches(a, strlen(a), inject, exact_only))
            return true;
    }
    for (const char *e = envp; *e != '\0'; e += strlen(e) + 1) {
        if (strncmp(e, "NODE_OPTIONS=", 13) != 0)
            continue;
        for (const char *p = e + 13; *p != '\0';) {
            while (*p == ' ')
                p++;
            const char *start = p;
            while (*p != '\0' && *p != ' ')
                p++;
            if (p > start && node_option_matches(start, p - start, inject, exact_only))
                return true;
        }
    }
    return false;
}

int __do_execve(const char *file, struct exec_args argv, struct exec_args envp) {
    ISH_SIGNPOST_SCOPE_BEGIN(exec, "execve", _exec_spid);
    ISH_SIGNPOST_EVENT(exec, "execve_path", "path=%{public}s", file);
    struct fd *fd = generic_open(file, O_RDONLY, 0);
    if (IS_ERR(fd))
        return PTR_ERR(fd);

    // [T-ish-exec-fewer-meta-txns] generic_open just stat'ed this fd for its
    // own access check; reuse that instead of a second fstat, which on fakefs
    // is a further meta.db transaction under the one global fs->lock (the
    // only queue left in a 40-way fork+exec storm, ~16 of 40 workers parked
    // on it at any instant in the 2026-09-19 native samples). Same instant
    // as the open, so setuid/mode decisions are unchanged.
    struct statbuf stat = fd->open_stat;
    int err;

    // if nobody has permission to execute, it should be safe to not execute
    if (!(stat.mode & 0111)) {
        fd_close(fd);
        return _EACCES;
    }

    err = format_exec(fd, file, argv, envp);
    if (err == _ENOEXEC)
        err = shebang_exec(fd, file, argv, envp);
    fd_close(fd);
    if (err < 0)
        return err;

    // setuid/setgid
    if (stat.mode & S_ISUID) {
        current->suid = current->euid;
        current->euid = stat.uid;
    }
    if (stat.mode & S_ISGID) {
        current->sgid = current->egid;
        current->egid = stat.gid;
    }

    // save current->comm
    lock(&current->general_lock);
    const char *basename = strrchr(file, '/');
    if (basename == NULL)
        basename = file;
    else
        basename++;
    // at most 15 bytes and a NUL, as Linux's __set_task_comm: a name of 16 or
    // more had none, and what printed comm with %s read on past it (/proc's
    // comm said 16 bytes of it, Linux's 15)
    snprintf(current->comm, sizeof(current->comm), "%s", basename);
    unlock(&current->general_lock);

    update_thread_name();

    // cloexec
    // consider putting this in fd.c?
    fdtable_do_cloexec(current->files);

    // reset signal handlers
    lock(&current->sighand->lock);
    for (int sig = 0; sig < NUM_SIGS; sig++) {
        struct sigaction_ *action = &current->sighand->action[sig];
        if (action->handler != SIG_IGN_)
            action->handler = SIG_DFL_;
    }
    unlock(&current->sighand->lock);
    // execve removes the calling thread's alternate signal stack.
    current->altstack = 0;
    current->altstack_size = 0;
    // And, as Linux's begin_new_exec, the POSIX timers: their signals and
    // values were the old program's, which may have died of the next one
    // (SIGUSR1's handler gone). setitimer's and alarm's timer stays.
    posix_timers_delete(current->group);

    current->did_exec = true;
    current->undumpable = false;
    vfork_notify(current);

    if (current->ptrace.traced) {
        lock(&pids_lock);
        send_signal(current, SIGTRAP_, (struct siginfo_) {
            .code = SI_USER_,
            .kill.pid = current->pid,
            .kill.uid = current->uid,
        });
        unlock(&pids_lock);
    }

    ISH_SIGNPOST_SCOPE_END(exec, "execve", _exec_spid);
    return 0;
}

int do_execve(const char *file, size_t argc, const char *argv_p, const char *envp_p) {
    struct exec_args argv = {.count = argc, .args = argv_p};
    struct exec_args envp = {.args = envp_p};
    while (*envp_p != '\0') {
        envp_p += strlen(envp_p) + 1;
        envp.count++;
    }
    return __do_execve(file, argv, envp);
}

static ssize_t user_read_string_array(addr_t addr, char *buf, size_t max) {
    size_t i = 0;
    size_t p = 0;
    for (;;) {
        uint64_t str_addr64;
        if (user_get(addr + i * ELF_PTR_SIZE, str_addr64))
            return _EFAULT;
        addr_t str_addr = (addr_t) str_addr64;
        if (str_addr == 0)
            break;
        size_t str_p = 0;
        for (;;) {
            if (p >= max)
                return _E2BIG;
            if (user_get(str_addr + str_p, buf[p]))
                return _EFAULT;
            str_p++;
            p++;
            if (buf[p - 1] == '\0')
                break;
        }
        i++;
    }
    if (p >= max)
        return _E2BIG;
    buf[p] = '\0';
    return i;
}

// [T-ish-offload-path-scope] OpenMinis#288. True only when `path` exists, can
// be read, and starts with "#!". Every failure — missing file, unreadable, a
// directory, a short file — answers false, which leaves the offload decision
// exactly where the path policy put it (see the call site for why a missing
// file must not block the offload). Reads two bytes; only reached for the
// handful of generic offload names, never on an ordinary exec.
static bool guest_file_is_shebang_script(const char *path) {
    struct fd *fd = generic_open(path, O_RDONLY_, 0);
    if (IS_ERR(fd))
        return false;
    char magic[2];
    ssize_t n = fd->ops->read ? fd->ops->read(fd, magic, sizeof(magic)) : -1;
    fd_close(fd);
    return n == 2 && magic[0] == '#' && magic[1] == '!';
}

static dword_t execve_file(const char *filename, addr_t argv_addr, addr_t envp_addr);

dword_t sys_execve(addr_t filename_addr, addr_t argv_addr, addr_t envp_addr) {
    char filename[MAX_PATH];
    if (user_read_string(filename_addr, filename, sizeof(filename)))
        return _EFAULT;
    return execve_file(filename, argv_addr, envp_addr);
}

// execveat was execve, its directory fd taken for the address of its path:
// EFAULT. musl's fexecve asks it (fd, "", AT_EMPTY_PATH), and falls back to
// /proc/self/fd/<fd> only for ENOSYS: CPython's os.execve(fd) failed. The
// file is named here as execve can open it: the fd's own path with
// AT_EMPTY_PATH (what /proc/self/fd/<fd> names, so an unlinked file is not
// found, as musl's fallback would not find it either), or the path from the
// directory fd. As Linux, an fd of no regular file is EACCES, a relative
// path from an fd of no directory ENOTDIR, and with AT_SYMLINK_NOFOLLOW a
// symlink ELOOP.
dword_t sys_execveat(fd_t dirfd, addr_t path_addr, addr_t argv_addr, addr_t envp_addr, dword_t flags) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("execveat(%d, \"%.1000s\", %#x) as execve:", dirfd, path, flags);
    if (flags & ~(AT_EMPTY_PATH_ | AT_SYMLINK_NOFOLLOW_))
        return _EINVAL;
    char filename[MAX_PATH];
    if (path[0] == '/' || (dirfd == AT_FDCWD_ && path[0] != '\0')) {
        strcpy(filename, path);
    } else if (path[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH_))
            return _ENOENT;
        struct fd *fd = dirfd == AT_FDCWD_ ? NULL : f_get(dirfd);
        if (dirfd != AT_FDCWD_ && fd == NULL)
            return _EBADF;
        if (fd == NULL || !S_ISREG(fd->type))
            return _EACCES;
        int err = generic_getpath(fd, filename);
        if (err < 0)
            return err;
    } else {
        struct fd *at = f_get(dirfd);
        if (at == NULL)
            return _EBADF;
        if (!S_ISDIR(at->type))
            return _ENOTDIR;
        char dir[MAX_PATH];
        int err = generic_getpath(at, dir);
        if (err < 0)
            return err;
        if (snprintf(filename, sizeof(filename), "%s/%s", strcmp(dir, "/") == 0 ? "" : dir, path) >=
                (int) sizeof(filename))
            return _ENAMETOOLONG;
    }
    if (flags & AT_SYMLINK_NOFOLLOW_) {
        struct statbuf stat;
        int err = generic_statat(AT_PWD, filename, &stat, false);
        if (err < 0)
            return err;
        if (S_ISLNK(stat.mode))
            return _ELOOP;
    }
    return execve_file(filename, argv_addr, envp_addr);
}

static dword_t execve_file(const char *filename, addr_t argv_addr, addr_t envp_addr) {
    int err = _ENOMEM;
    char *argv = malloc(ARGV_MAX);
    if (argv == NULL)
        goto err_free_argv;
    ssize_t argc = user_read_string_array(argv_addr, argv, ARGV_MAX);
    if (argc < 0) {
        err = argc;
        goto err_free_argv;
    }

    char *envp = malloc(ARGV_MAX);
    if (envp == NULL)
        goto err_free_envp;
    if (envp_addr != 0) {
        err = user_read_string_array(envp_addr, envp, ARGV_MAX);
        if (err < 0)
            goto err_free_envp;
    } else {
        // Do not take advantage of this nonstandard and nonportable misfeature!
        // - Michael Kerrisk, execve(2)
        envp[0] = envp[1] = '\0';
    }

    STRACE("execve(\"%.1000s\", {", filename);
    const char *args = argv;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("}, {");
    args = envp;
    while (*args != '\0') {
        STRACE("\"%.1000s\", ", args);
        args += strlen(args) + 1;
    }
    STRACE("})");

    // One-line exec trace (opt-in via ISH_EXEC_TRACE env at host startup).
    // Lets us correlate V8 crashes with the specific child command.
    if (ish_exec_trace()) {
        char argbuf[512];
        size_t p = 0;
        const char *a = argv;
        int n = 0;
        while (*a != '\0' && p < sizeof(argbuf) - 4 && n < 8) {
            int wrote = snprintf(argbuf + p, sizeof(argbuf) - p,
                                 "%s%.200s", n == 0 ? "" : " ", a);
            if (wrote < 0 || (size_t)wrote >= sizeof(argbuf) - p) break;
            p += wrote;
            a += strlen(a) + 1;
            n++;
        }
        argbuf[sizeof(argbuf) - 1] = 0;
        printk("EXEC[pid=%d ppid=%d]: %.200s [%s]\n",
               current->pid,
               current->parent ? current->parent->pid : 0,
               filename, argbuf);
    }

    // Add V8 flags to a program named node: the default set, or the jitless set
    // when asked for (both below). They choose speed and memory, not
    // correctness: node runs right with all of V8's tiers (checked 2026-11-22
    // with node 24: GC, WebAssembly, workers, fetch, TurboFan-optimized code
    // and npm install). An option the program gives itself, on its command
    // line or in NODE_OPTIONS, wins over the one here (node_option_given).
    {
        const char *base = strrchr(filename, '/');
        base = base ? base + 1 : filename;
        if (strcmp(base, "node") == 0) {
            // Fallback (jitless) mode, see the mode choice below.
            static const char *inject_args_jitless[] = {
                "--jitless",
                "--no-lazy",
                "--max-old-space-size=512",
                // Force single-threaded V8: in jitless mode the worker pool
                // has no JIT to do, but V8 still creates the threads and
                // they spend ~3s/startup blocked on futex/epoll waiting for
                // the (slow, emulated) main thread. Disabling them cuts
                // `node -e 0` from 2.3s → 0.35s on iSH ARM64. Validated
                // 2026-05-05 via syscall profile.
                "--no-concurrent-marking",
                "--no-concurrent-recompilation",
                "--no-lazy-compile-dispatcher",
                "--predictable",
                // [T-ish-node-cage-commit] Skip the young generation.
                // Measured 2026-08-27 on iPhone 11:
                // V8 startup commits ~5 x 128MB cage chunks (~640MB); with
                // --single-generation the commit drops by ~190MB
                // (164k -> 113k guest pages) because the semi-space chunks
                // are never created. Pure memory win for iSH's interpreted
                // workloads; the young-gen GC it disables exists to serve
                // allocation-heavy JIT throughput we don't have.
                "--single-generation",
            };
            // Default mode: node as it ships, with real WebAssembly (and with it undici's own fetch
            // and every package that loads wasm), no polyfill needed, and with V8's JS tiers
            // (Sparkplug, Maglev, TurboFan) and native regexp code. Under the gadgets the code V8
            // generates beats its interpreter by far: the interpreter is itself guest code there,
            // each bytecode a dozen guest instructions and an indirect branch. Measured 2026-11-23
            // on the Linux CLI (no AOT images), against the interpreter set below: a TUI's text
            // layout (ANSI widths, wrapping, inline markdown) 4.8x faster; kimi drawing a streamed
            // answer 1.6-1.8x the frames for the same CPU, idle after it 21% of a core -> 11%;
            // its start to the first screen 6% more CPU, its peak memory +40 MB (764 -> 803).
            // --no-lazy is left out: eager compilation made large CLIs start ~25% slower and the
            // Zone problem it was added for did not show up.
            static const char *inject_args_default[] = {
                "--max-old-space-size=512",
                "--no-concurrent-marking",
                "--no-concurrent-recompilation",
                "--no-lazy-compile-dispatcher",
                "--single-generation",
            };
            // With an AOT image of node in use (jit_aot_serves) its interpreter and builtins are
            // native code, while what V8 generates at run time runs through the gadgets unless
            // the JIT translates with room in its code region (jit_translates_with_room): only
            // then do the tiers stay. Measured 2026-10-01 on the Mac CLI (pure AOT build, node
            // image recorded in this mode) against the tiers: 9 agent workloads 15.6s -> 14.0s,
            // 13 MCP servers start 10-27% faster (mcp-remote only starts in this mode). With
            // the built-in JIT translating, on the phone (2026-11-26, iSH 2.2.1, node 24 and its
            // image), the tiers against this set: JS compute 1.57 s -> 0.60 s, a TUI's text
            // (wrapping, regex, JSON) 0.9 s -> 0.8 s, node -e 0 the same; the code region took
            // 9-12 MB for each the first time and nothing when run again (its translations kept).
            // An image covers the node it was recorded from, in this mode.
            static const char *inject_args_interpreter[] = {
                "--no-sparkplug",
                "--no-maglev",
                "--no-opt",
                "--regexp-interpret-all",
                "--max-old-space-size=512",
                "--no-concurrent-marking",
                "--no-concurrent-recompilation",
                "--no-lazy-compile-dispatcher",
                "--single-generation",
            };
            // The jitless fallback is explicit: ISH_NODE_MODE=jitless in the new program's
            // environment (so it also reaches the node that npx or a shebang starts), or a
            // caller that passes --jitless / --no-expose-wasm itself. Both of those flags take
            // WebAssembly away, and Node's own undici calls WebAssembly.compile() when it loads,
            // so they need the jitless set with wasm-polyfill.js.
            // ISH_NODE_MODE=interpreter or =tiers picks either set whatever images there are:
            // node is recorded for its AOT image, and measured, in the mode the app runs it in.
            bool jitless = false, forced = false, interpreter = false;
            for (const char *e = envp; *e != '\0'; e += strlen(e) + 1) {
                if (strncmp(e, "ISH_NODE_MODE=", 14) != 0)
                    continue;
                const char *mode = e + 14;
                if (strcmp(mode, "jitless") == 0) {
                    jitless = true;
                } else if (strcmp(mode, "interpreter") == 0) {
                    forced = interpreter = true;
                } else if (strcmp(mode, "tiers") == 0) {
                    forced = true;
                } else {
                    printk("ISH_NODE_MODE=%s is none of jitless, interpreter and tiers: node %s gets "
                           "the flags it would without it\n", mode, filename);
                }
            }
            // node's own options come before the script; what follows it belongs to the script
            for (const char *a = argv + strlen(argv) + 1; *a != '\0'; a += strlen(a) + 1) {
                if (a[0] != '-' || strcmp(a, "--") == 0)
                    break;
                if (strcmp(a, "--jitless") == 0 || strcmp(a, "--no-expose-wasm") == 0)
                    jitless = true;
            }
            // Conditionally inject --require for polyfill files that exist in guest fs.
            // With real WebAssembly (the default mode) only the fetch transport is used.
            static const char *optional_requires[] = {
                "--require=/lib/wasm-polyfill.js",    // WebAssembly shim (must load first)
                "--require=/lib/fetch-polyfill.js",   // undici dispatcher on native http/https
            };
            const char *inject_args[16]; // base args + optional requires
            size_t inject_count = 0;
#ifdef ISH_JIT
            if (!forced)
                interpreter = jit_aot_serves(filename) && !jit_translates_with_room();
#else
            (void) forced;  // no images without the JIT: the tiers unless asked
#endif
            if (jitless) {
                for (size_t i = 0; i < sizeof(inject_args_jitless)/sizeof(inject_args_jitless[0]); i++)
                    inject_args[inject_count++] = inject_args_jitless[i];
            } else if (interpreter) {
                for (size_t i = 0; i < sizeof(inject_args_interpreter)/sizeof(inject_args_interpreter[0]); i++)
                    inject_args[inject_count++] = inject_args_interpreter[i];
            } else {
                for (size_t i = 0; i < sizeof(inject_args_default)/sizeof(inject_args_default[0]); i++)
                    inject_args[inject_count++] = inject_args_default[i];
            }
            for (size_t i = jitless ? 0 : 1; i < sizeof(optional_requires)/sizeof(optional_requires[0]); i++) {
                // Extract path after "--require="
                const char *path = optional_requires[i] + 10; // strlen("--require=")
                struct fd *fd = generic_open(path, O_RDONLY_, 0);
                if (!IS_ERR(fd)) {
                    fd_close(fd);
                    inject_args[inject_count++] = optional_requires[i];
                }
            }
            for (size_t ai = 0; ai < inject_count; ai++) {
                const char *arg = inject_args[ai];
                size_t arg_len = strlen(arg) + 1; // includes NUL

                // Find end of argv buffer
                char *p = argv;
                while (*p != '\0')
                    p += strlen(p) + 1;
                size_t total_used = (p - argv) + 1;

                if (!node_option_given(argv, envp, arg) && total_used + arg_len + 1 <= ARGV_MAX) {
                    char *after_argv0 = argv + strlen(argv) + 1;
                    size_t tail = total_used - (after_argv0 - argv);
                    memmove(after_argv0 + arg_len, after_argv0, tail);
                    memcpy(after_argv0, arg, arg_len);
                    argc++;
                }
            }
        }

    }

    // Native offload: check if this binary should run natively on the host.
    // [T-ish-offload-path-scope] OpenMinis#288: path policy + env escape hatch
    // are applied inside the lookup; the #! check below needs the filesystem.
    bool offload_is_generic = false;
    const char *native_path = native_offload_lookup_exec(filename, envp, &offload_is_generic);
    // A generic offload (ffmpeg) must never take over a text script, even at a
    // system path — a wrapper like `#!/bin/sh ... exec /opt/real/ffmpeg "$@"`
    // is the user asking for THEIR ffmpeg, and belongs to shebang_exec.
    //
    // A file that does not exist is NOT a reason to skip: busybox ash execs
    // each PATH directory in turn, so the first whitelisted candidate
    // (/usr/local/bin/ffmpeg) is routinely absent, and offloading it is how a
    // bare `ffmpeg` reaches the native build at all.
    if (native_path && offload_is_generic && guest_file_is_shebang_script(filename)) {
        printk("native_offload: %s is a #! script, running it as one\n", filename);
        native_path = NULL;
    }
    if (native_path) {
        // native_offload_exec calls do_exit() on success, which unwinds via
        // pthread_exit() and skips the err_free_{argv,envp} labels below.
        // Register pthread cleanup handlers so the ARGV_MAX-sized buffers are
        // freed on that unwind path; without them each native-offloaded exec
        // (ffmpeg, minis-open, …) leaks ~2 × ARGV_MAX per process.
        pthread_cleanup_push(free, envp);
        pthread_cleanup_push(free, argv);
        err = native_offload_exec(native_path, filename, argc, argv, envp);
        // Only reach here on fallback. Do not execute cleanups — the normal
        // err_free_{argv,envp} path below will free them.
        pthread_cleanup_pop(0);
        pthread_cleanup_pop(0);
        if (err == 0) {
            // Should not reach here (do_exit doesn't return), but just in case
            goto err_free_envp;
        }
        // Fall through to emulated exec on failure
        printk("native_offload: fallback to emulated exec for %s\n", filename);
    }

    err = do_execve(filename, argc, argv, envp);

err_free_envp:
    free(envp);
err_free_argv:
    free(argv);
    return err;
}
