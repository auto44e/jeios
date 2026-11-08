/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP2 PR-2a (ADR 0012 D2/D3/D4/D6): GICv2 + CNTP bring-up.
 *
 * This file deliberately contains no exception/vector code.  The caller keeps
 * DAIF.I masked while the GIC is configured and while the PR-2a proof polls the
 * pending state.  The MMIO pages are mapped as Device-nGnRnE by main.c.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <jeios/status.h>
#include "gic.h"
#ifdef JE_ARM_GIC_HOSTED_TEST
#include "gic_hosted.h"
#endif

#define GICD_CTLR             UINT32_C(0x000)
#define GICD_TYPER            UINT32_C(0x004)
#define GICD_IIDR             UINT32_C(0x008)
#define GICD_IGROUPR0         UINT32_C(0x080)
#define GICD_ISENABLER0       UINT32_C(0x100)
#define GICD_ISPENDR0         UINT32_C(0x200)
#define GICD_IPRIORITYR       UINT32_C(0x400)

#define GICC_CTLR             UINT32_C(0x000)
#define GICC_PMR              UINT32_C(0x004)
#define GICC_HPPIR            UINT32_C(0x018)
#define GICC_IAR              UINT32_C(0x00c)
#define GICC_EOIR             UINT32_C(0x010)
#define GICC_IIDR             UINT32_C(0x0fc)

#define GICD_TYPER_SECURITY_EXTN (UINT32_C(1) << 10)
#define CNTP_CTL_ENABLE          UINT64_C(1)
#define CNTP_CTL_IMASK           (UINT64_C(1) << 1)
#define CNTP_CTL_ISTATUS         (UINT64_C(1) << 2)
#define GIC_ARM_IMPLEMENTER      UINT32_C(0x43b)
#define GIC_PRIORITY              UINT8_C(0x80)

_Static_assert(JE_GICD_BASE == UINT64_C(0x08000000),
               "ADR 0012 pins the GICD base");
_Static_assert(JE_GICC_BASE == UINT64_C(0x08010000),
               "ADR 0012 pins the GICC base");
_Static_assert(JE_GIC_PAGE == UINT64_C(0x1000),
               "GIC mappings are one 4 KiB Device page");
_Static_assert((JE_GICD_BASE & (JE_GIC_PAGE - UINT64_C(1))) == 0u,
               "GICD must be page aligned");
_Static_assert((JE_GICC_BASE & (JE_GIC_PAGE - UINT64_C(1))) == 0u,
               "GICC must be page aligned");
_Static_assert(JE_TIMER_INTID < 32u,
               "PR-2a uses the banked INTID 30 register bit");
_Static_assert(JE_GIC_SPURIOUS_MIN == 1020u,
               "GICv2 special INTIDs start at 1020");
_Static_assert(JE_TIMER_HZ == 100u,
               "ADR 0012 pins a 100 Hz timer");
_Static_assert(GICD_CTLR == UINT32_C(0x000) &&
                   GICD_TYPER == UINT32_C(0x004) &&
                   GICD_IIDR == UINT32_C(0x008) &&
                   GICD_IGROUPR0 == UINT32_C(0x080) &&
                   GICD_ISENABLER0 == UINT32_C(0x100) &&
                   GICD_ISPENDR0 == UINT32_C(0x200),
               "GICD offsets are pinned by ADR 0012");
_Static_assert(GICC_CTLR == UINT32_C(0x000) &&
                   GICC_PMR == UINT32_C(0x004) &&
                   GICC_HPPIR == UINT32_C(0x018) &&
                   GICC_IAR == UINT32_C(0x00c) &&
                   GICC_EOIR == UINT32_C(0x010) &&
                   GICC_IIDR == UINT32_C(0x0fc),
               "GICC offsets are pinned by ADR 0012");
_Static_assert(GICD_IPRIORITYR + JE_TIMER_INTID == UINT32_C(0x41e),
               "INTID 30 priority byte offset is pinned");
_Static_assert((uint64_t)INT32_MAX >= UINT64_C(0x7fffffff),
               "TVAL range must be a signed 32-bit range");

