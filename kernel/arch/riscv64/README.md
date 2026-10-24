# RISC-V architecture scaffold

Reserved home for the shared fresh C kernel's RV64 port. Planning only: there is
no boot entry, linker script, trap implementation or runnable RISC-V kernel here.
See [the staged port plan](../../../docs/RISCV.md) and
[target manifest](../../../config/riscv64.json).

Expected future modules (names are planning, not existing interfaces):
- boot.S/linker.ld: reviewed firmware handoff, stack/BSS/image layout;
- platform.c: bounded console, copied boot context and SBI discovery;
- trap entry/frame: complete saved state and fault diagnostics;
- paging/VM backend: Sv39 mappings, permission checks and TLB maintenance;
- interrupt/timer/context bridges: scheduler integration;
- user entry/copy bridges: U-mode isolation and narrowly scoped recovery.

Do not copy AMD64/ARM register layouts or add successful no-op implementations.
Shared portable mechanisms remain in kernel/core. New executable work requires
an accepted RV0 contract and native emulator evidence at each gate.
