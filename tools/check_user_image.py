#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the freestanding ring-3 image's undefined symbols and instructions."""
import re
import subprocess
import sys


def run(command):
    return subprocess.run(command, check=False, capture_output=True, text=True)


def main(argv):
    if len(argv) != 1:
        sys.stderr.write("usage: check_user_image.py IMAGE\n")
        return 1
    image = argv[0]
    undefined = run(["nm", "-u", image])
    if undefined.returncode != 0:
        sys.stderr.write("check_user_image: nm failed\n")
        return 1
    if undefined.stdout.strip():
        sys.stderr.write("check_user_image: undefined symbols:\n")
        sys.stderr.write(undefined.stdout)
        return 1
    disassembly = run(["objdump", "-d", image])
    if disassembly.returncode != 0:
        sys.stderr.write("check_user_image: objdump failed\n")
        return 1
    forbidden_register = re.compile(r"\b(?:xmm|ymm|zmm|mm)[0-9]+\b", re.IGNORECASE)
    forbidden_mnemonic = re.compile(
        r"\b(?:f[a-z][a-z0-9]*|fx(?:save|rstor)|xsave(?:c|opt|s)?|xrstor(?:s)?)\b",
        re.IGNORECASE,
    )
    for line in disassembly.stdout.splitlines():
        if "endbr" in line.lower() or forbidden_register.search(line):
            sys.stderr.write("check_user_image: forbidden instruction/register: {}\n".format(line))
            return 1
        match = re.search(r":\s+(?:[0-9a-f]{2}\s+)+(\S+)", line, re.IGNORECASE)
        if match and forbidden_mnemonic.fullmatch(match.group(1)):
            sys.stderr.write("check_user_image: forbidden instruction: {}\n".format(line))
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