static volatile uint64_t je_arm_ticks;
static volatile uint64_t je_arm_spurious;
static volatile uint32_t je_arm_last_iar;

#ifdef JE_ARM_GIC_HOSTED_TEST
/* Hosted-test seam (gic_hosted.h): every access goes through one test hook so
 * the fake backend sees the exact access sequence. Never in the kernel build. */
static inline uint32_t gic_read32(uint64_t base, uint32_t offset)
{
    return je_gic_hosted_read32(base + (uint64_t)offset);
}

static inline void gic_write32(uint64_t base, uint32_t offset, uint32_t value)
{
    je_gic_hosted_write32(base + (uint64_t)offset, value);
}

static inline uint8_t gic_read8(uint64_t base, uint32_t offset)
{
    return je_gic_hosted_read8(base + (uint64_t)offset);
}

static inline void gic_write8(uint64_t base, uint32_t offset, uint8_t value)
{
    je_gic_hosted_write8(base + (uint64_t)offset, value);
}

static inline void arm_isb(void) { je_gic_hosted_isb(); }

static inline uint64_t arm_read_cntfrq(void)
{
    return je_gic_hosted_sysreg_read(JE_GIC_SYSREG_CNTFRQ_EL0);
}

static inline uint64_t arm_read_cntpct(void)
{
    return je_gic_hosted_sysreg_read(JE_GIC_SYSREG_CNTPCT_EL0);
}

static inline uint64_t arm_read_cntp_ctl(void)
{
    return je_gic_hosted_sysreg_read(JE_GIC_SYSREG_CNTP_CTL_EL0);
}

static inline void arm_write_cntp_ctl(uint64_t value)
{
    je_gic_hosted_sysreg_write(JE_GIC_SYSREG_CNTP_CTL_EL0, value);
}

static inline void arm_write_cntp_tval(uint32_t value)
{
    je_gic_hosted_sysreg_write(JE_GIC_SYSREG_CNTP_TVAL_EL0, (uint64_t)value);
}

static inline uint64_t arm_read_isr_el1(void)
{
    return je_gic_hosted_sysreg_read(JE_GIC_SYSREG_ISR_EL1);
}
#else
static inline uint32_t gic_read32(uint64_t base, uint32_t offset)
{
    volatile uint32_t *address =
        (volatile uint32_t *)(uintptr_t)(base + (uint64_t)offset);
    return *address;
}

static inline void gic_write32(uint64_t base, uint32_t offset, uint32_t value)
{
    volatile uint32_t *address =
        (volatile uint32_t *)(uintptr_t)(base + (uint64_t)offset);
    *address = value;
}

static inline uint8_t gic_read8(uint64_t base, uint32_t offset)
{
    volatile uint8_t *address =
        (volatile uint8_t *)(uintptr_t)(base + (uint64_t)offset);
    return *address;
}

static inline void gic_write8(uint64_t base, uint32_t offset, uint8_t value)
{
    volatile uint8_t *address =
        (volatile uint8_t *)(uintptr_t)(base + (uint64_t)offset);
    *address = value;
}

static inline void arm_isb(void)
{
    __asm__ volatile("isb" ::: "memory");
}

static inline uint64_t arm_read_cntfrq(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(value) : : "memory");
    return value;
}

static inline uint64_t arm_read_cntpct(void)
{
    uint64_t value;
    /* ISB before the counter read: CNTPCT_EL0 reads can occur speculatively /
     * out of order (DDI0487), so without this the "sample CNTPCT before re-arm"
     * ordering the delta>=interval bound relies on holds only in program order,
     * not architecturally. Matches Linux __arch_counter_get_cntpct. */
    __asm__ volatile("isb" ::: "memory");
    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(value) : : "memory");
    return value;
}

static inline uint64_t arm_read_cntp_ctl(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, CNTP_CTL_EL0" : "=r"(value) : : "memory");
    return value;
}

static inline void arm_write_cntp_ctl(uint64_t value)
{
    __asm__ volatile("msr CNTP_CTL_EL0, %0" :: "r"(value) : "memory");
}

