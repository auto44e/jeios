/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP2 PR-2a (ADR 0012 D2/D3/D4/D6/D8): hosted test of the aarch64 GICv2 + CNTP
 * bring-up (kernel/arch/aarch64/gic.c) over a FAKE register backend.
 *
 * Author separation (AGENTS.md multi-agent model): this file was written by a
 * different agent from gic.c. Every expected register offset, bit and value is
 * re-derived here from GICv2 (IHI0048B) / DDI0487 with this file's OWN macros;
 * none of gic.c's private macros are reused as the oracle.
 *
 * Seam: built with -DJE_ARM_GIC_HOSTED_TEST, gic.c routes every MMIO access and
 * every CNT* / ISR_EL1 sysreg access through the hooks declared in gic_hosted.h,
 * implemented below over a fake GICD/GICC register file. The fake logs every
 * access in order, so the test proves NON-events a serial marker cannot (the
 * spurious path writes no EOIR; a refused CNTFRQ never enables the timer; init
 * never touches ICFGR/ITARGETSR; GICC_IAR is read exactly once).
 *
 * The fake MODELS the behaviour the properties depend on, so they are not
 * merely "a write occurred" checks (cross-review hardening, 2026-11-05):
 *   - CNTP (DDI0487 D11.2.4 TimerValue view): a TVAL write sets
 *     CVAL = CNTPCT + SignExtend(TVAL[31:0]); ISTATUS = ENABLE && CNTPCT >= CVAL;
 *     the timer output (the PPI level) = ISTATUS && !IMASK. A TVAL write also
 *     advances the fake CNTPCT by `rearm_advance` so a CNTPCT sample taken
 *     AFTER the re-arm is observably different from one taken before it.
 *   - GICD_ISPENDR0 bit 30 reflects the modelled timer level.
 *   - GICC_IAR is a one-shot acknowledge: the first read returns the queued
 *     INTID and consumes it; every later read returns 1023 (spurious).
 *   - GICD_CTLR has independent "disable does not take" and "post-enable
 *     readback wrong" faults, so each readback is covered on its own.
 *   - Any access outside the modelled registers is a violation, and EVERY test
 *     case ends with F.violations == 0 (end_case). A self-test proves the
 *     violation detector is live.
 *
 * What this does NOT prove (covered only by the QEMU smoke, or by PR-2b): that
 * the real QEMU GIC behaves like this fake; Device-nGnRnE mapping of the pages;
 * actual interrupt delivery / vector entry (IRQs stay masked in PR-2a).
 *
 * Each check names the mutation it defeats ("M: ...").
 */
#undef NDEBUG
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jeios/status.h>
#include "gic.h"
#include "gic_hosted.h"

/* ---- Independent GICv2 (IHI0048B §4) / DDI0487 layouts ---- */
#define GICD_PA UINT64_C(0x08000000)   /* QEMU virt-8.2 DTB intc reg[0] */
#define GICC_PA UINT64_C(0x08010000)   /* QEMU virt-8.2 DTB intc reg[2] */
#define PAGE    UINT64_C(0x1000)
#define D_CTLR      0x000u
#define D_TYPER     0x004u
#define D_IIDR      0x008u
#define D_IGROUPR0  0x080u
#define D_ISENABLER0 0x100u
#define D_ISPENDR0  0x200u
#define D_IPRIO     0x400u   /* byte-addressable, 0x400..0x7FF */
#define D_ITARGETS  0x800u   /* must NOT be written for the PPI */
#define D_ICFGR     0xC00u   /* must NOT be written for the PPI */
#define C_CTLR      0x000u
#define C_PMR       0x004u
#define C_IAR       0x00Cu
#define C_EOIR      0x010u
#define C_HPPIR     0x018u
#define C_IIDR      0x0FCu
#define C_DIR       0x1000u  /* GICC_DIR: second GICC page, deliberately unmapped */
#define TYPER_SECEXT (1u << 10)
#define CCTLR_EN_GRP0 (1u << 0)
#define CCTLR_FIQEN   (1u << 3)
#define CCTLR_EOIMODE (1u << 9)
#define CTL_ENABLE  UINT64_C(1)
#define CTL_IMASK   (UINT64_C(1) << 1)
#define CTL_ISTATUS (UINT64_C(1) << 2)
#define TIMER_ID 30u
#define TBIT (1u << TIMER_ID)
#define SPURIOUS 1023u

_Static_assert(JE_GICD_BASE == GICD_PA && JE_GICC_BASE == GICC_PA,
               "gic.h pinned bases must equal the DTB-derived values");
_Static_assert(JE_TIMER_INTID == TIMER_ID && JE_GIC_SPURIOUS_MIN == 1020u &&
                   JE_TIMER_HZ == 100u,
               "gic.h pinned INTID/spurious/HZ");
/* A 32-bit CNTFRQ field (DDI0487 D17: CNTFRQ_EL0[31:0]) divided by 100 can
 * never exceed INT32_MAX, so with the pinned [31:0] mask the interval>INT32_MAX
 * refusal in je_timer_arm is unreachable. If the field is ever widened this
 * must be revisited. (je_gic_handle_one's own >INT32_MAX argument refusal IS
 * reachable and is tested below.) */
_Static_assert(UINT32_MAX / 100u < (uint32_t)INT32_MAX,
               "masked CNTFRQ/100 always fits TVAL");
/* 300 GHz masked to [31:0]: 300e9 mod 2^32 = 3647256576 (69 * 2^32 =
 * 296352743424); /100 = 36472565. Independently re-derived here. */
#define F300G       UINT64_C(300000000000)
#define F300G_LOW   UINT32_C(3647256576)
#define F300G_IVL   UINT32_C(36472565)
_Static_assert((F300G & UINT32_MAX) == F300G_LOW, "300 GHz low word");
_Static_assert(F300G - UINT64_C(69) * (UINT64_C(1) << 32) == F300G_LOW,
               "300 GHz low word (independent form)");
_Static_assert(F300G_LOW / 100u == F300G_IVL, "300 GHz masked interval");

/* ---- Fake backend ---- */
enum { EV_R32 = 1, EV_W32, EV_R8, EV_W8, EV_SR, EV_SW, EV_ISB };
typedef struct { int kind; uint64_t where; uint64_t value; } event;

static struct {
    /* configuration (what the "hardware" reports) */
    uint32_t typer, gicd_iidr, gicc_iidr, igroupr0;
    uint32_t enable_fixed;        /* SGIs always-enabled bits (QEMU: 0xffff) */
    uint32_t enable_drop;         /* ISENABLER0 bits that do not stick */
    uint32_t gicd_ctlr_or_off;    /* OR'd into GICD_CTLR readback while disabled */
    uint32_t gicd_ctlr_or_on;     /* OR'd into GICD_CTLR readback while enabled */
    bool gicd_enable_drop;        /* a GICD_CTLR write of 1 does not stick */
    uint32_t pmr_impl;            /* implemented PMR bits (readback mask) */
    uint32_t gicc_ctlr_stuck;     /* OR'd into GICC_CTLR readback */
    uint8_t prio_impl;            /* implemented priority bits */
    uint32_t iar;                 /* queued INTID for the one-shot IAR */
    bool iar_armed;
    uint32_t hppir, ispendr0;
    unsigned ispendr_after;       /* ISPENDR0 reads before ispendr0 appears */
    uint64_t cntfrq, cntpct, cntpct_step, rearm_advance;
    /* state */
    uint32_t gicd_ctlr, enabled, gicc_ctlr, pmr;
    uint8_t prio[0x400];
    uint64_t cntp_ctl;            /* ENABLE|IMASK as written */
    uint64_t cval;                /* modelled CompareValue */
    uint64_t tval;                /* last raw TVAL write */
    /* counters */
    unsigned iar_reads, eoir_writes, tval_writes, ispendr_reads, cntpct_reads;
    uint32_t last_eoir;
    unsigned violations;
    bool quiet;                   /* self-test: expected violations, no noise */
    event log[1024];
    unsigned n;
} F;

