#!/bin/sh
# ADR 0014 boundary check for the neutral process-lifecycle module.
# Fails (non-zero) if kernel/core/lifecycle.c (CODE, not comments) references an
# x86/run-queue construct, or if lifecycle.c does not compile with the ARM
# freestanding flags (proving it is genuinely arch-neutral and MP3-reusable).
#
# Run from the repo root. Used by `make check`.
set -eu

LC_C="kernel/core/lifecycle.c"
LC_H="include/jeios/lifecycle.h"

if [ ! -f "$LC_C" ] || [ ! -f "$LC_H" ]; then
    echo "lifecycle boundary: SKIP (no $LC_C / $LC_H yet)"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Strip C comments and string literals so the token scan sees CODE only (the
# module's own contract comment legitimately NAMES the forbidden constructs).
strip() {
    # Remove /* ... */ (incl. multiline) and // ... comments, then blank strings.
    sed -e ':a' -e 'N' -e '$!ba' -e 's,/\*[^*]*\*\+\([^/*][^*]*\*\+\)*/, ,g' "$1" \
        | sed -e 's,//.*$,,' -e 's,"[^"]*", "",g'
}
strip "$LC_C" > "$tmp/lc.code"
strip "$LC_H" > "$tmp/lh.code"

fail=0
# Multi-char / symbol tokens.
for tok in je_trap_frame rsp0 rflags 'int \$0x80' 'int \$0x81'; do
    if grep -nE "$tok" "$tmp/lc.code" "$tmp/lh.code" >/dev/null 2>&1; then
        echo "lifecycle boundary: FAIL forbidden token '$tok' in neutral module CODE"
        fail=1
    fi
done
# Whole-word matches for short tokens.
for tok in cr3 tss hlt; do
    if grep -niwE "$tok" "$tmp/lc.code" "$tmp/lh.code" >/dev/null 2>&1; then
        echo "lifecycle boundary: FAIL forbidden token '$tok' in neutral module CODE"
        fail=1
    fi
done
# The scheduler run-queue header must not be pulled into the neutral module.
if grep -nE '#include[[:space:]]+<jeios/sched\.h>|#include[[:space:]]+"[^"]*sched\.h"' "$LC_C" "$LC_H" >/dev/null 2>&1; then
    echo "lifecycle boundary: FAIL lifecycle module includes sched.h (x86 frame leak)"
    fail=1
fi
# It must not touch the x86 thread table (code, not comments).
if grep -nE 'threads\[' "$tmp/lc.code" >/dev/null 2>&1; then
    echo "lifecycle boundary: FAIL lifecycle.c CODE references the x86 threads[] table"
    fail=1
fi
[ "$fail" -eq 0 ] || exit 1

# Arch-neutral compile: the module must build with the aarch64 freestanding flags
# (with -Werror), proving MP3's ARM path can link the same source.
ARMCC=aarch64-linux-gnu-gcc
if command -v "$ARMCC" >/dev/null 2>&1; then
    if "$ARMCC" -Iinclude -std=c11 -O2 -ffreestanding -fno-stack-protector \
        -mgeneral-regs-only -mstrict-align -fno-pic -DJE_ARCH_AARCH64 \
        -Wall -Wextra -Werror -Wconversion -Wshadow -c "$LC_C" -o "$tmp/lifecycle-arm.o" \
        2>"$tmp/err"; then
        echo "lifecycle boundary: PASS (no x86 tokens in code; compiles arch-neutral for aarch64)"
    else
        echo "lifecycle boundary: FAIL lifecycle.c does not compile with ARM freestanding flags:"
        cat "$tmp/err"
        exit 1
    fi
else
    echo "lifecycle boundary: PASS tokens ok ($ARMCC absent; ARM compile check skipped)"
fi