static inline void arm_write_cntp_tval(uint32_t value)
{
    __asm__ volatile("msr CNTP_TVAL_EL0, %0"
                     :: "r"((uint64_t)value) : "memory");
}

static inline uint64_t arm_read_isr_el1(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, ISR_EL1" : "=r"(value) : : "memory");
    return value;
}

#endif /* JE_ARM_GIC_HOSTED_TEST */

static bool gic_iidr_plausible(uint32_t value)
{
    return value != 0u && (value & UINT32_C(0xfff)) == GIC_ARM_IMPLEMENTER;
}

je_status je_gic_init(je_gic_evidence *evidence)
{
    const uint32_t timer_bit = UINT32_C(1) << JE_TIMER_INTID;
    je_gic_evidence readback = { 0 };

    /* Quiesce CNTP before changing distributor or CPU-interface state. */
    arm_write_cntp_ctl(CNTP_CTL_IMASK);
    arm_isb();

    /* Identify the device by READ-ONLY registers BEFORE the first write: on a
     * wrong machine / bad base a blind GICD_CTLR write would hit an
     * unidentified device. Refuse a non-secure-off GICv2. */
    readback.gicd_typer = gic_read32(JE_GICD_BASE, GICD_TYPER);
    readback.gicd_iidr = gic_read32(JE_GICD_BASE, GICD_IIDR);
    readback.gicc_iidr = gic_read32(JE_GICC_BASE, GICC_IIDR);
    if ((readback.gicd_typer & GICD_TYPER_SECURITY_EXTN) != 0u)
        return JE_ERR_DENIED;
    if (!gic_iidr_plausible(readback.gicd_iidr) ||
        !gic_iidr_plausible(readback.gicc_iidr))
        return JE_ERR_UNSUPPORTED;

    gic_write32(JE_GICD_BASE, GICD_CTLR, 0u);
    if (gic_read32(JE_GICD_BASE, GICD_CTLR) != 0u) return JE_ERR_IO;

    readback.igroupr0 = gic_read32(JE_GICD_BASE, GICD_IGROUPR0);
    if ((readback.igroupr0 & timer_bit) != 0u)
        return JE_ERR_INVALID;

    gic_write8(JE_GICD_BASE, GICD_IPRIORITYR + JE_TIMER_INTID,
               GIC_PRIORITY);
    readback.ipriority30 =
        gic_read8(JE_GICD_BASE, GICD_IPRIORITYR + JE_TIMER_INTID);
    if (readback.ipriority30 != GIC_PRIORITY)
        return JE_ERR_IO;

    /* The PPI is banked/fixed and level-sensitive; do not write ICFGR/ITARGETSR. */
    gic_write32(JE_GICD_BASE, GICD_ISENABLER0, timer_bit);
    readback.isenabler0 = gic_read32(JE_GICD_BASE, GICD_ISENABLER0);
    if ((readback.isenabler0 & timer_bit) == 0u) return JE_ERR_IO;

    gic_write32(JE_GICD_BASE, GICD_CTLR, 1u);
    readback.gicd_ctlr = gic_read32(JE_GICD_BASE, GICD_CTLR);
    if (readback.gicd_ctlr != 1u) return JE_ERR_IO;

    gic_write32(JE_GICC_BASE, GICC_PMR, UINT32_C(0xff));
    readback.gicc_pmr = gic_read32(JE_GICC_BASE, GICC_PMR);
    if (readback.gicc_pmr != UINT32_C(0xff)) return JE_ERR_IO;

    /* Plain 1: EnableGrp0=1, FIQEn=0, EOImode=0. */
    gic_write32(JE_GICC_BASE, GICC_CTLR, 1u);
    readback.gicc_ctlr = gic_read32(JE_GICC_BASE, GICC_CTLR);
    if (readback.gicc_ctlr != 1u) return JE_ERR_IO;

    if (evidence != NULL) *evidence = readback;
    return JE_OK;
}

