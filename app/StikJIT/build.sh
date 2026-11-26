#!/bin/bash
# Builds StikJIT.xcframework, which the JIT helper extension links
# (app/JITHelper): StikJIT at the commit below, with app/StikJIT/patches.
# StikJIT is the debugger side of iOS 26's JIT protocol, over the loopback
# tunnel to the device's own services (MPL-2.0; it bundles the idevice FFI,
# MIT). Built rather than taken from its releases, for the patches.
#
# usage: app/StikJIT/build.sh   (on a Mac with Xcode 27.1 or later; needs
#                                 xcodegen: brew install xcodegen)
#
# Writes build/StikJIT/StikJIT.xcframework, where the Xcode project finds it,
# and does nothing when that one was built from this commit and these patches.
set -euo pipefail
cd "$(dirname "$0")/../.."

# StikJIT 2.0.1
COMMIT=b3e01a30ded4cd0b27cf1b6bd42f123bbdf49c3a

out=build/StikJIT
# Built again when the commit, the patches or this script change
stamp="$COMMIT $(cat app/StikJIT/patches/*.patch app/StikJIT/build.sh | shasum -a 256 | cut -c1-16)"
if [ -f "$out/StikJIT.xcframework/Info.plist" ] && [ "$(cat "$out/stamp" 2>/dev/null)" = "$stamp" ]; then
    echo "StikJIT.xcframework: built from $stamp already"
    exit 0
fi
command -v xcodegen > /dev/null || { echo "app/StikJIT/build.sh: no xcodegen (brew install xcodegen)" >&2; exit 1; }

rm -rf "$out"
mkdir -p "$out/src"
curl -fsSL -o "$out/src.tar.gz" "https://github.com/StikDebug/StikJIT/archive/$COMMIT.tar.gz"
tar -xzf "$out/src.tar.gz" -C "$out/src" --strip-components=1
for patch in app/StikJIT/patches/*.patch; do
    patch -d "$out/src" -p1 --forward < "$patch"
done
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