static void fake_log(int kind, uint64_t where, uint64_t value)
{
    assert(F.n < sizeof F.log / sizeof F.log[0]);
    F.log[F.n++] = (event){ kind, where, value };
}

static void violation(const char *what, uint64_t address)
{
    if (!F.quiet)
        fprintf(stderr, "gic_bits: fake: unmodeled %s at 0x%llx\n", what,
            (unsigned long long)address);
    ++F.violations;
}

/* DDI0487 D11.2.4: ISTATUS = (CNTPCT - CVAL) >= 0 while ENABLE; the timer
 * output asserted to the GIC is ISTATUS && !IMASK. (With ENABLE=0 ISTATUS is
 * UNKNOWN architecturally; the fake reports 0.) */
static bool timer_istatus(void)
{
    return (F.cntp_ctl & CTL_ENABLE) != 0u && F.cntpct >= F.cval;
}

static bool timer_line(void)
{
    return timer_istatus() && (F.cntp_ctl & CTL_IMASK) == 0u;
}

/* QEMU virt-8.2 GICv2 (gic-version=2, secure=off) good identity, as observed
 * on the real boot: TYPER 0x8, GICD_IIDR 0x43b, GICC_IIDR 0x2043b. */
static void fake_reset(void)
{
    memset(&F, 0, sizeof F);
    F.typer = 0x08u;
    F.gicd_iidr = 0x43Bu;
    F.gicc_iidr = 0x2043Bu;
    F.igroupr0 = 0u;
    F.enable_fixed = 0xFFFFu;
    F.pmr_impl = 0xFFu;
    F.prio_impl = 0xFFu;
    F.iar = SPURIOUS;
    F.iar_armed = false;
    F.hppir = SPURIOUS;
    F.cntfrq = 62500000u;
    F.cntpct = UINT64_C(0x123456789);
    F.cntpct_step = 0u;
    F.rearm_advance = 0u;
    F.cntp_ctl = 0u;             /* timer disabled at reset */
    F.cval = UINT64_MAX;
    F.gicd_ctlr = 1u;            /* firmware left it on: init must clear it */
    F.enabled = F.enable_fixed;
}

/* Queue `iar` for the next (and only the next) GICC_IAR read. */
static void queue_iar(uint32_t iar)
{
    F.iar = iar;
    F.iar_armed = true;
}

/* Put the modelled timer in the "fired" state: enabled, unmasked, condition
 * met (CVAL in the past), so the PPI level is asserted. */
static void timer_fired(void)
{
    F.cntp_ctl = CTL_ENABLE;
    F.cval = F.cntpct - UINT64_C(5000);
    assert(timer_line());
}

static bool in_page(uint64_t a, uint64_t base, uint32_t *off)
{
    if (a < base || a >= base + PAGE) return false;
    *off = (uint32_t)(a - base);
    return true;
}

uint32_t je_gic_hosted_read32(uint64_t a)
{
    uint32_t off, v = 0xDEADBEEFu;
    assert((a & 3u) == 0u);  /* -mstrict-align Device: 32-bit accesses aligned */
    if (in_page(a, GICD_PA, &off)) {
        switch (off) {
        case D_CTLR:
            v = F.gicd_ctlr | ((F.gicd_ctlr & 1u) ? F.gicd_ctlr_or_on
                                                  : F.gicd_ctlr_or_off);
            break;
        case D_TYPER: v = F.typer; break;
        case D_IIDR: v = F.gicd_iidr; break;
        case D_IGROUPR0: v = F.igroupr0; break;
        case D_ISENABLER0: v = F.enabled; break;
        case D_ISPENDR0:
            v = (F.ispendr_reads++ >= F.ispendr_after) ? F.ispendr0 : 0u;
            if (timer_line()) v |= TBIT;   /* level-sensitive PPI */
            break;
        default: violation("GICD read32", a);
        }
    } else if (in_page(a, GICC_PA, &off)) {
        switch (off) {
        case C_CTLR: v = F.gicc_ctlr | F.gicc_ctlr_stuck; break;
        case C_PMR: v = F.pmr & F.pmr_impl; break;
        case C_IAR:   /* one-shot acknowledge: consumes the queued INTID */
            ++F.iar_reads;
            v = F.iar_armed ? F.iar : SPURIOUS;
            F.iar_armed = false;
            break;
        case C_HPPIR: v = F.hppir; break;
        case C_IIDR: v = F.gicc_iidr; break;
        default: violation("GICC read32", a);
        }
    } else {
        violation("read32 outside GICD/GICC pages", a);
    }
    fake_log(EV_R32, a, v);
    return v;
}

void je_gic_hosted_write32(uint64_t a, uint32_t v)
{
    uint32_t off;
    assert((a & 3u) == 0u);
    fake_log(EV_W32, a, v);
    if (in_page(a, GICD_PA, &off)) {
        switch (off) {
        case D_CTLR:
            F.gicd_ctlr = v & 1u;
            if (F.gicd_enable_drop) F.gicd_ctlr = 0u;
            if (F.gicd_ctlr_or_off & 1u) F.gicd_ctlr = 1u;  /* disable sticks on */
            break;
        case D_ISENABLER0: F.enabled |= v & ~F.enable_drop; break; /* W1S */
        default: violation("GICD write32", a);
        }
    } else if (in_page(a, GICC_PA, &off)) {
        switch (off) {
        case C_CTLR: F.gicc_ctlr = v; break;
        case C_PMR: F.pmr = v & 0xFFu; break;
        case C_EOIR: ++F.eoir_writes; F.last_eoir = v; break;
        default: violation("GICC write32", a);
        }
    } else {
        violation("write32 outside GICD/GICC pages", a);
    }
}

uint8_t je_gic_hosted_read8(uint64_t a)
{
    uint32_t off;
    uint8_t v = 0xA5u;
    if (in_page(a, GICD_PA, &off) && off >= D_IPRIO && off < D_IPRIO + 0x400u)
        v = (uint8_t)(F.prio[off - D_IPRIO] & F.prio_impl);
    else
        violation("read8", a);
    fake_log(EV_R8, a, v);
    return v;
}

void je_gic_hosted_write8(uint64_t a, uint8_t v)
{
    uint32_t off;
    fake_log(EV_W8, a, v);
    if (in_page(a, GICD_PA, &off) && off >= D_IPRIO && off < D_IPRIO + 0x400u)
        F.prio[off - D_IPRIO] = v;
    else
        violation("write8", a);
}

