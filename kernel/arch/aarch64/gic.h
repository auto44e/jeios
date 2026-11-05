/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP2 PR-2a (ADR 0012 D2/D3/D4/D6): aarch64 GICv2 + architected (CNTP) timer
 * bring-up, used POLLED with IRQs still masked. The returning IRQ path is PR-2b.
 *
 * THIS HEADER IS THE FROZEN CONTRACT between the implementation (gic.c), the
 * hosted test (tests/test_gic_bits.c, over a fake MMIO backend), and the ARM
 * glue (main.c). Register values are pinned in gic.c against GICv2 (IHI0048),
 * DDI0487 and the QEMU virt-8.2 DTB (GICD 0x08000000, GICC 0x08010000, CNTP
 * INTID 30, level-sensitive); _Static_assert / readback guard them.
 *
 * Pinned support-tier addresses (ADR 0012 D2; flex: DTB discovery later):
 *   GICD = 0x08000000 (one 4 KiB Device-nGnRnE page; all used regs < 0x1000)
 *   GICC = 0x08010000 (one 4 KiB Device-nGnRnE page; GICC_DIR at +0x1000 is
 *                      deliberately left UNMAPPED so an EOImode path faults)
 * The glue adds both pages to the W^X map as JE_MAP_DEVICE before calling init.
 *
 * Caller context: EL1, one CPU, MMU on, DAIF.I MASKED throughout PR-2a. gic.c
 * reads/writes only the two Device pages and the CNTP_* / CNTFRQ_EL0 / ISR_EL1
 * system registers. No allocation. No vector/exception involvement in 2a.
 */
#ifndef JEIOS_AARCH64_GIC_H
#define JEIOS_AARCH64_GIC_H

#include <stdint.h>
#include <stdbool.h>
#include <jeios/status.h>

#define JE_GICD_BASE UINT64_C(0x08000000)
#define JE_GICC_BASE UINT64_C(0x08010000)
#define JE_GIC_PAGE  UINT64_C(0x1000)

/* The CNTP non-secure EL1 physical timer PPI on this machine (DTB-confirmed). */
#define JE_TIMER_INTID 30u
#define JE_GIC_SPURIOUS_MIN 1020u   /* INTID >= this is special/spurious */
#define JE_TIMER_HZ 100u

/*
 * Identify + configure the GICv2 for the single CNTP PPI and enable the CPU
 * interface, with IRQs still masked. Fails closed (JE_ERR_*) on: a GICD/GICC
 * readback inconsistent with a secure-off GICv2 (SecurityExtn set, zero/implausible
 * IIDR), group-1 readback (must be group 0), or a PMR/enable readback mismatch.
 * On success the distributor + CPU interface are enabled and INTID 30 is enabled
 * at priority 0x80 with PMR 0xFF, group 0, FIQEn=0, EOImode=0. Does NOT arm the
 * timer and does NOT unmask IRQs. `evidence` (optional, may be NULL) receives the
 * readback values for marker emission.
 */
typedef struct {
    uint32_t gicd_typer, gicd_iidr, gicc_iidr;
    uint32_t gicc_ctlr, gicc_pmr;      /* post-config readbacks */
    uint32_t gicd_ctlr, isenabler0;    /* post-config readbacks */
    uint32_t igroupr0;                 /* GICD_IGROUPR0 readback (bit 30 must be 0) */
    uint32_t ipriority30;              /* GICD_IPRIORITYR byte 30 readback (0x80) */
} je_gic_evidence;
je_status je_gic_init(je_gic_evidence *evidence);

/*
 * Read CNTFRQ_EL0, derive the 100 Hz interval, and ARM CNTP (TVAL then
 * CTL=ENABLE,IMASK=0). Fails closed if CNTFRQ_EL0 < JE_TIMER_HZ or the derived
 * interval exceeds INT32_MAX (TVAL is signed 32-bit). *out_freq and *out_interval
 * (optional) receive the values. Does NOT unmask IRQs.
 */
je_status je_timer_arm(uint64_t *out_freq, uint32_t *out_interval);

/*
 * One acknowledge/handle/EOI cycle, POLLED (PR-2a) or from the IRQ handler
 * (PR-2b reuses it). Reads GICC_IAR exactly once. If INTID >= 1020 (spurious):
 * sets *was_spurious=true, writes NO EOIR, returns JE_OK without bumping ticks.
 * If INTID == JE_TIMER_INTID: samples CNTPCT (into *out_cntpct if non-NULL),
 * requires CNTP_CTL.ISTATUS==1, re-arms TVAL + isb, bumps the tick counter, then
 * writes the exact IAR to GICC_EOIR. Any other INTID: EOIRs it then returns
 * JE_ERR_INVALID (invariant break — only INTID 30 is enabled). `interval` is the
 * value je_timer_arm derived (needed to re-arm).
 */
je_status je_gic_handle_one(uint32_t interval, bool *was_spurious,
                            uint64_t *out_cntpct);

/* Poll until the timer line is pending (GICD_ISPENDR0 bit 30 / GICC_HPPIR==30)
 * or a bounded CNTPCT deadline of `max_cntpct_wait` ticks elapses. Returns true
 * if pending, false on deadline (the caller then emits TIMER FAIL no-tick).
 * PR-2a only; PR-2b takes the interrupt instead of polling. */
bool je_timer_poll_pending(uint64_t max_cntpct_wait);

/* Monotonic 100 Hz tick counter (ADR 0012 D7 / ADR 0010 D6 reworded). Mirrors
 * x86 je_timer_ticks semantics. */
uint64_t je_arm_timer_ticks(void);

/* Spurious-interrupt count (evidence; must stay 0 in the normal timer proof and
 * become 1 in the spurious test). */
uint64_t je_arm_spurious_count(void);

/* Current raw ISR_EL1 (bit 7 = I pending) and CNTPCT_EL0 — small readback
 * helpers the proof + oracle use; pure sysreg reads. */
uint64_t je_arm_read_isr_el1(void);
uint64_t je_arm_read_cntpct(void);

#endif /* JEIOS_AARCH64_GIC_H */
