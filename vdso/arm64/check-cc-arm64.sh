#!/bin/sh
# Check if the provided compiler can target aarch64

CLANG="$1"

if [ -z "$CLANG" ]; then
    echo "Usage: $0 <clang-path>"
    exit 1
fi

# Try to compile a simple aarch64 program, from stdin: busybox's mktemp (a musl
# host's) takes no suffix after the X's, which a .c file would need
TMPOUT=$(mktemp /tmp/check-cc-arm64.XXXXXX)

"$CLANG" -target aarch64-linux-gnu -x c -c - -o "$TMPOUT" 2>/dev/null << 'EOF'
void _start(void) {
    __asm__ volatile("svc #0");
}
EOF
RESULT=$?

rm -f "$TMPOUT"

if [ $RESULT -ne 0 ]; then
    echo "Error: $CLANG cannot compile for aarch64 target"
    exit 1
fi

echo "ARM64 cross-compiler check passed"
exit 0
