#!/bin/bash
# Configure and build an iSH command line tool with AddressSanitizer.
#
# usage: tools/build-asan.sh <build dir> [meson options...]
#   e.g. tools/build-asan.sh build-asan -Dguest_arch=arm64
#
# ASan reports an out-of-bounds or freed heap access at the moment it
# happens, where without it iSH aborts somewhere later in malloc or free, or
# carries on with a corrupt heap. Run the regression tests or the smoke test
# with the binary it builds (tests/regress/run.sh -i <build dir>/ish,
# tests/smoke/run.sh -i <build dir>/ish): both notice an ASan build and fail
# on any report.
#
# The ASan options iSH needs are built in (__asan_default_options in
# main.c); ASAN_OPTIONS adds to them or overrides them.
#
# The compiler's own ASan runtime is used if it has one (Apple's clang, or a
# clang with compiler-rt installed). Ubuntu's clang ships without it, so
# then the code is instrumented by clang and linked against gcc's libasan,
# which comes from the same LLVM sources.

set -euo pipefail
cd "$(dirname "$0")/.."

if [ $# -lt 1 ]; then
    sed -n '2,20p' "$0"
    exit 2
fi
build=$1
shift

cc=${CC:-clang}
probe=$(mktemp -d)
trap 'rm -rf "$probe"' EXIT
echo 'int main(void) { return 0; }' > "$probe/t.c"

if "$cc" -fsanitize=address -o "$probe/t" "$probe/t.c" 2>/dev/null; then
    echo "build-asan: $cc has its own ASan runtime"
    sanitize=(-Db_sanitize=address)
else
    libasan=$(gcc -print-file-name=libasan.so 2>/dev/null || true)
    if [ -z "$libasan" ] || [ "$libasan" = libasan.so ] || [ ! -e "$libasan" ]; then
        echo "build-asan: $cc has no ASan runtime and gcc's libasan was not found" >&2
        echo "  (install compiler-rt for $cc, or gcc's libasan: apt install libasan8)" >&2
        exit 1
    fi
    libasan=$(readlink -f "$libasan")
    if ! "$cc" -fsanitize=address -fno-sanitize-link-runtime -o "$probe/t" "$probe/t.c" "$libasan" 2>/dev/null; then
        echo "build-asan: $cc cannot link its ASan code against $libasan" >&2
        exit 1
    fi
    echo "build-asan: $cc has no ASan runtime; linking gcc's $libasan"
    sanitize=("-Dc_args=-fsanitize=address -fno-sanitize-link-runtime -fno-omit-frame-pointer"
              "-Dc_link_args=-fno-sanitize-link-runtime $libasan")
fi

CC=$cc meson setup "$build" -Dbuildtype=debugoptimized -Db_lundef=false "${sanitize[@]}" "$@"
ninja -C "$build" ish tools/fakefsify
echo "build-asan: built $build/ish"