uint64_t je_gic_hosted_sysreg_read(je_gic_sysreg r)
{
    uint64_t v = 0;
    switch (r) {
    case JE_GIC_SYSREG_CNTFRQ_EL0: v = F.cntfrq; break;
    case JE_GIC_SYSREG_CNTPCT_EL0:
        ++F.cntpct_reads;
        v = F.cntpct;
        F.cntpct += F.cntpct_step;
        break;
    case JE_GIC_SYSREG_CNTP_CTL_EL0:
        v = (F.cntp_ctl & (CTL_ENABLE | CTL_IMASK)) |
            (timer_istatus() ? CTL_ISTATUS : 0u);
        break;
    case JE_GIC_SYSREG_ISR_EL1: v = 0; break;
    default: violation("sysreg read", (uint64_t)r);
    }
    fake_log(EV_SR, (uint64_t)r, v);
    return v;
}

void je_gic_hosted_sysreg_write(je_gic_sysreg r, uint64_t v)
{
    fake_log(EV_SW, (uint64_t)r, v);
    switch (r) {
    case JE_GIC_SYSREG_CNTP_CTL_EL0:
        /* Only ENABLE/IMASK are writable; ISTATUS is RO, the rest RES0. */
        if (v & ~(CTL_ENABLE | CTL_IMASK)) violation("CNTP_CTL RES0/RO bits", v);
        F.cntp_ctl = v & (CTL_ENABLE | CTL_IMASK);
        break;
    case JE_GIC_SYSREG_CNTP_TVAL_EL0:
        if (v >> 32) violation("CNTP_TVAL upper bits", v);
        /* TimerValue view: CVAL = CNTPCT + SignExtend(TVAL[31:0]). */
        F.cval = F.cntpct + (uint64_t)(int64_t)(int32_t)(uint32_t)v;
        F.tval = v;
        ++F.tval_writes;
        F.cntpct += F.rearm_advance;   /* time passes across the re-arm */
        break;
    default: violation("sysreg write", (uint64_t)r);
    }
}

void je_gic_hosted_isb(void) { fake_log(EV_ISB, 0, 0); }

