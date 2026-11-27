#!/bin/bash
# Builds StikJIT.xcframework, which the JIT helper extension links
# (app/JITHelper): StikJIT from deps/StikJIT, zhkl0228's fork (MPL-2.0), with
# the idevice FFI it links (MIT) built from deps/idevice, zhkl0228's fork's
# stikjit branch, in place of the library StikJIT bundles. StikJIT is the
# debugger side of iOS 26's JIT protocol, over the loopback tunnel to the
# device's own services.
#
# The fork's StikJIT makes one tunnel where StikJIT made two, writes the JIT
# region's pages with their answers read many at a time, and connects no DVT
# remote server; the fork's idevice is compiled for speed (opt-level 3, where
# upstream's is "z"; ARMv8's AES and SHA-512 instructions; jktcp with a sliding
# window). StikJIT's bundled library is upstream idevice 0.1.68, obfuscated.
#
# usage: app/StikJIT/build.sh   (on a Mac with Xcode 27.1 or later, xcodegen
#                                 (brew install xcodegen) and rustup with the
#                                 aarch64-apple-ios target)
#   STIKJIT_SRC=<checkout> IDEVICE_SRC=<checkout>: built from those instead,
#   as their working trees are
#
# Writes build/StikJIT/StikJIT.xcframework, where the Xcode project finds it,
# and does nothing when that one was built from the same trees by this script.
set -euo pipefail
cd "$(dirname "$0")/../.."

src="${STIKJIT_SRC:-deps/StikJIT}"
idevice="${IDEVICE_SRC:-deps/idevice}"
TARGET=aarch64-apple-ios

# A checkout's commit and what its working tree changed of it, untracked
# files included
tree_state() {
    git -C "$1" rev-parse HEAD
    git -C "$1" diff HEAD
    git -C "$1" ls-files --others --exclude-standard -z | (cd "$1" && xargs -0 cat)
}
for tree in "$src" "$idevice"; do
    git -C "$tree" rev-parse HEAD > /dev/null ||
        { echo "app/StikJIT/build.sh: $tree is no checkout (git submodule update --init)" >&2; exit 1; }
done

out=build/StikJIT
stamp="StikJIT $(git -C "$src" rev-parse --short HEAD), idevice $(git -C "$idevice" rev-parse --short HEAD) \
$({ tree_state "$src"; tree_state "$idevice"; cat app/StikJIT/build.sh; } | shasum -a 256 | cut -c1-16)"
if [ -f "$out/StikJIT.xcframework/Info.plist" ] && [ "$(cat "$out/stamp" 2>/dev/null)" = "$stamp" ]; then
    echo "StikJIT.xcframework: built from $stamp already"
    exit 0
fi
command -v xcodegen > /dev/null || { echo "app/StikJIT/build.sh: no xcodegen (brew install xcodegen)" >&2; exit 1; }
command -v cargo > /dev/null || { echo "app/StikJIT/build.sh: no cargo (rustup)" >&2; exit 1; }
rustc --print target-list | grep -qx "$TARGET" ||
    { echo "app/StikJIT/build.sh: rustc has no $TARGET (rustup target add $TARGET)" >&2; exit 1; }

# The idevice FFI, its default features (all its services, AWS-LC) and the
# obfuscated service names StikJIT's bundled one had; in a target directory of
# its own, as app/idevice/build.sh's features are others. No LTO, which leaves
# LLVM bitcode in the archive that Xcode's linker does not read
# (app/idevice/build.sh).
cargo_target="$(pwd)/$out-cargo"
(cd "$idevice/ffi" &&
    BINDGEN_EXTRA_CLANG_ARGS="--sysroot=$(xcrun --sdk iphoneos --show-sdk-path)" \
    IPHONEOS_DEPLOYMENT_TARGET=17.4 CARGO_PROFILE_RELEASE_LTO=false CARGO_TARGET_DIR="$cargo_target" \
    cargo build --release --target "$TARGET" --features obfuscate)

rm -rf "$out"
mkdir -p "$out/src"
rsync -a --exclude .git "$src/" "$out/src/"
cp "$cargo_target/$TARGET/release/libidevice_ffi.a" "$idevice/ffi/idevice.h" "$out/src/idevice/"
(cd "$out/src" && xcodegen generate)
# Its module and its main type are both StikJIT: in the module interface,
# StikJIT.DDIPaths is read as the type's member, and the helper did not
# build against it (Xcode 16.4; StikJIT's releases are built with Xcode 27,
# and earlier ones, it says, may not read their interfaces), unless module
# names there are aliased. Xcode 27.2's Swift writes module selectors there
# (StikJIT::DDIPaths) and ignores the alias flag, said obsoleted by them.
read -r name version <<< "$(xcodebuild -version | head -1)"
[ "$name" = Xcode ] || { echo "app/StikJIT/build.sh: xcodebuild -version said \"$name $version\"" >&2; exit 1; }
IFS=. read -r major minor rest <<< "$version"
swift_flags='$(inherited)'
if (( major * 100 + minor < 2702 )); then
    swift_flags='$(inherited) -alias-module-names-in-module-interface'
fi
xcodebuild archive -project "$out/src/StikJIT.xcodeproj" -scheme StikJIT -destination 'generic/platform=iOS' \
    -archivePath "$out/StikJIT" BUILD_LIBRARY_FOR_DISTRIBUTION=YES SKIP_INSTALL=NO CODE_SIGNING_ALLOWED=NO \
    "OTHER_SWIFT_FLAGS=$swift_flags"
xcodebuild -create-xcframework -framework "$out/StikJIT.xcarchive/Products/Library/Frameworks/StikJIT.framework" \
    -output "$out/StikJIT.xcframework"
echo "$stamp" > "$out/stamp"
echo "StikJIT.xcframework: built from $stamp"
