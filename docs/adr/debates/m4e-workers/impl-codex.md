# M4 PR-E worker notes

## Current status

Observation time: 2026-10-31T20:19:38+00:00  
Final verification observation: 2026-10-31T20:20:17+00:00  
Repository: `/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/wt-m4e-impl`  
Branch: `wip/m4e-impl`  
Inspected commit: `1df911aa6c58250e7fbc61cebfa8ed8a1958c3a2`  
Working tree: uncommitted; no commit, push, or git-config change was made.

- Implemented: M4 PR-E image conversion, freestanding ring-3 C image, kernel
  image registration, and M4e-s1 fixture.
- Tested: local build, smoke, integration, three documented x86 profiles,
  repeated NX smoke, ELF/image checks, and deterministic user ELF rebuild.
- Planned: peer worker's `tools/test_mkimage.py` must be integrated, followed
  by a clean `make check`.
- Blocked: `make check` and `ASAN_OPTIONS=detect_leaks=0 make check` stop at
  the required `tools/test_mkimage.py` invocation because that peer-owned file
  is absent in this worktree. The Makefile intentionally keeps this as a hard
  dependency.

## Changed files

Assigned files changed:

- `Makefile`
- `kernel/arch/x86_64/user_demo.S`
- `kernel/core/image.c`
- `kernel/core/main.c`
- `tools/mkimage.py`
- `tools/smoke.py`
- `tools/check_user_image.py`
- `user/crt0.S`
- `user/user.ld`
- `user/lib/mem.c`
- `user/services/hello.c`

Required uncommitted handoff file:

- `WORKER_NOTES.md`

No frozen headers, tests, or peer-owned `tools/test_mkimage.py` were edited.
Generated output remains under `build/` and is not part of the source change.

## Implementation decisions

1. `mkimage.py` uses only `struct`, bounded integer checks, and the frozen
   program-header rules E1-E10. It parses only the ELF header and program
   headers; the kernel receives the deterministic blob and descriptor and
   never parses ELF. Section parsing and third-party ELF libraries were
   rejected because they are outside D10 and the no-dependency requirement.
2. The image name is `c_hello`, which satisfies the frozen name grammar and
   produces the frozen `je_img_c_hello_*` symbols.
3. The generated comment carries the full input/blob SHA-256 and blob length.
   The Makefile extracts only the first 16 blob-hash characters into a kernel
   compile definition for the required smoke marker. This keeps the runtime
   marker deterministic without adding hash parsing to the kernel.
4. Ring-3 C objects use their own pattern and flags, including
   `-mgeneral-regs-only`, `-fno-tree-loop-distribute-patterns`, and no
   built-in/libc assumptions. The linker script creates separate RX, R, and
   RW PT_LOAD segments, excludes ELF metadata sections, aligns sections to
   4 KiB, and enforces the guard bound.
5. `_start` accumulates all startup-v1 promised-zero GPRs into `r11` and
   executes `ud2` on violation. It calls `je_main`, sends the result through
   the existing EXIT syscall ABI, and has no fallback return path.
6. The hello image checks distinct failure conditions for startup arguments,
   grant count/extra handles, zeroed BSS, initialized and writable data,
   intact rodata, CLOCK_TICKS, and the exact READY send. It grants no
   capability beyond the single SEND end used by the fixture.
7. M4e uses a phase handshake. The kernel audits the child before allowing
   the assembly parent to continue, observes READY before explicitly reaping
   the child, validates DEAD/EXIT zero, then reaps the parent and checks VM,
   endpoint, and process-pool baselines. This ordering was chosen to avoid
   reaping before the READY observation and to avoid the TOCTOU race found
   during the first implementation attempt.
8. The forbidden-instruction checker is a small stdlib-only host tool over
   `nm -u` and `objdump -d`; it rejects SIMD registers, x87/FPU mnemonics,
   save/restore instructions, and `endbr`.
9. The required peer test remains a hard `make check` dependency. Making it
   optional would hide an unverified frozen-tool contract and was rejected.

## Verification commands and results

Read-only preparation and source checks:

- `git rev-parse HEAD` -> `1df911aa6c58250e7fbc61cebfa8ed8a1958c3a2`.
- `git branch --show-current` -> `wip/m4e-impl`.
- `git diff --check` -> pass.
- `python3 -m py_compile tools/mkimage.py tools/smoke.py tools/check_user_image.py` -> pass.
- Source-only search for `M4e debug` -> no matches.
- `readelf -lW build/0/user/hello.elf` -> three non-header-loaded RX/R/RW
  segments, 4 KiB alignment, and no executable GNU stack.
- `nm -u build/0/user/hello.elf` -> empty.
- `python3 tools/check_user_image.py build/0/user/hello.elf` -> pass.

Builds and image metadata:

- `make -B build/0/user/hello.elf build/0/gen/image_c_hello.c build/0/kernel/core/image.o build/0/gen/image_c_hello.o` -> pass.
- `make all` -> pass.
- `python3 tools/mkimage.py --name c_hello --elf build/0/user/hello.elf --verify` -> pass.
- `make -B build/0/user/hello.elf` twice with SHA-256 comparison -> identical.
- Final `make all` after the image-table cleanup -> pass.
- Final `make smoke` after that rebuild -> pass, including the required M4e-s1 marker.
- User ELF size: `13280` bytes.
- User ELF SHA-256: `0981e7ed9fdb9b88933a7d5c71eba3ea765bcd0fd167085a09d3760055b2b3a4`.
- Embedded blob size: `613` bytes.
- Embedded blob SHA-256: `351bc37034ee86c363684c013917239855cc62a43fd68f3898b245829484b8a3`.
- Generated C size: `5532` bytes.

Runtime and regression verification:

- `make smoke` -> pass; observed the required M4e-s1 marker and normal halt.
- `make integration` -> all fault modes 1 through 11 pass.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --expect-nx absent` -> pass.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M --expect-nx absent` -> pass.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64 --memory 1536M --expect-nx present` -> pass.
- Five repeated `python3 tools/smoke.py build/0/jeios.iso --expect-nx present`
  runs after `make smoke` -> all pass; each observed
  `image sha256=351bc37034ee86c3`.
- `make check` -> all local tests, ELF checks, mkimage verification, and
  user-image checks pass; then fails because
  `tools/test_mkimage.py` is absent.
- `ASAN_OPTIONS=detect_leaks=0 make check` -> same result; no earlier ASAN
  failure observed, but the target is blocked at the absent peer test.

## Not verified here

- The peer worker's `tools/test_mkimage.py` and its adversarial cases were not
  available in this worktree.
- No remote CI, independent human review, ARM qualification, or release
  qualification was observed.

## Handoff

The lead should integrate the listed source files and the uncommitted
`WORKER_NOTES.md`, then integrate the peer's `tools/test_mkimage.py` and rerun
`make check` plus the final smoke/build evidence from the integrated tree.
The next bounded task is to resolve the peer-test dependency; pass criteria are
that `python3 tools/test_mkimage.py` exists, passes, and both plain and ASAN
`make check` complete with exit status zero.