je_status je_timer_arm(uint64_t *out_freq, uint32_t *out_interval)
{
    /* CNTFRQ_EL0[63:32] are RES0 (DDI0487), so read and mask to [31:0] per
     * ADR 0012 D3. (A hostile/garbage upper half cannot change the frequency.)
     * The INT32_MAX guard below still fails closed for any in-range value whose
     * /100 interval could not be represented in the signed-32-bit TVAL field —
     * unreachable for a 32-bit frequency, but kept as an explicit invariant. */
    const uint32_t frequency = (uint32_t)arm_read_cntfrq();
    if (frequency < JE_TIMER_HZ) return JE_ERR_INVALID;
    const uint32_t interval = frequency / JE_TIMER_HZ;
    if (interval == 0u) return JE_ERR_INVALID;
    if ((uint64_t)interval > (uint64_t)INT32_MAX) return JE_ERR_UNSUPPORTED;

    arm_write_cntp_ctl(CNTP_CTL_IMASK);
    arm_isb();
    arm_write_cntp_tval(interval);
    arm_write_cntp_ctl(CNTP_CTL_ENABLE);
    arm_isb();

    if (out_freq != NULL) *out_freq = (uint64_t)frequency;
    if (out_interval != NULL) *out_interval = interval;
    return JE_OK;
}

je_status je_gic_handle_one(uint32_t interval, bool *was_spurious,
                            uint64_t *out_cntpct)
{
    if (interval == 0u || (uint64_t)interval > (uint64_t)INT32_MAX ||
        was_spurious == NULL)
        return JE_ERR_INVALID;

    *was_spurious = false;
    const uint32_t iar = gic_read32(JE_GICC_BASE, GICC_IAR);
    je_arm_last_iar = iar;
    const uint32_t intid = iar & UINT32_C(0x3ff);

    if (intid >= JE_GIC_SPURIOUS_MIN) {
        *was_spurious = true;
        ++je_arm_spurious;
        return JE_OK;
    }

    if (intid != JE_TIMER_INTID) {
        gic_write32(JE_GICC_BASE, GICC_EOIR, iar);
        return JE_ERR_INVALID;
    }

    const uint64_t cntpct = arm_read_cntpct();
    if (out_cntpct != NULL) *out_cntpct = cntpct;
    if ((arm_read_cntp_ctl() & CNTP_CTL_ISTATUS) == 0u) {
        /* The timer condition already cleared before we acked (a benign re-pend
         * race on real HW: TVAL+isb need not have dropped the level line at the
         * GIC before this ack). NOT an I/O error — EOI the acked interrupt, do
         * NOT bump the tick, and report JE_ERR_BUSY so the IRQ-path caller
         * (PR-2b) treats it as a spurious-but-handled wakeup, not a fatal. */
        gic_write32(JE_GICC_BASE, GICC_EOIR, iar);
        return JE_ERR_BUSY;
    }

    arm_write_cntp_tval(interval);
    arm_isb();
    ++je_arm_ticks;
    gic_write32(JE_GICC_BASE, GICC_EOIR, iar);
    return JE_OK;
}

bool je_timer_poll_pending(uint64_t max_cntpct_wait)
{
    const uint64_t start = arm_read_cntpct();

    for (;;) {
        const uint32_t distributor_pending =
            gic_read32(JE_GICD_BASE, GICD_ISPENDR0);
        const uint32_t cpu_pending = gic_read32(JE_GICC_BASE, GICC_HPPIR) &
                                     UINT32_C(0x3ff);
        const uint32_t timer_bit = UINT32_C(1) << JE_TIMER_INTID;
        if ((distributor_pending & timer_bit) != 0u ||
            cpu_pending == JE_TIMER_INTID)
            return true;

        if (arm_read_cntpct() - start > max_cntpct_wait) return false;
    }
}

uint64_t je_arm_timer_ticks(void)
{
    return je_arm_ticks;
}

uint64_t je_arm_spurious_count(void)
{
    return je_arm_spurious;
}

uint32_t je_gic_last_iar(void)
{
    return je_arm_last_iar;
}

uint64_t je_arm_read_isr_el1(void)
{
    return arm_read_isr_el1();
}

uint64_t je_arm_read_cntpct(void)
{
    return arm_read_cntpct();
}
