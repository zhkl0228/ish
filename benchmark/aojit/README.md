# AOJIT: AOT images, their workloads and tests

AOT images are the ARM64 JIT's translations of one guest module, recorded ahead of time and linked
into a target (the Mac CLI with `-Dcli_aot`, the app with `ISH_AOT_OBJECTS`). This directory holds
what makes and checks them; the generic tools are in `tools/jit_aot/`.

## Layout

```
tools/jit_aot/             the image pipeline (host, no dependency on this suite)
├── record.sh              record one module with a JIT build and make its .S (RECORDING= keeps the raw file)
├── gen.py                 recording -> aot_<name>.S in the compact table layout
├── compact.py             .S of the older 64-bit table layout -> compact layout (code unchanged)
└── attrib.py              macOS `sample` + ISH_JIT_MAP -> CPU per guest module

benchmark/aojit/           images, workloads and tests (host scripts at the top)
├── images.json            the images: module path substring, recording workload, packages
├── record_images.sh       record the images of images.json (in parallel), IOS=1 also assembles iOS .o
├── install.sh             copy guest/ into a rootfs as /tmp/aojit and make the inputs (Mac CLI)
├── phone.sh               the same inside the app, run the cases, send the results back
├── verify.sh              three-way check: AOT on / AOT off / ISH_JIT=0 must print the same
├── profile.sh             is a module worth an image: wall on/off/all-native, translations, CPU per module
├── compat.sh              the ARM64 compatibility suite (benchmark/run.sh) against a given ish
├── images/                recorded images (not committed, see below)
└── guest/                 copied to /tmp/aojit in the guest
    ├── run_cases.py, cases.json, setup.sh   A/B cases (AOT on/off, hits per module), input generation
    ├── <module>/          per module or area: its recording workload (*_train.sh), the case workloads
    │                      and the generators of their inputs (gen_*); busybox charts doc gh net node pil
    │                      py pyext rg shell ssh zlib
    ├── verify/            the checks of verify.sh (check.sh, threeway.sh, futex_stress.py)
    └── measure/           workloads for profile.sh (ssh, curl, py, py_local, py_tls, rg, git, zlib)
```

Inputs (JSON, logs, images, certificates, the 10 MB scp file) are generated in the guest by
`guest/setup.sh` and the scripts themselves, never committed.

## The app's tools: `images-app.json`