/* ---- helpers ---- */
static unsigned checks;
#define CHECK(cond, ...) do {                                             \
        ++checks;                                                         \
        if (!(cond)) {                                                    \
            fprintf(stderr, "gic_bits: FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                                 \
            fputc('\n', stderr);                                          \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

/* Every case ends here: any unmodeled access (out-of-page, GICC_DIR, wrong
 * width, ICFGR/ITARGETSR, RES0 sysreg bits) is fatal, universally. */
static void end_case(const char *what)
{
    CHECK(F.violations == 0u, "%s: %u unmodeled fake accesses", what,
          F.violations);
}

/* Index of first event matching (kind, where[, value]); -1 if none. */
static int find_ev(int kind, uint64_t where, bool match_value, uint64_t value)
{
    for (unsigned i = 0; i < F.n; ++i)
        if (F.log[i].kind == kind && F.log[i].where == where &&
            (!match_value || F.log[i].value == value))
            return (int)i;
    return -1;
}

static unsigned count_ev(int kind, uint64_t where)
{
    unsigned c = 0;
    for (unsigned i = 0; i < F.n; ++i)
        c += F.log[i].kind == kind && F.log[i].where == where;
    return c;
}

static unsigned count_kind(int kind)
{
    unsigned c = 0;
    for (unsigned i = 0; i < F.n; ++i) c += F.log[i].kind == kind;
    return c;
}

static bool is_mmio(int kind)
{
    return kind == EV_R32 || kind == EV_W32 || kind == EV_R8 || kind == EV_W8;
}

static bool is_mmio_write(int kind) { return kind == EV_W32 || kind == EV_W8; }

static unsigned mmio_writes(void)
{
    return count_kind(EV_W32) + count_kind(EV_W8);
}

static bool wrote32(uint64_t a) { return find_ev(EV_W32, a, false, 0) >= 0; }

/* Any write, of ANY width, landing in the GICC page. */
static bool wrote_gicc(void)
{
    for (unsigned i = 0; i < F.n; ++i)
        if (is_mmio_write(F.log[i].kind) && F.log[i].where >= GICC_PA &&
            F.log[i].where < GICC_PA + PAGE)
            return true;
    return false;
}

/* EOIR touched with ANY width (32-bit or a byte-lane write). */
static bool wrote_eoir_any_width(void)
{
    for (unsigned i = 0; i < F.n; ++i)
        if (is_mmio_write(F.log[i].kind) && F.log[i].where >= GICC_PA + C_EOIR &&
            F.log[i].where < GICC_PA + C_EOIR + 4u)
            return true;
    return false;
}

/* Every write-class event (MMIO + sysreg) in order, compared to `want`. */
static void expect_writes(const event *want, unsigned nwant, const char *what)
{
    unsigned k = 0;
    for (unsigned i = 0; i < F.n; ++i) {
        const event *e = &F.log[i];
        if (e->kind != EV_W32 && e->kind != EV_W8 && e->kind != EV_SW &&
            e->kind != EV_ISB)
            continue;
        CHECK(k < nwant, "%s: extra write #%u kind=%d at 0x%llx=0x%llx", what, k,
              e->kind, (unsigned long long)e->where,
              (unsigned long long)e->value);
        CHECK(e->kind == want[k].kind && e->where == want[k].where &&
                  e->value == want[k].value,
              "%s: write #%u is kind=%d 0x%llx=0x%llx, want kind=%d 0x%llx=0x%llx",
              what, k, e->kind, (unsigned long long)e->where,
              (unsigned long long)e->value, want[k].kind,
              (unsigned long long)want[k].where,
              (unsigned long long)want[k].value);
        ++k;
    }
    CHECK(k == nwant, "%s: %u writes, want %u", what, k, nwant);
}

#define SW(r, v) { EV_SW, (uint64_t)(r), (uint64_t)(v) }
#define ISB { EV_ISB, 0, 0 }
#define WD(off, v) { EV_W32, GICD_PA + (off), (v) }
#define WC(off, v) { EV_W32, GICC_PA + (off), (v) }
#define W8D(off, v) { EV_W8, GICD_PA + (off), (v) }

/* ---- fake self-test: the violation detector is live ---- */
static void test_fake_selfcheck(void)
{
    /* If the detector were dead, every "violations == 0" below would be
     * vacuous. Drive the hooks directly with accesses gic.c must never make. */
    fake_reset();
    F.quiet = true;
    (void)je_gic_hosted_read32(GICC_PA + C_DIR);            /* GICC_DIR, unmapped */
    CHECK(F.violations == 1u, "selfcheck: GICC_DIR read not flagged");
    je_gic_hosted_write32(GICD_PA + D_ICFGR, 0u);
    CHECK(F.violations == 2u, "selfcheck: ICFGR write not flagged");
    je_gic_hosted_write8(GICC_PA + C_EOIR, 30u);           /* byte EOIR */
    CHECK(F.violations == 3u, "selfcheck: byte EOIR not flagged");
    je_gic_hosted_write32(GICD_PA + D_ITARGETS + 28u, 1u);
    CHECK(F.violations == 4u, "selfcheck: ITARGETSR write not flagged");
    je_gic_hosted_sysreg_write(JE_GIC_SYSREG_CNTFRQ_EL0, 1u);
    CHECK(F.violations == 5u, "selfcheck: CNTFRQ write not flagged");
    je_gic_hosted_sysreg_write(JE_GIC_SYSREG_CNTP_CTL_EL0, CTL_ISTATUS);
    CHECK(F.violations == 6u, "selfcheck: CTL.ISTATUS write not flagged");
    (void)je_gic_hosted_read32(UINT64_C(0x09000000));      /* PL011, not GIC */
    CHECK(F.violations == 7u, "selfcheck: out-of-page read not flagged");

    /* The models behave as specified (so the semantic checks are meaningful). */
    fake_reset(); queue_iar(TIMER_ID);
    CHECK(je_gic_hosted_read32(GICC_PA + C_IAR) == TIMER_ID &&
              je_gic_hosted_read32(GICC_PA + C_IAR) == SPURIOUS,
          "selfcheck: IAR must be one-shot");
    fake_reset(); F.cntp_ctl = CTL_ENABLE;
    je_gic_hosted_sysreg_write(JE_GIC_SYSREG_CNTP_TVAL_EL0, 100u);
    CHECK(!timer_istatus() && F.cval == F.cntpct + 100u, "selfcheck: TVAL model");
    F.cntpct += 100u;
    CHECK(timer_line(), "selfcheck: deadline reached -> line asserted");
    je_gic_hosted_sysreg_write(JE_GIC_SYSREG_CNTP_TVAL_EL0, 0x80000000u);
    CHECK(timer_istatus(), "selfcheck: negative TVAL must NOT clear ISTATUS");
    end_case("selfcheck models");
}

/* ---- je_gic_init ---- */
static void test_init_good(void)
{
    fake_reset();
    je_gic_evidence ev;
    memset(&ev, 0xCC, sizeof ev);
    je_status st = je_gic_init(&ev);
    CHECK(st == JE_OK, "good init: status %s", je_status_name(st));
    end_case("good init");

    /* Identify before write (ADR 0012 D3; lead contract change 2026-11-05).
     * M: blind GICD_CTLR=0 write before TYPER/IIDR identification. The FIRST
     * MMIO access must be a READ of GICD_TYPER or GICD_IIDR, and all three ID
     * reads must precede the first MMIO write of any width. */
    int first_mmio = -1, first_mmio_wr = -1;
    for (unsigned i = 0; i < F.n; ++i) {
        if (first_mmio < 0 && is_mmio(F.log[i].kind)) first_mmio = (int)i;
        if (first_mmio_wr < 0 && is_mmio_write(F.log[i].kind))
            first_mmio_wr = (int)i;
    }
    CHECK(first_mmio >= 0 && F.log[first_mmio].kind == EV_R32 &&
              (F.log[first_mmio].where == GICD_PA + D_TYPER ||
               F.log[first_mmio].where == GICD_PA + D_IIDR),
          "good init: first GIC access #%d is kind=%d at 0x%llx, want a READ of "
          "GICD_TYPER/IIDR", first_mmio, first_mmio >= 0 ? F.log[first_mmio].kind : 0,
          first_mmio >= 0 ? (unsigned long long)F.log[first_mmio].where : 0ull);
    int r_typer = find_ev(EV_R32, GICD_PA + D_TYPER, false, 0);
    int r_diidr = find_ev(EV_R32, GICD_PA + D_IIDR, false, 0);
    int r_ciidr = find_ev(EV_R32, GICC_PA + C_IIDR, false, 0);
    CHECK(r_typer >= 0 && r_diidr >= 0 && r_ciidr >= 0 && first_mmio_wr >= 0 &&
              r_typer < first_mmio_wr && r_diidr < first_mmio_wr &&
              r_ciidr < first_mmio_wr,
          "good init: ID reads typer=%d diidr=%d ciidr=%d must precede first "
          "write %d", r_typer, r_diidr, r_ciidr, first_mmio_wr);
    CHECK(F.log[first_mmio_wr].kind == EV_W32 &&
              F.log[first_mmio_wr].where == GICD_PA + D_CTLR &&
              F.log[first_mmio_wr].value == 0u,
          "good init: first write must be the GICD_CTLR=0 quiesce");

    /* Exact pinned write sequence (ADR 0012 D3 steps 1-9). M: TVAL/CTL left
     * enabled; ICFGR/ITARGETSR touched; enable before priority; GICC before
     * GICD; ISENABLER0 written with extra bits (would enable other INTIDs);
     * GICC_CTLR with FIQEn/EOImode; GICD_CTLR left at firmware value. */
    static const event want[] = {
        SW(JE_GIC_SYSREG_CNTP_CTL_EL0, CTL_IMASK), ISB,
        WD(D_CTLR, 0u),
        W8D(D_IPRIO + TIMER_ID, 0x80u),
        WD(D_ISENABLER0, TBIT),
        WD(D_CTLR, 1u),
        WC(C_PMR, 0xFFu),
        WC(C_CTLR, CCTLR_EN_GRP0),
    };
    expect_writes(want, sizeof want / sizeof want[0], "good init");
    CHECK(F.log[0].kind == EV_SW && F.log[1].kind == EV_ISB,
          "good init: CNTP quiesce + isb must be the FIRST accesses");

    /* Final fake register state vs an independent expectation. */
    CHECK(F.gicd_ctlr == 1u, "GICD_CTLR=0x%x", F.gicd_ctlr);
    CHECK(F.gicc_ctlr == 1u && !(F.gicc_ctlr & (CCTLR_FIQEN | CCTLR_EOIMODE)),
          "GICC_CTLR=0x%x", F.gicc_ctlr);
    CHECK(F.pmr == 0xFFu, "PMR=0x%x", F.pmr);
    CHECK(F.enabled == (0xFFFFu | TBIT), "enabled=0x%x", F.enabled);
    CHECK(F.igroupr0 == 0u, "group must stay 0");
    for (unsigned i = 0; i < 0x400u; ++i)
        CHECK(F.prio[i] == (i == TIMER_ID ? 0x80u : 0u),
              "priority byte %u = 0x%x", i, F.prio[i]);
    CHECK(0x80u < F.pmr, "priority 0x80 must be < PMR to signal");
    CHECK((F.cntp_ctl & CTL_ENABLE) == 0u && (F.cntp_ctl & CTL_IMASK) != 0u,
          "init must quiesce (not arm) CNTP: ctl=0x%llx",
          (unsigned long long)F.cntp_ctl);
    CHECK(F.tval_writes == 0u, "init must not arm the timer");
    CHECK(F.iar_reads == 0u && F.eoir_writes == 0u,
          "init must not acknowledge/EOI anything");

    /* Evidence carries the real readbacks. M: evidence from constants. */
    CHECK(ev.gicd_typer == 0x08u && ev.gicd_iidr == 0x43Bu &&
              ev.gicc_iidr == 0x2043Bu,
          "evidence id %x %x %x", ev.gicd_typer, ev.gicd_iidr, ev.gicc_iidr);
    CHECK(ev.gicc_ctlr == 1u && ev.gicc_pmr == 0xFFu && ev.gicd_ctlr == 1u &&
              ev.isenabler0 == (0xFFFFu | TBIT),
          "evidence cfg %x %x %x %x", ev.gicc_ctlr, ev.gicc_pmr, ev.gicd_ctlr,
          ev.isenabler0);
    CHECK(ev.igroupr0 == 0u && ev.ipriority30 == 0x80u,
          "evidence group/prio %x %x", ev.igroupr0, ev.ipriority30);

    /* Evidence group word is the readback, not a constant: another INTID in
     * group 1 is allowed (only bit 30 is refused) and must be reported. */
    fake_reset(); F.igroupr0 = 1u;
    memset(&ev, 0xCC, sizeof ev);
    CHECK(je_gic_init(&ev) == JE_OK && ev.igroupr0 == 1u,
          "evidence igroupr0 must be the real readback (got 0x%x)", ev.igroupr0);
    end_case("igroupr0 bit0 init");

    /* Evidence is optional. */
    fake_reset();
    CHECK(je_gic_init(NULL) == JE_OK, "init(NULL) must succeed");
    CHECK(F.gicc_ctlr == 1u, "init(NULL) must still configure");
    end_case("init(NULL)");
}

/* Run init on a mutated fake; require exactly `want` and a fail-closed stop. */
typedef enum { STOP_BEFORE_ANY_WRITE, STOP_BEFORE_CONFIG, STOP_BEFORE_ENABLE,
               STOP_BEFORE_GICD_ON, STOP_BEFORE_CPUIF, STOP_BEFORE_GICC_EN,
               STOP_AFTER_GICC } stop_point;

static void refuse(const char *what, je_status want, stop_point stop)
{
    je_status st = je_gic_init(NULL);
    CHECK(st != JE_OK, "%s: init must REFUSE, got OK", what);
    CHECK(st == want, "%s: status %s, want %s", what, je_status_name(st),
          je_status_name(want));
    CHECK(F.iar_reads == 0u && F.eoir_writes == 0u, "%s: no IAR/EOIR", what);
    CHECK(F.tval_writes == 0u && (F.cntp_ctl & CTL_ENABLE) == 0u,
          "%s: timer must not be armed", what);
    /* M: refusal check moved after the enables (fail-open). */
    switch (stop) {
    case STOP_BEFORE_ANY_WRITE:
        /* Identification refusals: not a single MMIO write of any width. */
        CHECK(mmio_writes() == 0u, "%s: %u MMIO writes before identification "
              "refusal", what, mmio_writes());
        /* fall through */
    case STOP_BEFORE_CONFIG:
        CHECK(find_ev(EV_W8, GICD_PA + D_IPRIO + TIMER_ID, false, 0) < 0,
              "%s: priority written", what);
        /* fall through */
    case STOP_BEFORE_ENABLE:
        CHECK(!wrote32(GICD_PA + D_ISENABLER0), "%s: enabled INTID 30", what);
        /* fall through */
    case STOP_BEFORE_GICD_ON:
        CHECK(find_ev(EV_W32, GICD_PA + D_CTLR, true, 1u) < 0,
              "%s: distributor enabled", what);
        /* fall through */
    case STOP_BEFORE_CPUIF:
        CHECK(!wrote_gicc(), "%s: CPU interface touched", what);
        /* fall through */
    case STOP_BEFORE_GICC_EN:
        CHECK(!wrote32(GICC_PA + C_CTLR), "%s: CPU interface enabled", what);
        break;
    case STOP_AFTER_GICC:
        break;
    }
    end_case(what);
}

static void test_init_refusals(void)
{
    fake_reset(); F.typer |= TYPER_SECEXT;
    refuse("TYPER.SecurityExtn=1", JE_ERR_DENIED, STOP_BEFORE_ANY_WRITE);
    CHECK(F.gicd_ctlr == 1u, "SecurityExtn: firmware GICD_CTLR must be untouched");

    fake_reset(); F.gicd_iidr = 0u;
    refuse("GICD_IIDR=0", JE_ERR_UNSUPPORTED, STOP_BEFORE_ANY_WRITE);
    fake_reset(); F.gicc_iidr = 0u;   /* M: only GICD_IIDR checked */
    refuse("GICC_IIDR=0", JE_ERR_UNSUPPORTED, STOP_BEFORE_ANY_WRITE);
    fake_reset(); F.gicd_iidr = 0x0001F123u;  /* non-Arm implementer */
    refuse("GICD_IIDR implementer!=0x43b", JE_ERR_UNSUPPORTED,
           STOP_BEFORE_ANY_WRITE);
    fake_reset(); F.gicc_iidr = 0xFFFFFFFFu;  /* bus float */
    refuse("GICC_IIDR=0xffffffff", JE_ERR_UNSUPPORTED, STOP_BEFORE_ANY_WRITE);

    /* GICD_CTLR: the disable readback and the post-enable readback are
     * independent faults (M: either readback deleted). */
    fake_reset(); F.gicd_ctlr_or_off = 1u;    /* disable does not take */
    refuse("GICD_CTLR disable readback", JE_ERR_IO, STOP_BEFORE_CONFIG);
    fake_reset(); F.gicd_ctlr_or_off = 2u;    /* reserved bit reads back */
    refuse("GICD_CTLR disable readback bit1", JE_ERR_IO, STOP_BEFORE_CONFIG);
    fake_reset(); F.gicd_ctlr_or_on = 2u;     /* post-enable reads 0x3 */
    refuse("GICD_CTLR post-enable readback 0x3", JE_ERR_IO, STOP_BEFORE_CPUIF);
    fake_reset(); F.gicd_enable_drop = true;  /* post-enable reads 0x0 */
    refuse("GICD_CTLR post-enable readback 0x0", JE_ERR_IO, STOP_BEFORE_CPUIF);

    fake_reset(); F.igroupr0 = TBIT;  /* INTID 30 in group 1 */
    refuse("IGROUPR0 bit30=1", JE_ERR_INVALID, STOP_BEFORE_CONFIG);

    fake_reset(); F.prio_impl = 0x00u;  /* priority byte does not stick */
    refuse("IPRIORITYR[30] readback", JE_ERR_IO, STOP_BEFORE_ENABLE);
    fake_reset(); F.prio_impl = 0xF0u;  /* 4 priority bits: 0x80 still sticks */
    CHECK(je_gic_init(NULL) == JE_OK, "4-bit priority: 0x80 representable");
    end_case("4-bit priority");

    fake_reset(); F.enable_drop = TBIT;  /* M: no readback of ISENABLER0 */
    refuse("ISENABLER0 bit30 not set", JE_ERR_IO, STOP_BEFORE_GICD_ON);

    fake_reset(); F.pmr_impl = 0xF8u;  /* 5 priority bits: PMR reads 0xf8 */
    refuse("PMR readback 0xf8", JE_ERR_IO, STOP_BEFORE_GICC_EN);

    fake_reset(); F.gicc_ctlr_stuck = CCTLR_FIQEN;  /* M: readback masked to bit0 */
    refuse("GICC_CTLR FIQEn readback", JE_ERR_IO, STOP_AFTER_GICC);
    fake_reset(); F.gicc_ctlr_stuck = CCTLR_EOIMODE;
    refuse("GICC_CTLR EOImode readback", JE_ERR_IO, STOP_AFTER_GICC);
}

/* ---- je_timer_arm ---- */
static void test_timer_arm(void)
{
    uint64_t freq = 0;
    uint32_t interval = 0;

    fake_reset();
    const uint64_t t0 = F.cntpct;
    CHECK(je_timer_arm(&freq, &interval) == JE_OK, "arm 62.5 MHz");
    CHECK(freq == 62500000u && interval == 625000u,
          "freq=%llu interval=%u (want 62500000/625000)",
          (unsigned long long)freq, interval);
    /* M: TVAL after ENABLE; missing isb; IMASK left set; ENABLE never set. */
    static const event want[] = {
        SW(JE_GIC_SYSREG_CNTP_CTL_EL0, CTL_IMASK), ISB,
        SW(JE_GIC_SYSREG_CNTP_TVAL_EL0, 625000u),
        SW(JE_GIC_SYSREG_CNTP_CTL_EL0, CTL_ENABLE), ISB,
    };
    expect_writes(want, sizeof want / sizeof want[0], "timer arm");
    /* Semantic: armed exactly one interval out, not yet fired, unmasked. */
    CHECK(F.cval == t0 + 625000u && !timer_istatus() && !timer_line(),
          "timer arm: cval=0x%llx istatus=%d", (unsigned long long)F.cval,
          (int)timer_istatus());
    F.cntpct = t0 + 624999u;
    CHECK(!timer_line(), "timer arm: fired one tick early");
    F.cntpct = t0 + 625000u;
    CHECK(timer_line(), "timer arm: must fire at the deadline (IMASK=0)");
    end_case("timer arm");

    /* 300 GHz: the pinned contract MASKS CNTFRQ to [31:0], so the outcome is
     * deterministic — accept with freq = low32 and interval = low32/100.
     * M: refusal instead of mask; unmasked 64-bit divide; interval/freq
     * out-params from different values; TVAL != reported interval. */
    je_status st;
    fake_reset(); F.cntfrq = F300G;
    const uint64_t t1 = F.cntpct;
    st = je_timer_arm(&freq, &interval);
    CHECK(st == JE_OK, "300 GHz: status %s, want OK (masked [31:0])",
          je_status_name(st));
    CHECK(freq == F300G_LOW && interval == F300G_IVL && F.tval == F300G_IVL &&
              F.cval == t1 + F300G_IVL && (F.cntp_ctl & CTL_ENABLE) != 0u,
          "300 GHz: freq=%llu interval=%u tval=%llu, want %u/%u",
          (unsigned long long)freq, interval, (unsigned long long)F.tval,
          F300G_LOW, F300G_IVL);
    end_case("300 GHz");

    static const uint64_t bad[] = { 0u, 50u, 99u,
                                    UINT64_C(0xFFFFFFFF00000000) | 99u };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        fake_reset();
        F.cntfrq = bad[i];
        st = je_timer_arm(&freq, &interval);
        CHECK(st == JE_ERR_INVALID, "CNTFRQ=0x%llx: status %s, want INVALID",
              (unsigned long long)bad[i], je_status_name(st));
        /* M: refusal after the arm sequence. */
        CHECK(count_kind(EV_SW) == 0u, "CNTFRQ=0x%llx: no sysreg writes allowed",
              (unsigned long long)bad[i]);
        F.cntpct += UINT64_C(1) << 40;
        CHECK(!timer_line(), "CNTFRQ=0x%llx refused but the timer fires",
              (unsigned long long)bad[i]);
        end_case("CNTFRQ refusal");
    }

    fake_reset(); F.cntfrq = 100u;  /* boundary: smallest accepted */
    CHECK(je_timer_arm(NULL, &interval) == JE_OK && interval == 1u &&
              F.tval == 1u, "CNTFRQ=100 -> interval 1");
    end_case("CNTFRQ=100");
    fake_reset(); F.cntfrq = 199u;
    CHECK(je_timer_arm(NULL, &interval) == JE_OK && interval == 1u,
          "CNTFRQ=199 -> interval 1 (floor)");
    end_case("CNTFRQ=199");

    /* CNTFRQ_EL0[63:32] are RES0 (DDI0487); the pinned contract masks [31:0].
     * M: unmasked 64-bit value silently truncated somewhere else. */
    fake_reset(); F.cntfrq = UINT64_C(0xFFFFFFFF00000000) | 62500000u;
    CHECK(je_timer_arm(&freq, &interval) == JE_OK && freq == 62500000u &&
              interval == 625000u && F.tval == 625000u,
          "upper RES0 bits must be masked: freq=%llu interval=%u",
          (unsigned long long)freq, interval);
    end_case("CNTFRQ upper RES0");

}

/* ---- je_gic_handle_one ---- */
#define REARM_ADVANCE UINT64_C(1000)

static void test_handle_timer(uint32_t iar, uint32_t interval)
{
    fake_reset();
    F.cntpct = UINT64_C(0xABCDEF012345);
    /* Advance strictly less than the interval so the modelled re-arm is still
     * in the future after the step (interval 1 => no advance; the larger
     * intervals carry the sample-after-re-arm detection). */
    F.rearm_advance = interval > REARM_ADVANCE ? REARM_ADVANCE : interval - 1u;
    timer_fired();
    queue_iar(iar);
    const uint64_t pre = F.cntpct;   /* counter value BEFORE the re-arm step */
    const uint64_t t0 = je_arm_timer_ticks(), s0 = je_arm_spurious_count();
    bool sp = true;
    uint64_t pct = 0;
    je_status st = je_gic_handle_one(interval, &sp, &pct);
    CHECK(st == JE_OK, "IAR=0x%x: status %s", iar, je_status_name(st));
    CHECK(!sp, "IAR=0x%x: was_spurious set", iar);
    CHECK(je_arm_timer_ticks() == t0 + 1u, "IAR=0x%x: ticks +%llu, want +1", iar,
          (unsigned long long)(je_arm_timer_ticks() - t0));
    CHECK(je_arm_spurious_count() == s0, "IAR=0x%x: spurious bumped", iar);
    /* (A) The sample is the PRE-re-arm counter: the fake advances CNTPCT by
     * REARM_ADVANCE on the TVAL write. M: sample moved after the re-arm. */
    CHECK(pct == pre, "IAR=0x%x: cntpct sample 0x%llx != pre-re-arm 0x%llx "
          "(sampled after the re-arm?)", iar, (unsigned long long)pct,
          (unsigned long long)pre);
    /* (B) The re-arm is REAL in the model: ISTATUS and the PPI level are
     * cleared, CVAL is exactly one interval past the pre-re-arm counter, and
     * the line re-asserts precisely at that deadline. M: TVAL=0 / negative /
     * wrong register (condition stays met -> level stays high -> re-pend). */
    CHECK(!timer_istatus() && !timer_line(),
          "IAR=0x%x: re-arm did not clear ISTATUS/level (cval=0x%llx pct=0x%llx)",
          iar, (unsigned long long)F.cval, (unsigned long long)F.cntpct);
    CHECK(F.cval == pre + interval, "IAR=0x%x: cval=0x%llx want pre+interval",
          iar, (unsigned long long)F.cval);
    CHECK(F.iar_reads == 1u, "IAR read %u times, want exactly 1", F.iar_reads);
    /* M: EOIR = INTID instead of the full IAR (loses CPUID bits). */
    CHECK(F.eoir_writes == 1u && F.last_eoir == iar,
          "IAR=0x%x: EOIR writes=%u last=0x%x", iar, F.eoir_writes, F.last_eoir);
    CHECK(F.tval_writes == 1u && F.tval == interval, "re-arm TVAL=%llu x%u",
          (unsigned long long)F.tval, F.tval_writes);
    /* Order: IAR read, CNTPCT sample, ISTATUS read, TVAL re-arm, isb, EOIR
     * last. M: EOI before re-arm (level still asserted -> re-pend), ISTATUS
     * read after the re-arm (re-arm clears it), sample after the re-arm. */
    int iar_rd = find_ev(EV_R32, GICC_PA + C_IAR, false, 0);
    int pct_rd = find_ev(EV_SR, JE_GIC_SYSREG_CNTPCT_EL0, false, 0);
    int ctl_rd = find_ev(EV_SR, JE_GIC_SYSREG_CNTP_CTL_EL0, false, 0);
    int tv = find_ev(EV_SW, JE_GIC_SYSREG_CNTP_TVAL_EL0, false, 0);
    int eo = find_ev(EV_W32, GICC_PA + C_EOIR, false, 0);
    CHECK(iar_rd >= 0 && pct_rd > iar_rd && ctl_rd > iar_rd && tv > pct_rd &&
              tv > ctl_rd && eo > tv,
          "order iar=%d cntpct=%d ctl=%d tval=%d eoir=%d", iar_rd, pct_rd,
          ctl_rd, tv, eo);
    CHECK(count_ev(EV_SR, JE_GIC_SYSREG_CNTPCT_EL0) == 1u,
          "CNTPCT must be sampled exactly once (got %u)",
          count_ev(EV_SR, JE_GIC_SYSREG_CNTPCT_EL0));
    bool isb_between = false;
    for (int i = tv + 1; i < eo; ++i) isb_between |= F.log[i].kind == EV_ISB;
    CHECK(isb_between, "isb required between TVAL re-arm and EOIR");
    CHECK(eo == (int)F.n - 1, "EOIR must be the last access");
    CHECK(count_kind(EV_SW) == 1u, "only TVAL may be written (CTL untouched)");
    CHECK(count_kind(EV_W8) == 0u && count_kind(EV_W32) == 1u,
          "exactly one MMIO write (EOIR)");
    end_case("handle timer");
    /* Deadline: the line re-asserts exactly one interval after the sample. */
    F.cntpct = pre + interval - 1u;
    CHECK(!timer_line(), "IAR=0x%x: re-armed deadline too early", iar);
    F.cntpct = pre + interval;
    CHECK(timer_line(), "IAR=0x%x: re-armed deadline not reached on time", iar);

    /* out_cntpct optional */
    fake_reset(); timer_fired(); queue_iar(iar);
    CHECK(je_gic_handle_one(interval, &sp, NULL) == JE_OK && F.eoir_writes == 1u &&
              F.iar_reads == 1u && !timer_line(), "NULL out_cntpct");
    end_case("handle timer NULL out");
}

static void test_handle_spurious(uint32_t iar)
{
    fake_reset();
    timer_fired();   /* line high: a mutant that ignores INTID would re-arm */
    if (iar != SPURIOUS) queue_iar(iar);   /* 1023 also = "nothing queued" */
    const uint64_t t0 = je_arm_timer_ticks(), s0 = je_arm_spurious_count();
    bool sp = false;
    je_status st = je_gic_handle_one(625000u, &sp, NULL);
    CHECK(st == JE_OK, "spurious IAR=0x%x: status %s", iar, je_status_name(st));
    CHECK(sp, "spurious IAR=0x%x: was_spurious not set", iar);
    CHECK(je_arm_timer_ticks() == t0, "spurious IAR=0x%x bumped ticks", iar);
    CHECK(je_arm_spurious_count() == s0 + 1u, "spurious count +%llu",
          (unsigned long long)(je_arm_spurious_count() - s0));
    /* (C) M: spurious path writes EOIR (GICv2 4.4.5: must not) — with ANY
     * width, including a byte-lane write; and no other write at all. */
    CHECK(!wrote_eoir_any_width() && F.eoir_writes == 0u,
          "spurious IAR=0x%x wrote EOIR", iar);
    CHECK(F.iar_reads == 1u, "spurious IAR=0x%x: IAR read %u times, want 1", iar,
          F.iar_reads);
    CHECK(count_kind(EV_SW) == 0u && mmio_writes() == 0u,
          "spurious must write nothing");
    CHECK(timer_line(), "spurious path must not touch the timer");
    end_case("handle spurious");
}

static void test_handle_unexpected(uint32_t iar)
{
    fake_reset();
    timer_fired();
    queue_iar(iar);
    const uint64_t t0 = je_arm_timer_ticks(), s0 = je_arm_spurious_count();
    bool sp = true;
    je_status st = je_gic_handle_one(625000u, &sp, NULL);
    CHECK(st == JE_ERR_INVALID, "IAR=0x%x: status %s, want INVALID", iar,
          je_status_name(st));
    CHECK(!sp, "IAR=0x%x flagged spurious", iar);
    CHECK(je_arm_timer_ticks() == t0 && je_arm_spurious_count() == s0,
          "IAR=0x%x changed counters", iar);
    /* (E) one-shot IAR: exactly one acknowledge. M: double IAR read. */
    CHECK(F.iar_reads == 1u, "IAR=0x%x: IAR read %u times, want 1", iar,
          F.iar_reads);
    /* M: unexpected INTID left active (no EOIR) -> stuck. */
    CHECK(F.eoir_writes == 1u && F.last_eoir == iar,
          "IAR=0x%x: EOIR writes=%u last=0x%x", iar, F.eoir_writes, F.last_eoir);
    CHECK(count_kind(EV_W32) == 1u && count_kind(EV_W8) == 0u,
          "IAR=0x%x: exactly one MMIO write (EOIR)", iar);
    CHECK(F.tval_writes == 0u && count_kind(EV_SW) == 0u && timer_line(),
          "IAR=0x%x touched the timer", iar);
    end_case("handle unexpected");
}

/* ISTATUS==0 with INTID 30 acked: the benign "condition already cleared"
 * re-pend race (lead contract change 2026-11-05: JE_ERR_BUSY, was IO). */
static void test_handle_busy(uint32_t iar, bool timer_enabled)
{
    fake_reset();
    F.rearm_advance = REARM_ADVANCE;
    F.cntp_ctl = timer_enabled ? CTL_ENABLE : 0u;
    F.cval = F.cntpct + UINT64_C(100000);   /* deadline in the future */
    assert(!timer_istatus());
    queue_iar(iar);
    const uint64_t cval0 = F.cval, ctl0 = F.cntp_ctl;
    const uint64_t t0 = je_arm_timer_ticks(), s0 = je_arm_spurious_count();
    bool sp = true;
    je_status st = je_gic_handle_one(625000u, &sp, NULL);
    /* M: no ISTATUS check (ticks bumped on a stale ack); status still IO;
     * EOIR skipped (stuck active); EOIR of the INTID instead of the IAR. */
    CHECK(st == JE_ERR_BUSY, "ISTATUS=0 IAR=0x%x: status %s, want BUSY", iar,
          je_status_name(st));
    CHECK(!sp, "ISTATUS=0 flagged spurious");
    CHECK(je_arm_timer_ticks() == t0, "ISTATUS=0 bumped ticks");
    CHECK(je_arm_spurious_count() == s0, "ISTATUS=0 bumped spurious count");
    CHECK(F.iar_reads == 1u, "ISTATUS=0: IAR read %u times, want 1", F.iar_reads);
    CHECK(F.eoir_writes == 1u && F.last_eoir == iar,
          "ISTATUS=0 must EOIR the exact IAR: writes=%u last=0x%x", F.eoir_writes,
          F.last_eoir);
    CHECK(count_kind(EV_W32) == 1u && count_kind(EV_W8) == 0u,
          "ISTATUS=0: exactly one MMIO write (EOIR)");
    CHECK(F.tval_writes == 0u && count_kind(EV_SW) == 0u && F.cval == cval0 &&
              F.cntp_ctl == ctl0, "ISTATUS=0 re-armed / touched the timer");
    end_case("handle busy");
}

static void test_handle_one(void)
{
    test_handle_timer(TIMER_ID, 625000u);
    test_handle_timer((1u << 10) | TIMER_ID, 625000u);   /* CPUID field set */
    test_handle_timer((7u << 10) | TIMER_ID, 625000u);
    test_handle_timer(TIMER_ID, 1u);                     /* smallest interval */
    test_handle_timer(TIMER_ID, (uint32_t)INT32_MAX);    /* largest accepted */
    static const uint32_t spur[] = { 1020u, 1021u, 1022u, 1023u, (3u << 10) | 1023u };
    for (unsigned i = 0; i < sizeof spur / sizeof spur[0]; ++i)
        test_handle_spurious(spur[i]);
    static const uint32_t other[] = { 0u, 29u, 31u, 42u, 1019u, (1u << 10) | 42u };
    for (unsigned i = 0; i < sizeof other / sizeof other[0]; ++i)
        test_handle_unexpected(other[i]);
    test_handle_busy(TIMER_ID, true);
    test_handle_busy((2u << 10) | TIMER_ID, true);
    test_handle_busy(TIMER_ID, false);   /* timer disabled: ISTATUS reads 0 */

    /* Bad arguments: must not ACK (an IAR read without EOIR leaves it active).
     * M: interval range checked after the IAR read; only ==0 rejected. */
    bool sp = true;
    static const uint32_t bad_iv[] = { 0u, (uint32_t)INT32_MAX + 1u, UINT32_MAX };
    for (unsigned i = 0; i < sizeof bad_iv / sizeof bad_iv[0]; ++i) {
        fake_reset(); timer_fired(); queue_iar(TIMER_ID);
        je_status st = je_gic_handle_one(bad_iv[i], &sp, NULL);
        CHECK(st == JE_ERR_INVALID && F.iar_reads == 0u && F.n == 0u,
              "interval 0x%x: status %s reads=%u n=%u, want INVALID before any "
              "access", bad_iv[i], je_status_name(st), F.iar_reads, F.n);
        end_case("handle bad interval");
    }
    fake_reset(); timer_fired(); queue_iar(TIMER_ID);
    CHECK(je_gic_handle_one(625000u, NULL, NULL) == JE_ERR_INVALID &&
              F.iar_reads == 0u && F.n == 0u, "was_spurious NULL: refuse before IAR");
    end_case("handle NULL was_spurious");
}

/* ---- je_timer_poll_pending ---- */
static void test_poll(void)
{
    fake_reset(); F.ispendr0 = TBIT;
    CHECK(je_timer_poll_pending(1000u), "ISPENDR0 bit30 -> pending");
    end_case("poll ispendr");
    fake_reset(); F.hppir = TIMER_ID;
    CHECK(je_timer_poll_pending(1000u), "HPPIR=30 -> pending");
    end_case("poll hppir");
    fake_reset(); F.hppir = (2u << 10) | TIMER_ID;
    CHECK(je_timer_poll_pending(1000u), "HPPIR CPUID|30 -> pending");
    end_case("poll hppir cpuid");

    /* Becomes pending after a few polls, within the deadline. */
    fake_reset(); F.ispendr0 = TBIT; F.ispendr_after = 5u; F.cntpct_step = 10u;
    CHECK(je_timer_poll_pending(1000u) && F.ispendr_reads == 6u,
          "late pending: reads=%u", F.ispendr_reads);
    end_case("poll late");

    /* End to end over the timer MODEL: arm (interval 100 at 10 kHz), then the
     * modelled PPI level appears once CNTPCT passes CVAL. */
    fake_reset(); F.cntfrq = 10000u; F.cntpct_step = 10u;
    uint32_t interval = 0;
    CHECK(je_timer_arm(NULL, &interval) == JE_OK && interval == 100u, "arm 10 kHz");
    CHECK(je_timer_poll_pending(1000u) && F.ispendr_reads > 1u && F.cntpct >= F.cval,
          "model: armed timer must become pending at its deadline (reads=%u)",
          F.ispendr_reads);
    end_case("poll model");
    /* Armed but IMASK=1: the level never reaches the GIC -> deadline false. */
    fake_reset(); F.cntpct_step = 100u;
    F.cntp_ctl = CTL_ENABLE | CTL_IMASK; F.cval = F.cntpct;
    CHECK(timer_istatus() && !je_timer_poll_pending(5000u),
          "model: IMASK=1 must not be reported pending");
    end_case("poll imask");

    /* Never pending (other bits only): must return false, bounded by CNTPCT.
     * M: unbounded loop (would hang -> the build's timeout), wrong bit. */
    fake_reset(); F.ispendr0 = ~TBIT; F.hppir = 29u; F.cntpct_step = 100u;
    const uint64_t start = F.cntpct;
    CHECK(!je_timer_poll_pending(10000u), "not pending must time out false");
    CHECK(F.cntpct - start > 10000u && F.cntpct_reads < 200u,
          "deadline: elapsed=%llu reads=%u", (unsigned long long)(F.cntpct - start),
          F.cntpct_reads);
    CHECK(F.iar_reads == 0u && count_kind(EV_W32) == 0u && count_kind(EV_SW) == 0u &&
              count_kind(EV_W8) == 0u,
          "poll must be read-only (no ACK/EOI/arm)");
    end_case("poll deadline");
}

int main(void)
{
    test_fake_selfcheck();
    test_init_good();
    test_init_refusals();
    test_timer_arm();
    test_handle_one();
    test_poll();
    printf("gic_bits: PASS (%u checks: fake self-check, init sequence/identify-"
           "before-write/state/evidence, 15 init refusals fail-closed, timer arm "
           "order + modelled deadline + CNTFRQ bounds/mask, handle_one "
           "timer/spurious/unexpected/busy with one-shot IAR + modelled re-arm, "
           "bounded poll; violations==0 every case)\n", checks);
    return 0;
}
