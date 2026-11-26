# fs/wait syscall regression tests

These pin the observable behaviour of five fixes so a future change that
reintroduces one of them fails here instead of in a user's shell.

| Fix | What broke before it | Covered by |
| --- | --- | --- |
| #30 `O_DIRECTORY` flag value | `O_DIRECTORY_` carried the x86 value `1<<16` instead of aarch64's `0x4000`, so the `ENOTDIR` check never fired and `O_DIRECT` (`1<<16` on arm64) was misread as `O_DIRECTORY`. GNU `cp` over an existing file failed with `cannot create regular file '<dst>/<src>': Not a directory`. | `regress_syscall.c`, `regress_cp.sh` |
| #30 `O_NOFOLLOW` | Translated to the host `open()`, but `generic_openat()` resolves the last path component first, so the flag was a no-op and `open(symlink, O_NOFOLLOW)` returned an fd to the target. | `regress_syscall.c` |
| #31 fakefs `stat`/`fstat` | The hook-routed branch of `fakefs_stat()` never set `dev` (nor `nlink`/`blksize`), so `stat()` and `fstat()` disagreed on `st_dev` for one file. GNU `cp` reads that as `skipping file '...', as it was replaced while being copied`. | `regress_syscall.c`, `regress_cp.sh` |
| #29 `waitpid` EINTR | `do_wait()` treated its internal 1s timeout (`_ETIMEDOUT`) as a signal, so any child running longer than a second made its parent's wait fail with `EINTR`. gcc died with `failed to get exit status: Interrupted system call`. | `regress_syscall.c` |
| FMOV (immediate) decode | `gen.c` had two call sites expanding an 8-bit AArch64 FP immediate through `arm64_fpimm_to_bits()`, which expects bit6 already inverted. `gen_simd_fp()` inverted it; the scalar `FMOV Dd/Sd, #imm` path passed the raw imm8, so every scalar FP immediate was mis-scaled 8x (`#0.71875` -> `11.5`). V8's `ieee754::cos` loads its C1/C2 range-reduction constants this way, making `Math.cos(Math.PI/4)` return `15.707106781186546`. | `regress_fmov_imm.{c,S}` |
| `waitid` siginfo | `sys_waitid` returned the raw wait status instead of decoding it: `si_status` was `7936` for `exit(31)` and `si_code` was always 0, so a killed child looked like an exited one. `si_uid` was never set at all. | `regress_syscall.c` |
| POSIX timer `SIGEV_SIGNAL` | `sys_timer_create`'s `SIGEV_SIGNAL` branch left `timer->thread_pid` at 0, and `posix_timer_callback` resolves its target with `pid_get_task(thread_pid)`. Pid 0 is never a live task, so the lookup returned NULL and the callback dropped the signal — both syscalls reported success, the clock expired on time, and nothing was delivered. GNU coreutils `timeout` schedules its deadline this way, so `timeout 3 sleep 300` ran the full 300s and exited 0, which reads from the outside as "processes cannot be killed". busybox's `timeout` was unaffected: it polls with `kill(pid, 0)` from a watchdog grandchild and arms no timer, which is why the same command worked in one rootfs and hung in another. | `regress_syscall.c` |
| JIT gadget-buffer OOM | `gen()` called `abort()` when `realloc()` could not grow the gadget buffer, so one guest process outgrowing memory (a `python3` thread pool backgrounded on a phone) killed the whole app — every other guest thread plus the UI — with SIGABRT in `gen_ldst`/`gen_step`. It also advanced `capacity` before the `realloc`, leaving it describing memory that was never allocated. | `regress_jit_oom.c` |
| AF_NETLINK stub hang | The stub answered a request only when it was sent with `sendto()`/`sendmsg()`, and its read was a bare blocking `read()` of the backing socketpair that ignored `MSG_PEEK`, `MSG_DONTWAIT` and `O_NONBLOCK`. busybox `ip` sends with `write()`, so `ip addr` waited for a reply that was never produced until the 900 s tool timeout or a manual stop (exit 143); iproute2 peeks first, and the peek consumed the only reply. Since the stub was switched on by default (`2952b439`) this hit every `ip addr \|\| ifconfig`. Now every send path is answered, links and addresses come from the host's `getifaddrs()`, what the host cannot tell us (routes, rules, neighbours) is `NLMSG_ERROR EOPNOTSUPP` at once, and no read blocks without bound. | `regress_netlink_stub.c` |
| `/proc/pid/maps` across a gap | maps and smaps found a region's end with `mem_next_page`, which skips page tables that hold nothing. Where a mapping ended on a 2 MB boundary (the end of one page table) and the next table was absent, the walk landed on the next mapping, and if both were anonymous with the same permissions they were one entry, the gap in it: a JVM's hs_err map had one `rw-p` line of 15 TB. On an iSH without the fix the gap case fails. | `regress_smaps.c` |
| JVM address-space reservations | HotSpot reserves with `mmap(PROT_NONE, MAP_NORESERVE)`: a 240 MB code cache, 64 MB metaspace nodes, G1's side tables. Under 256 MB iSH gave each the host address space of its whole size. That costs no memory, but every guest process shares the app's one address space, which iOS caps, and with a few JVMs at once (Maven, the javadoc it forks, another build) one failed to start: `Failed to reserve memory for metaspace`. They are lazy reservations now from 1 MB. Over 256 MB they already were, but `MAP_FIXED`, HotSpot's uncommit, left the old pages there, readable and with their contents, and a hint iSH honoured it moved to an aligned address. On an iSH without the fix the 512 MB uncommit and hint cases fail anywhere, and the 5 GB case on a device. | `regress_noreserve.c` |
| A mapping larger than the host's largest free range | `pt_map_nothing` gave a mapping one host `mmap` of its whole size. The app's address space is shared by every guest process and capped by iOS; fragmented, its largest free range was 212 MB with Claude Code running. V8 reserves its 256 MB code range `PROT_NONE` with `MAP_NORESERVE` and makes it all RWX with one `mprotect`, which iSH commits: that failed with `ENOMEM`, and node, Kimi Code with it, died at its start with `Failed to reserve virtual memory for CodeRange`. A plain 256 MB anonymous mapping failed the same way, and so did a thread's stack: libuv sizes it by `RLIMIT_STACK`, 128 MB in iSH, and musl reserves it `PROT_NONE` and makes it read-write with one `mprotect`. When a V8 platform worker's `pthread_create` failed so, node's main thread waited forever for that worker to start, with no output and no CPU. A file's mapping, one host `mmap` too, failed the same way: `claude.exe` has a 230 MB writable segment, and every exec of it (Claude Code, and the ugrep it runs as `grep`) died of `SIGSEGV`. One that does not fit is now mapped in 16 MB host chunks, a file's each with the file and its offset, which AOT images and `MADV_DONTNEED` go by. The test catches it only where the host has no free range that large, as on an iPhone with Claude Code running; on the Linux CLI it passes either way. | `regress_big_commit.c` |
| `unlink` of a directory | `unlink(2)` of a directory said `EPERM`, the host's (Darwin's) answer, where Linux says `EISDIR`. `remove(3)` goes on to `rmdir(2)` only for `EISDIR` (musl and glibc alike), so `remove()` of a directory failed: Java's `File.delete()` is `remove()`, and Maven's protobuf plugin died with `Unable to clean up temporary proto file directory: Directory ... unable to be deleted`. `generic_unlinkat` now says `EISDIR` when an unlink fails on a directory. On a Linux host realfs already said `EISDIR`, so `-r` mode passes there with or without the fix: it catches the bug on fakefs or on a macOS host. | `regress_syscall.c` |
| Leaked thread wedges every fork | `do_exit_group`'s safety valve gives up on a thread that has not left the group within 500 + 10*N ms, unlinks it and releases its sighand/files, but leaves the host thread running. A thread parked in `epoll_wait` then woke from `poll_wait`'s bounded 1 s host wait and dereferenced its torn-down `current->group`; in the idle check it did so holding `pids_lock`, faulted, and `jit_crash_handler` parked it forever with the lock held, so no `fork()` ever completed again (Minis 1.16(1), 2026-10-14, `gh` netpoller). `poll_wait` and `do_wait` now leave as soon as they wake up orphaned (`task_orphaned()`), and the post-syscall guard ends the thread. On an idle host the valve seldom leaks anything, so the test passing locally is necessary but not sufficient: the leak itself reproduces on a loaded iPhone. | `regress_leaked_thread_exit.c` |
| Paths longer than the host's | Darwin takes 1024 bytes in a path (`MAXPATHLEN`), Linux and the guest 4096, and realfs and fakefs gave the host a path whole, from the mount's root: a directory deeper than 1024 bytes could not be made, entered or have a file in it (`ENAMETOOLONG`), however short the guest's path (a relative one from a cwd down there; CPython's `test_os` and `test_tarfile`). And `F_GETPATH` of a directory there said `ENOSPC`, which getcwd, `openat` from its fd and fakefs's readdir ask. A longer path is now taken from a directory down it, each opened in turn (`realfs_at_begin`), and such a directory's path is found by its parents, by inode, as getcwd does. On a Linux host it passes either way; on Darwin (CI's macOS job, the app) the old iSH failed at 1039 bytes. | `regress_long_path.c` |
| `/proc/stat`'s cpu line on Darwin | It was `HOST_CPU_LOAD_INFO`'s counts, 32 bits for all the host's CPUs: a 20-CPU Mac's idle count wrapped every 25 days (an iPhone's every 82), and the cpu line fell 2^33 below the sum of its cpuN lines, which `top` and `htop` take differences of. It is their sum now, as Linux's is. | `regress_proc_stat.c` |

## Running

```sh
tests/regress/run.sh                          # against alpine-arm64-321 via -r
tests/regress/run.sh -i build/ish -r myrootfs # pick binary / rootfs
tests/regress/run.sh -m -f -r alpine-arm64-fakefs   # fakefs: runs the cp cases
```

`run.sh` cross-compiles `regress_syscall.c` for the guest with
`aarch64-linux-musl-gcc` (override with `$CC_GUEST`). If no cross-compiler is
installed it skips rather than fails, since the toolchain is not vendored here.

On an aarch64 Linux host (Ubuntu's `musl-tools`) the host's `musl-gcc` builds
the guest's binaries: `CC_GUEST=musl-gcc`. It has no kernel headers, so
`regress_packet` and `regress_netlink_stub` fail to build with it; give it
`-isystem` a directory with links to `/usr/include/linux`,
`/usr/include/asm-generic` and `/usr/include/aarch64-linux-gnu/asm`, by a
script that runs `musl-gcc -isystem <dir> "$@"`.

Keep the rootfs on a disk of the machine's own. Docker Desktop's share of the
Mac's disk (virtiofs) does not let a socket be chmod'ed (`EINVAL`), so iSH,
which gives a socket bound on its rootfs the mode asked for that way, fails
`bind()`: `regress_unix_path` (3 checks) and `regress_inotify` (1) fail there
and nowhere else.

Every assertion in `regress_syscall.c` is mount-independent, so `-r` mode is the
one that matters for the syscall cases. `-f` mode exists for the `cp` cases,
which need GNU coreutils — usually only present in the fakefs rootfs. Staging a
static binary into a fakefs means registering it in `meta.db`, which is slow, so
`-f` skips the syscall binary and runs only `regress_cp.sh`.

`regress_cp.sh` needs GNU coreutils `cp`, not busybox `cp`: only GNU `cp` probes
its destination with `open(dst, O_PATH|O_DIRECTORY)` and compares
`(st_dev, st_ino)` across `stat`/`fstat`. Without it the `cp` cases skip.

To exercise #31 against a real fakefs mount point, pass the guest path of a file
under one:

```sh
tests/regress/run.sh -m -f -r alpine-arm64-fakefs -f /var/minis/shared/doc.txt
```

That requires a path-translate hook, which only the iOS app installs, so it is
optional — the `stat`/`fstat` assertions still run against the rootfs without it.

## Verifying the tests actually catch the bugs

Run the built binary against an iSH built from a commit before these fixes; it
should report 16 failures across all five areas:

```sh
aarch64-linux-musl-gcc -static -O0 -o <rootfs>/tmp/regress_syscall \
    tests/regress/regress_syscall.c
<old-ish> -r <rootfs> /tmp/regress_syscall
```

## fs hot path / exit race (2026-09-19)

| Fix | What broke before it | Covered by |
| --- | --- | --- |
| process-wide path cache (e78c6980) | `__thread` cache was empty for every forked process; fork storms spent 85% CPU re-resolving the same paths through SQLite and host fstatat | `regress_path_cache.sh` |
| orphan cleanup on demand (43b7c59b) | every last close ran a SQLite write transaction under `inodes_lock`; rename-over-a-closed-file leaked its `stats` row | `regress_inode_orphan.sh` + host-side orphan count |
| inode ref taken in open() (608530ba) | `generic_openat` held `inodes_lock` across a SQLite read; the naive "fstat outside the lock" reorder reintroduces the d57b6d26 open-vs-unlink race | `regress_open_unlink_race.c` |
| mm unpublished under general_lock in do_exit | `/proc/<pid>/{cmdline,stat,statm,maps,mem}` readers took `mem->lock` on an mm that `do_exit` was destroying without `general_lock`: `mem_destroy` trapped on `pthread_rwlock_destroy`=EBUSY or the reader hit freed memory. Killed iSH in <1s with a fork storm plus `ps`/`/proc` readers | `regress_proc_exit_race.sh` |

All four run from `tests/regress/run_fs_perf.sh -i build-native/ish -r <fakefs rootfs> [-b]`,
which works on a COPY of the rootfs so the meta.db orphan check is reproducible.

## poll waits woken for nothing (2026-10-08)

| Fix | What broke before it | Covered by |
| --- | --- | --- |
| poll spin guards (T-ish-poll-spin-guard) | A guest poll that wants only hangups (`events=0`, as the Rust runtime probes fds 0-2 before `main()`) registered the fd with kqueue `EVFILT_READ` + `NOTE_LOWAT=INT_MAX`. xnu ignores that low-water mark for regular files and clamps it to the buffer size for pipes, so a full pipe or a file with unread bytes fired forever while the guest check found nothing. With timeout 0 `poll_wait` never returned; otherwise it spun at 100% of a host core. `head -n 7604 big.py \| rg ...` hung (rg never read, head stayed blocked) and, backgrounded, got the app killed for its CPU budget. | `regress_poll_spurious.c` |

`regress_poll_spurious.c` runs each case in a child under a 3 s watchdog and
also checks CPU time inside timed waits. `/proc/ish/poll_spin` switches the four
guards (`regfile`, `hupedge`, `deadline`, `backoff`) off one by one, e.g.
`echo hupedge 0 > /proc/ish/poll_spin`; with all four off the test must fail
(5 of 7 cases hang), which proves it catches the bug.
