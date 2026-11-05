/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP2 PR-2a hosted-test seam for gic.c (ADR 0012). Compiled ONLY when
 * JE_ARM_GIC_HOSTED_TEST is defined (tests/test_gic_bits.c); the kernel build
 * never includes this header and keeps the raw volatile MMIO + mrs/msr paths.
 *
 * Under the define, every gic.c register access goes through exactly one of the
 * hooks below, which the hosted test implements over a fake GICD/GICC register
 * file and fake CNT* system registers. The seam is deliberately at the lowest
 * level (one hook per MMIO width / per sysreg) so the fake observes the exact
 * sequence, width, offset and value of every access — including NON-accesses
 * (e.g. "spurious path wrote no EOIR"), which a serial marker cannot prove.
 *
 * Addresses passed to the MMIO hooks are the full physical address
 * (JE_GICD_BASE/JE_GICC_BASE + offset), exactly what the kernel dereferences.
 */
#ifndef JEIOS_AARCH64_GIC_HOSTED_H
#define JEIOS_AARCH64_GIC_HOSTED_H

#include <stdint.h>

typedef enum {
    JE_GIC_SYSREG_CNTFRQ_EL0 = 1,
    JE_GIC_SYSREG_CNTPCT_EL0,
    JE_GIC_SYSREG_CNTP_CTL_EL0,
    JE_GIC_SYSREG_CNTP_TVAL_EL0,
    JE_GIC_SYSREG_ISR_EL1
} je_gic_sysreg;

uint32_t je_gic_hosted_read32(uint64_t address);
void je_gic_hosted_write32(uint64_t address, uint32_t value);
uint8_t je_gic_hosted_read8(uint64_t address);
void je_gic_hosted_write8(uint64_t address, uint8_t value);
uint64_t je_gic_hosted_sysreg_read(je_gic_sysreg reg);
void je_gic_hosted_sysreg_write(je_gic_sysreg reg, uint64_t value);
void je_gic_hosted_isb(void);

#endif /* JEIOS_AARCH64_GIC_HOSTED_H */
