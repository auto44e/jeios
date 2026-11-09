#!/bin/sh
# ADR 0015 D3: the interrupted-mode SP is selected through a mode-checked
# accessor. Keep raw `->sp` field access confined to those accessor bodies.
set -eu

raw_sp_re='(->[[:space:]]*sp|[])][[:space:]]*\.[[:space:]]*sp)([^[:alnum:]_]|$)'
selftest_file="$(mktemp)"
trap 'rm -f "$selftest_file"' EXIT HUP INT TERM
for form in '(*tf).sp' 'tf[0].sp' 'tf-> sp'; do
    printf '%s\n' "$form" >"$selftest_file"
    if ! grep -Eq -- "$raw_sp_re" "$selftest_file"; then
        echo "AArch64 trap-frame boundary: FAIL self-test missed $form" >&2
        exit 1
    fi
done

bad="$(grep -RInE --include='*.c' -- "$raw_sp_re" \
    kernel/arch/aarch64 2>/dev/null |
    awk '($0 !~ /sched_arm\.c:[0-9]+:.*return[[:space:]]+&frame->[[:space:]]*sp;/ &&
          $0 !~ /main\.c:[0-9]+:.*evidence->[[:space:]]*sp/) { print }')"
if [ -n "$bad" ]; then
    echo "AArch64 trap-frame boundary: FAIL raw trap-frame SP access:" >&2
    echo "$bad" >&2
    exit 1
fi

count="$(grep -c 'return &frame->sp;' kernel/arch/aarch64/sched_arm.c || true)"
if [ "$count" -ne 2 ]; then
    echo "AArch64 trap-frame boundary: FAIL expected two SP accessors, found $count" >&2
    exit 1
fi

echo "AArch64 trap-frame boundary: PASS (raw SP confined to mode-checked accessors)"
