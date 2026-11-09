#!/bin/sh
# ADR 0016 D1: the AArch64 usercopy leaves are exception-safe veneers.  Each
# copy routine may use only x0-x3/x30 (and their w aliases), the approved
# byte-copy loop instructions, and no stack or branch-with-link instruction.
set -eu

source_file="kernel/arch/aarch64/usercopy_arm.S"
if [ ! -f "$source_file" ]; then
    echo "AArch64 usercopy boundary: FAIL missing $source_file" >&2
    exit 1
fi

extract_instructions()
{
    awk '
        /^je_copy_(from_user_run|to_user_run|user_fixup):/ {
            active = 1
            next
        }
        active && /^[.]size[[:space:]]+je_copy_/ {
            active = 0
            next
        }
        active && /^[[:space:]]+[[:alpha:]]/ {
            sub(/[[:space:]]*\/\*.*$/, "")
            sub(/[[:space:]]*\/\/.*$/, "")
            if ($0 !~ /^[[:space:]]*$/) print
        }
    ' "$1"
}

check_file()
{
    extract_instructions "$1" |
        awk '
            function is_allowed_register(token, number) {
                if (token == "xzr" || token == "wzr") return 1
                if (token !~ /^[xw][0-9]+$/) return 1
                number = substr(token, 2) + 0
                return number <= 3 || number == 30
            }
            {
                mnemonic = $1
                if (mnemonic ~ /^bl(r)?$/) {
                    print "branch-with-link: " $0 > "/dev/stderr"
                    bad = 1
                }
                if (mnemonic !~ /^(cbz|ldrb|ldtrb|strb|sttrb|add|subs|b[.]ne|mov|ret)$/) {
                    print "unapproved mnemonic: " $0 > "/dev/stderr"
                    bad = 1
                }
                line = $0
                if (line ~ /(^|[^[:alnum:]_])sp([^[:alnum:]_]|$)/) {
                    print "stack register: " $0 > "/dev/stderr"
                    bad = 1
                }
                count = split(line, tokens, /[^[:alnum:]_]+/)
                for (i = 1; i <= count; ++i) {
                    if (tokens[i] ~ /^[xw][0-9]+$/ &&
                        !is_allowed_register(tokens[i])) {
                        print "forbidden register: " $0 > "/dev/stderr"
                        bad = 1
                    }
                }
            }
            END { exit bad ? 1 : 0 }
        '
}

# Self-test every known planted violation class, including instructions that
# are not in the approved mnemonic list and therefore were previously invisible
# to an allowlist-only line filter.
selftest_file="$(mktemp)"
trap 'rm -f "$selftest_file"' EXIT HUP INT TERM
for form in \
    "ldr x16, [x0]" \
    "sub sp, sp, #16" \
    "bl copy_helper" \
    "blr x30" \
    "orr x17, x0, x0" \
    "ldrh w18, [x0]" \
    "mrs x17, tpidr_el0" \
    "eor x18, x0, x0" \
    "str x16, [x0]" \
    "add x17, x0, #1" \
    "mov x18, x0"
do
    {
        printf '%s\n' 'je_copy_from_user_run:'
        printf '    %s\n' "$form"
        printf '%s\n' '.size je_copy_from_user_run, .-je_copy_from_user_run'
    } >"$selftest_file"
    if check_file "$selftest_file" >/dev/null 2>&1; then
        echo "AArch64 usercopy boundary: FAIL self-test missed $form" >&2
        exit 1
    fi
done

if ! check_file "$source_file"; then
    echo "AArch64 usercopy boundary: FAIL leaf register/stack/instruction discipline" >&2
    exit 1
fi

for label in je_copy_user_load_site je_copy_user_store_site je_copy_user_fixup; do
    if ! grep -q "^${label}:" "$source_file"; then
        echo "AArch64 usercopy boundary: FAIL missing $label" >&2
        exit 1
    fi
done

echo "AArch64 usercopy boundary: PASS (approved leaf instructions, x0-x3+x30 only, no stack/link)"
