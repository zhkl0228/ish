#!/bin/bash
# Builds idevice's C library (its FFI) for iOS, which the app links for its
# on-device pairing (app/OnDevicePairing.m): iOS 27 pairs a device with a host
# it finds advertised on the network, iSH, and the pairing file that comes of
# it is the built-in JIT's (BuiltinJIT.m). idevice at the commit below (MIT),
# its remote_pairing feature only (pairable_host_*, rp_pairing_file_*), with
# rustcrypto (pure Rust: no cmake, no bindgen).
#
# usage: app/idevice/build.sh   (on a Mac with Xcode and rustup, the target:
#                                 rustup target add aarch64-apple-ios)
#
# Writes build/idevice/libidevice_ffi.a and idevice.h, where the app's build
# finds them (AppARM64.xcconfig), and does nothing when those were built from
# this commit by this script.
set -euo pipefail
cd "$(dirname "$0")/../.."

# idevice v0.1.68: pairable_host_prepare and pairable_host_accept_fd, the
# pairing an app can do with its own Bonjour advertisement (an app may
# publish one with NetService; idevice's own mDNS would need the multicast
# entitlement)
COMMIT=d32c8189c51c2789496b0768039419c3705498c3
TARGET=aarch64-apple-ios
FEATURES=remote_pairing,rustcrypto

out=build/idevice
stamp="$COMMIT $(shasum -a 256 app/idevice/build.sh | cut -c1-16)"
if [ -f "$out/libidevice_ffi.a" ] && [ "$(cat "$out/stamp" 2>/dev/null)" = "$stamp" ]; then
    echo "libidevice_ffi.a: built from $stamp already"
    exit 0
fi
command -v cargo > /dev/null || { echo "app/idevice/build.sh: no cargo (rustup)" >&2; exit 1; }
rustc --print target-list | grep -qx "$TARGET" ||
    { echo "app/idevice/build.sh: rustc has no $TARGET (rustup target add $TARGET)" >&2; exit 1; }

rm -rf "$out"
mkdir -p "$out/src"
curl -fsSL -o "$out/src.tar.gz" "https://github.com/jkcoxson/idevice/archive/$COMMIT.tar.gz"
tar -xzf "$out/src.tar.gz" -C "$out/src" --strip-components=1
# The app's deployment target, for objects the app's link takes as its own;
# no LTO, which leaves LLVM bitcode in the archive that Xcode's linker does
# not read (tctiSH's experience). Its lints capped, as cargo caps a registry
# dependency's: with a few of its features, code the others use is unused
# here (an import, a function), which it warned of.
export IPHONEOS_DEPLOYMENT_TARGET=18.0 CARGO_PROFILE_RELEASE_LTO=false RUSTFLAGS="--cap-lints allow"
(cd "$out/src/ffi" && cargo build --locked --release --target "$TARGET" --no-default-features --features "$FEATURES")

library="$out/src/target/$TARGET/release/libidevice_ffi.a"
symbols=$(nm -g "$library" 2> /dev/null || true)
for symbol in _pairable_host_prepare _pairable_host_accept_fd _pairable_host_free _rp_pairing_file_to_bytes; do
    grep -q " T $symbol\$" <<< "$symbols" ||
        { echo "app/idevice/build.sh: $library exports no $symbol" >&2; exit 1; }
done
cp "$library" "$out/libidevice_ffi.a"
# libplist's libplist_version() is declared with no prototype, which the
# app's -Wstrict-prototypes warned of in OnDevicePairing.m: (void), as it is
sed 's/^    PLIST_API const char\* libplist_version();$/    PLIST_API const char* libplist_version(void);/' \
    "$out/src/ffi/idevice.h" > "$out/idevice.h"
grep -q "^    PLIST_API const char\* libplist_version(void);$" "$out/idevice.h" ||
    { echo "app/idevice/build.sh: idevice.h declares no libplist_version() to give a prototype" >&2; exit 1; }
echo "$stamp" > "$out/stamp"
echo "libidevice_ffi.a: built from $stamp ($(du -h "$out/libidevice_ffi.a" | cut -f1))"