What the app's users run most has a list of its own: Java and Maven (`libjvm.so`), Kimi Code
(`/usr/bin/node`), Claude Code (`claude.exe`, a Bun executable) and musl under all of them, and
what their sessions run most, bash, python3 (`libpython3`), busybox, ssh and libcrypto. Their workloads are `guest/java`,
`guest/kimi`, `guest/claude` and `guest/app` (musl, bash, python3); the agent CLIs answer through a
model API of their own in the guest (`guest/llm/mock_llm.py`, OpenAI's chat completions and
Anthropic's messages), so no account or network is needed for them. A `[session]` prompt has it
call tools, as a coding session does (`guest/claude/claude_session.sh`: git, grep, find, sed and
unittest through Bash, and Claude Code's own Glob and Grep). What a session starts besides
claude.exe chose the extra images: by translations (ISH_JIT_MAP), python3, git and bash; recorded
and timed, bash's and python3's workloads were 2.3 and 4.2 times faster with their images and git's
no faster (its time is its processes' start and the file system), so git has none. Claude Code's
workload (`guest/claude/claude_image.sh`) is print mode (`claude_train.sh`), the session, and a long
session (`claude_long.sh`): resumed on the interactive screen, which `guest/claude/screen.py` drives in
a pseudo-terminal (tools with the screen asking to allow an edit, a subagent, thinking, a long
markdown answer, `/compact`), and a fresh screen's prompt. The app runs with no executable memory,
where a block the image lacks runs through the gadgets: on the phone (2026-11-25, Claude Code
2.1.332, the images of print mode alone) 95.3% of claude.exe's blocks came from the image in print
mode, 88.8% in the session, 85.6% in a long session resumed on the screen and 81.7% on a fresh one.
(When the app had a JIT, the session recorded too made claude.exe's image 41 MB larger and the
session no faster, 10.8 s either way; the image's size is not what counts.)

Claude Code runs with JavaScriptCore's tiers as iSH runs (the app's `/usr/libexec/claude-wrapper`
chooses unless a `BUN_JSC_` setting of them is given already, as kernel/exec.c chooses node's), and
its workloads record it in the mode it runs in with iSH translating:
- iSH translating (`/proc/ish/jit`'s `mode: JIT`): its JITs but FTL (`BUN_JSC_useFTLJIT=0`). What
  they write at run time no image serves, but iSH translates it once hot. On a Mac (2026-11-28, the
  app's images linked into the CLI, PIC, a 512 MB code region), Claude Code resuming a long session
  and working through tool calls took 2.17 s of CPU a turn with FTL off, 2.45 s with every tier,
  2.44 s with Baseline alone and 4.0 s with no JIT, its GC's threads then half its CPU.
- Not translating: no JIT (`BUN_JSC_useJIT=0`), as before the app had executable memory: what the
  JIT writes then runs through the gadgets, while its interpreter is claude.exe's own code. Measured
  in the app then (2026-11-24, iPhone, its images, two runs each): print mode 6.6 -> 6.1 s of CPU, a
  `[session]` 5.0 -> 4.5 s, in the same wall time or less; [anon] blocks 78-88K a run -> 200-1500;
  claude.exe's image serves 99% of its blocks with the JIT and 98% without.

busybox (what a session's commands are on Alpine: ls, grep, sed, find and the rest), ssh (git over
ssh) and libcrypto (ssh's, and TLS's) came in the same day (`guest/busybox/busybox_train.sh`,
`guest/ssh/ssh_train.sh`, `guest/app/crypto_train.sh`). ssh's and libcrypto's workloads talk to an
sshd that `record_app_images.sh` starts in an ish of its own, on a root of its own (images-app.json's
`sshd`: its packages and port, and the images that need it), with the recording root's client keys.

### Published sets: what a clone of the repository builds with

The app links a published set of them, with nothing to do but build: `app-images.lock.json` names
the set (the root the app ships, `deps/rootfs/arm64-root.tar.gz`, and the images recorded on it,
with each file's sha256, the abi and the versions they were recorded on), `app/AOTImages.xcconfig`
links its objects, and `app/fetch-aot.py`, in the app's Build Meson (ARM64) phase, fetches them
from `https://hk.gzmtx.cn/ish-aot/<set>/` into `build/aot/<set>/` (and the root, if there is none),
checking every sha256. `ish-install` installs Claude Code and Kimi Code at the versions the set was
recorded on (`app/RootfsPatch.bundle/files/usr/local/share/ish-install/versions`), which is what
makes their images serve the phone's: a new version comes with an app whose set was recorded on it.
That file has the apk packages' versions too (those of whose code an image is): an app updated
with a set of newer versions leaves what is installed as it was, which the images then serve only
in part (claude.exe: half its blocks), so every login shell names what is of another version than
the set's, Claude Code or Kimi Code, or an apk package older than its (`ish-install --outdated`),
and `ish-install -u` installs the set's (the npm tools again, Claude Code with `--prefix
/opt/claude-code`; `apk upgrade -U` of the older packages); Claude Code's wrapper says it too as
it starts.

CI keeps the set current (`.github/workflows/aot-images.yml`, `ci_app_images.sh`): on a push that
changes ish's abi or what the images are recorded from (a workload, `images-app.json`, the
recording scripts or `gen.py`: `recording_inputs.py`'s hash, kept in the lock), and every day for
new versions of Claude Code, Kimi Code and Alpine's nodejs, openjdk25-jdk, maven, python3 and ssh,
it records on the lock's root, publishes, commits the new lock, xcconfig and versions to master
with the app's build number one more (`CURRENT_PROJECT_VERSION`), and runs the app's build
(`ci.yml`) on it, which publishes that build's ipa; a pull request instead when that commit cannot go onto
master as it is then. The server keeps the set master's lock names and
the newest one; a publish removes the rest. Uploads go as user `ish-aot`, whose key (the secret
`AOT_SSH_KEY`) can only rsync into that directory (`rrsync`).

By hand, for a new root or a set of one's own, on a Mac (Apple silicon) or a Linux arm64 host:

```sh
benchmark/aojit/record_app_images.sh        # build-arm64-jit, build-aot-root, the images, app/AOTImages.local.xcconfig
benchmark/aojit/measure_app_images.sh       # optional: each workload with and without its image, on the CLI
AOT_NO_LOCAL_XCCONFIG=1 benchmark/aojit/record_app_images.sh   # or, to publish: then
python3 benchmark/aojit/publish_app_images.py benchmark/aojit/images/<abi>/app build-arm64-jit/ish build-aot-root --upload
```

`record_app_images.sh` alone writes `app/AOTImages.local.xcconfig` (not committed), which overrides
the published set in the next build in Xcode (`$(SRCROOT)` paths, so a Linux host that shares the
repository with the Mac can write it); remove it to go back to the set. The root is the one
the app ships (`deps/rootfs/arm64-root.tar.gz`), made a fakefs as the app's (`fakefsify`, which
needs libarchive: `brew install libarchive`), with the packages at the versions apk and npm give
that day, as `ish-install` installs them on the phone: an image serves the module it was recorded
from, so record again after the root or the tools change, and keep the phone's packages at the same
versions (`apk upgrade`). An update does no harm: blocks are matched one by one, and an image still
serves the blocks of another version that did not change (`other versions` in `/proc/ish/jit`).
Claude Code updates itself often. node is recorded in the interpreter, the mode `kernel/exec.c`
gives it when its image is in use; `measure_app_images.sh` also times V8's tiers, with the image and
without, which is what the mode should be chosen by.

Linux records too: the JIT's code region is a memfd mapped twice there, as iOS maps it. Linux and
Darwin builds of ish share what an image depends on (the abi in `/proc/ish/jit` is the same for the
same source), so a Linux recording serves the app: `gen.py --format macho` writes it in Mach-O
(a C name gets its underscore, an assembly label declared without `NAME()` does not), and clang 20
assembles that for iOS (`IOS_CC`; clang 18's assembler is quadratic in an image's label
differences, of which a large image has millions: claude.exe's was unfinished after 8 minutes, 17 s with clang 20).
For the Linux CLI `gen.py` writes ELF (`.ish_aot` after `.text`: native code takes a pointer
within 128MB after its image's start for native code, so the gadgets must come before it), which
`measure_app_images.sh` assembles with GNU as.

Measured on Linux (18 cores; CLI built as the app, `-Djit_emit=false`), the workloads of
`guest/`, with the images and without (`ISH_JIT=0`):

| workload | images | none |
|---|---|---|
| Java and Maven (javac twice, `mvn package` twice) | 177 s | 230 s |
| Claude Code (`-p` three times, stream-json) | 13 s | 23 s |
| Kimi Code, node in the interpreter | 38 s | 96 s |
| Kimi Code, node with V8's tiers | 46 s | 94 s |

And on Linux (abi `bd14aa62`, Claude Code 2.1.331), the Claude Code session that runs tools
(`claude_session.sh`, the model API outside ish), three runs each, the median:

| images | session |
|---|---|
| none (`ISH_JIT=0`) | 19.7 s |
| musl, node, jvm, claude | 11.3 s |
| and git, bash, python3 | 10.8 s |

## Images

Recorded AOT images live in `images/` but are not committed (`.gitignore`). They are large (the set
below is about 2 GB with recordings) and tied to one ish build, so regenerate them instead of
sharing them.

```
images/
└── <abi>/                 abi fingerprint of the ish that recorded them (/proc/ish/jit: "abi xxxxxxxx")
    ├── latest/            recorded on the current package versions
    │   ├── S/aot_<name>.S     images (tools/jit_aot/gen.py output), for -Dcli_aot
    │   ├── ios/aot_<name>_ios.o   the same, assembled for iOS, for ISH_AOT_OBJECTS
    │   └── rec/<name>.jsonl   raw recordings (ISH_JIT_RECORD); gen.py turns them into .S again
    ├── old/               recorded on older package versions (family-matching A/B)
    │   ├── S/ …
    │   └── rec/ …
    └── minis/             the set MinisApp links (its deps/aot), same layout
```

`<name>` and what each image covers are in `images.json` (module substring, recording
workload, packages).

An image only loads into an ish with the same abi (`jit_abi()`: `JIT_CODE_VERSION` in
`asbestos/guest-arm64/jit.c`, the gadget conventions, the struct layouts). Any other ish lists it as
`rejected` in `/proc/ish/jit`. After a change to `jit.c`, `gen.c`, the gadgets or the offsets, record
again into a new `<abi>/` directory. Within one abi, an image serves its exact module (build-id) and,
through family matching, other versions of it (`libz.so.1*`, …).

**Table layout.** Since abi `97963b7f` the tables use the compact layout of
`asbestos/guest-arm64/aot.h`: 32-bit self-relative pointers (the linker resolves them, dyld has
nothing to rebase) and keys without the words that are always 0. `gen.py` writes it; images made
before it (abi `daf8fcc7`, the 64-bit layout) are rewritten with `tools/jit_aot/compact.py <in.S>
<out.S> --abi 97963b7f`, which leaves the native code byte for byte as it is. On the 26 images this
made the linked app 9% smaller (tables 77.1 -> 53.4 MB, rebases 2.43M -> 7.3K); the IPA stays about
the same size, since the relative offsets compress less well than the absolute pointers did.

- `daf8fcc7/`: the 64-bit layout, for feat/aot-family-match up to `0e59def6` (build 822/823). Its
  `rec/` recordings cannot go through the current gen.py (their recorder is gone and gen.py refuses
  that abi); convert the `.S` with compact.py instead.
- `97963b7f/`: the compact layout. `latest/` is the 26-image set converted from `daf8fcc7/latest`;
  `minis/` is MinisApp's 25-image set (musl, busybox and zlib from `old/`, libcrypto, libssl and
  pcre2 recorded on the app's rootfs), converted the same way.
- `1f3bb5a0/`: JIT_CODE_VERSION 8 (one range check per TLB lookup, cycles counted on backward edges
  and rets), compact layout. `latest/` holds the six images below; the rest are not recorded yet.

## Current set: abi `1f3bb5a0` (JIT_CODE_VERSION 8)

| image | module | recorded on |
|---|---|---|
| musl | ld-musl-aarch64.so.1 | musl 1.2.5-r11 |
| busybox | /bin/busybox | busybox 1.37.0-r14 |
| python | libpython3.12.so.1.0 | python3 3.12.15-r0 |
| pyjson | lib-dynload `_json` | python3 3.12.15-r0 |
| node | /usr/bin/node | nodejs 22.23.2-r0, default node mode |
| gh | /usr/bin/gh | github-cli 2.63.0-r5 |

Recorded 2026-10-05 on a clone of `alpine-arm64-321-latest` brought up to the Alpine 3.21 packages of
that day (`apk upgrade`; python3 had moved to 3.12.15), so that `apk add` in a fresh app installs
exactly these builds. pyjson was recorded with a json encode/decode one-liner because
`pyext_train.sh` hung without certificates then (fixed); record it again with `pyext_train.sh`.
Every other image of the set below still has to be recorded for this abi; until then the app runs
those modules in gadgets. Size: `.S` 2.3 GB in all (node 605 MB, gh 456 MB, python 167 MB), iOS
objects 306 MB.

Against the same images recorded at abi `97963b7f`: pure-AOT Mac CLI CPU time -7 to -16%; virtual
iPhone (build 830 vs 831, interleaved, AOT on) python json -17.6%, node json -15.8%, gh -9 to -11%,
grep -r -9.8%, gzip -3.6%; hit rates the same. AOT on vs off in the app: 1.4x (node start) to 3.9x
(grep -r).

### `1f3bb5a0/minis/`: MinisApp's 26 images

`minis/{S,ios,rec}`, delivery table `1f3bb5a0/minis-delivery-table.md` (module, version, build-id,
translations, hit rate, sizes). File names carry the package version (`aot_musl_1.2.5-r8.S`), the module
symbol the same with `.` and `-` as `_`.

- Group A, the modules of the Minis built-in rootfs (musl 1.2.5-r8, busybox 1.37.0-r8, zlib 1.3.1-r2,
  libcrypto3/libssl3 3.3.2-r4): recorded on a clone of `alpine-arm64-321-old`, whose build-ids are exactly
  those, without `apk upgrade` (musl_train.sh and busybox_train.sh with grep/find, zlib_train.sh,
  tls_train.sh; the certificates come from another rootfs since this one has no openssl).
- Group B, what `apk add` installs on Alpine 3.21 on 2026-10-05: python3 3.12.15 (libpython and ten
  lib-dynload extensions, pyext_train.sh with the TLS server), sqlite, pillow, libwebp, libjpeg, numpy,
  pcre2 (`pcre_train2.sh` and its git repo live in `alpine-arm64-321-net/tmp/net`), ssh and scp (an sshd
  from `ssh/server.sh` in a second ish on a fakefs rootfs); node and gh reuse the `latest/` recordings.
  Only python3 changed version against the 97963b7f set.
- Checked in one pure-AOT build: 26 in use, 0 rejected. Hit rates over the AOJIT cases (not the
  recording workloads): most above 97%; python3 94%, numpy 89%, pillow 85%, libjpeg 56% (thumbnail
  decoding takes paths pil_train.sh does not); libssl 99.9% for python TLS but 18% under node's https,
  which no recording covers.

## Previous set: abi `97963b7f` (JIT_CODE_VERSION 7, compact tables; recorded at `daf8fcc7`, converted)

| image | module | recorded on | latest/S | old/S |
|---|---|---|---|---|
| musl | ld-musl-aarch64.so.1 | musl 1.2.5-r11 / r8 | ✅ | ✅ |
| busybox | /bin/busybox | busybox r14 / r8 | ✅ | ✅ |
| zlib | libz.so.1 | zlib 1.3.2 / 1.3.1 | ✅ | ✅ |
| python | libpython3.12.so.1.0 | python 3.12.14 / 3.12.12 | ✅ | ✅ |
| node | /usr/bin/node | nodejs 22.23.2 | ✅ | |
| crypto, ssl | libcrypto.so.3, libssl.so.3 | openssl 3.3.7-r1 | ✅ | |
| sqlite | libsqlite3.so.0 | sqlite-libs (Alpine 3.21) | ✅ | |
| imaging, jpeg, webp | PIL `_imaging`, libjpeg.so.8, libwebp.so.7 | py3-pillow, libjpeg-turbo, libwebp | ✅ | |
| numpy | numpy `_multiarray_umath` | py3-numpy (apk) | ✅ | |
| ssh, scp | /usr/bin/ssh, /usr/bin/scp | openssh-client-default 9.9_p2-r0 (build-id df164958…, cbc5234e…) | ✅ | |
| py* (11) | lib-dynload `_json` `_hashlib` `_blake2` `_struct` `_datetime` `binascii` `math` `zlib` `_socket` `select` `_ssl` | python3 3.12.14-r0 | ✅ | |
| rg | /usr/bin/rg | ripgrep 14.1.1-r0 | ✅ | |

Rootfs used: `latest` = `alpine-arm64-321-latest` (musl, busybox, zlib, python, node) and
`alpine-arm64-321-full` (the other seven). `old` = `alpine-arm64-321-old`. All three are in the main checkout,
`/Users/ethan/Src/github.com/ish-arm64/` (untracked). The build 822/823 IPAs link the first 12 `latest/ios/*.o`. ssh and scp came later (2026-09-27, rootfs `alpine-arm64-321-cand` = full + openssh-client). Their workload needs an sshd in a second ish; start it from `guest/ssh/server.sh` on a fakefs rootfs (sshd rejects a realfs `/var/empty`). sftp-mode scp also needs `prctl(PR_SET_DUMPABLE)` support on that server side. The Python extension images (about 6.3 MB of iOS objects) cut local one-liner CPU by about 5%. The rg image (24 MB) makes a directory search 1.21× faster in wall time. gh is a static Go binary, so only its own image applies; its workload needs no account or network (`gh api` stops at the login prompt). On the Mac CLI, a multi-threaded program that is pid 1 (`ish … /bin/sh -c "gh help"`, which sh execs) sometimes leaves ish running after it has exited, so the cases and compatibility tests run gh as a child of sh. rg's worker threads spend most of their CPU in ish's own lock and file-syscall paths, which the image does not change.

The musl, busybox, zlib, python and node images of this set were recorded with the workloads
before they moved into `guest/`. Those workloads had the same scripts under `/tmp/p0`, `/tmp/p3`,
`/tmp/p5`, `/tmp/p6` and `/tmp/pz`, so a new recording differs only in noise.

## Is a module worth an image?

```sh
benchmark/aojit/profile.sh py_local build-arm64-aot/ish build-arm64-jit/ish -r $R
```

It prints the workload's wall time with the images on, off and with everything native (the runtime
JIT build: roughly what images of every module would give), the translations per module and the
CPU per module. A module is worth recording when the gap between "AOT on" and "all native" is
large and its own share of the CPU explains it (ssh: 76% of the CPU in ssh itself, 2.1x; curl:
libcurl about 2%, not worth it). `guest/measure/` holds the workloads; git needs a fakefs rootfs
(`-f`), since git hangs on the Mac CLI's realfs.

## Regenerate

All commands run from the repo root (`ish-arm64-ios-opt`). `R` is a rootfs directory and `ABI`
is the abi of the recording build.

### 1. Recording build: JIT with run-time translation

```sh
meson setup build-arm64-jit -Djit=true -Djit_emit=true -Dbuildtype=release
ninja -C build-arm64-jit
ISH_JIT_PIC=1 build-arm64-jit/ish -r $R /bin/cat /proc/ish/jit | grep -o 'abi [0-9a-f]*'   # -> ABI
```

The abi also covers the runtime conventions (PIC, pinned registers). Recordings run with
`ISH_JIT_PIC=1`, and a build with images linked turns PIC on by itself. Without the variable a plain
JIT build shows another abi (`2c9b3f6a` instead of `daf8fcc7` for this set). That abi is not the one
of the images.

### 2. Rootfs with the packages and the suite

```sh
build-arm64-jit/ish -r $R /sbin/apk add -u $(python3 -c "import json; print(' '.join(json.load(open('benchmark/aojit/images.json'))['packages']['apk']))")
build-arm64-jit/ish -r $R /usr/bin/pip install --break-system-packages python-pptx
build-arm64-jit/ish -r $R /bin/sh -c "npm i -g --no-audit --no-fund $(python3 -c "import json; print(' '.join(json.load(open('benchmark/aojit/images.json'))['packages']['npm']))")"
benchmark/aojit/install.sh build-arm64-jit/ish $R [old rootfs]   # suite -> $R/tmp/aojit, inputs
```

The npm packages are cf and the MCP servers the node workload starts. Without them
`node/node_train.sh` skips those parts, and the node image covers less.

**Node mode.** `kernel/exec.c` adds V8 flags to every exec of a program named `node`. The default is
node as it ships (no --jitless, no polyfill needed: WebAssembly and undici's own fetch work), with
V8's JS tiers and native regexp off, so JS runs in the interpreter, which the node image covers.
`ISH_NODE_MODE=jitless` in the environment, or `--jitless` / `--no-expose-wasm` on the command
line, selects the old jitless set with `/lib/wasm-polyfill.js`. The two modes execute different
parts of the node binary, so record node in the mode the app runs. The node image under `minis-node/` is
recorded in the default mode; the node image of the older sets is jitless.

### 3. Record: `.S`, plus iOS objects with `IOS=1`

```sh
O=benchmark/aojit/images/$ABI/latest
IOS=1 JOBS=3 benchmark/aojit/record_images.sh build-arm64-jit/ish $R $O/tmp        # all images
IOS=1 benchmark/aojit/record_images.sh build-arm64-jit/ish $R $O/tmp zlib python  # or some
mkdir -p $O/S $O/ios $O/rec && mv $O/tmp/aot_*_ios.o $O/ios/ && mv $O/tmp/aot_*.S $O/S/ && mv $O/tmp/rec/* $O/rec/ && rmdir $O/tmp/rec $O/tmp
```

- Each image is one ish run over its workload with `ISH_JIT_PIC=1 ISH_JIT_RECORD=… ISH_JIT_RECORD_MOD=<module>`, followed by `gen.py`.
- node takes about 10 minutes and python about 3. The rest are quick.
- For the `old/` set, run the same commands on the old rootfs with `O=…/$ABI/old` and only the images whose module differs (musl, busybox, zlib, python).

Rebuild only the `.S` from a kept recording (for example after a `gen.py` change that keeps the abi):

```sh
python3 tools/jit_aot/gen.py $O/rec/<name>.jsonl build-arm64-jit/ish $R $O/S/aot_<name>.S --name <name>
xcrun -sdk iphoneos clang -target arm64-apple-ios16.0 -c -o $O/ios/aot_<name>_ios.o $O/S/aot_<name>.S
```

`gen.py` options: `--family '<fnmatch>'` overrides the family pattern (derived from the soname by
default). `--no-links` drops the static direct links.

### 4. Link

Mac CLI (pure AOT, no executable memory at run time, as on iOS):

```sh
S=$PWD/benchmark/aojit/images/$ABI/latest/S
meson setup build-arm64-aot -Djit=true -Djit_emit=false -Dbuildtype=release \
    -Dcli_aot=$(ls $S/aot_*.S | paste -sd, -)
ninja -C build-arm64-aot
```

App / IPA: the ARM64 targets build the JIT with the AOT runtime (`ISH_ENABLE_JIT = YES` in
`app/GuestARM64.xcconfig`); with `ISH_JIT_EMIT = YES` there it also translates at run time while a
debugger is attached (Xcode, StikDebug), and without one runs the images and gadgets only. List the images in `app/AOTImages.local.xcconfig`
(not committed, included by `app/AppARM64.xcconfig`) and a plain build of the scheme links them:

```sh
{ printf 'ISH_AOT_OBJECTS ='; for f in benchmark/aojit/images/$ABI/latest/ios/aot_*_ios.o; do printf ' $(SRCROOT)/%s' $f; done; echo; } > app/AOTImages.local.xcconfig
xcodebuild -project iSH.xcodeproj -scheme iSH-ARM64 -configuration Release \
    -archivePath build-ipa/iSH-ARM64.xcarchive -allowProvisioningUpdates DEVELOPMENT_TEAM=<team> \
    CURRENT_PROJECT_VERSION=<build> archive
xcodebuild -exportArchive -archivePath build-ipa/iSH-ARM64.xcarchive -exportPath build-ipa/export \
    -exportOptionsPlist build-ipa/ExportOptions.plist -allowProvisioningUpdates
```

### 5. Check

```sh
build-arm64-aot/ish -r $R /bin/cat /proc/ish/jit      # every image "ok", none "rejected"
build-arm64-aot/ish -r $R /usr/bin/python3 /tmp/aojit/run_cases.py            # quick tier A/B
build-arm64-aot/ish -r $R /usr/bin/python3 /tmp/aojit/run_cases.py --tier all # everything
AOJIT_SSH=127.0.0.1:2222 benchmark/aojit/verify.sh build-arm64-aot/ish -r $R   # three-way, exit 1 on a difference
benchmark/aojit/compat.sh build-arm64-aot/ish                                   # 232 compatibility tests
```

`verify.sh` and the ssh cases need an sshd in a second ish on a fakefs rootfs:
`build-arm64-jit/ish -f <fakefs> /bin/sh /tmp/aojit/ssh/server.sh 2222 < <client public keys>`
(`guest/ssh/keys.sh` prints them). Without `AOJIT_SSH` the ssh part is skipped.

On the phone: `sh /tmp/phone.sh <host> <repo dir on host>` (see `phone.sh`). Add `OLDLIB=<dir>`
for the family cases.

Reference, 822 on the Mac, quick tier:

| case | speed-up |
|---|---|
| pip list | 2.57× |
| ash loop | 5.15× |
| node log/hash/zlib | 3.11× |
| family cases on the old modules | 1.47–4.20× |

The full tier is 1.4–4.8×. The exceptions are bzip2 (no image) and node.fs.
