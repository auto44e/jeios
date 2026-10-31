/*
 * ADR 0010 D7/D11: service-protocol constants shared by the kernel fixtures, the
 * supervisor and ring-3 services. Ring-3 safe: includes only <stdint.h>. This is a v0
 * protocol, explicitly NOT a stable ABI (ADR 0010 D11, INIT.md:131).
 *
 * READY (D7): the FIRST message a service sends on its readiness channel. Exactly
 * JE_SVC_READY_LEN bytes, no capabilities:
 *   u32 op      = JE_SVC_OP_READY
 *   u32 version = JE_SVC_PROTO_V0
 * A different first message, wrong length or attached caps is a failed start.
 */
#ifndef JEIOS_SVC_H
#define JEIOS_SVC_H

#include <stdint.h>

#define JE_SVC_PROTO_V0   0u
#define JE_SVC_OP_READY   0x59444552u   /* "REDY" little-endian */
#define JE_SVC_READY_LEN  8u

typedef struct {
    uint32_t op;
    uint32_t version;
} je_svc_ready;
_Static_assert(sizeof(je_svc_ready) == JE_SVC_READY_LEN, "READY message size");

/* ADR 0010 D2 broker bound, shared by endpoint.h (pool sizing) and the supervisor
 * (moved here from endpoint.h in PR-F so both see one definition). */
#define JE_BROKER_MAX        6u
_Static_assert(JE_BROKER_MAX <= 6u, "ADR 0010 D11 broker bound");

/* Built-in image ids (ADR 0010 D5/D10). Dense; ids are the PROC_SPAWN image_id. */
#define JE_IMAGE_SPAWN_CHILD 0u   /* M4c asm child (PR-C) */
#define JE_IMAGE_C_HELLO     1u   /* first ring-3 C image (PR-E) */
#define JE_IMAGE_SUPERVISOR  2u   /* the supervisor (PR-F) */
#define JE_IMAGE_RESTART_SVC 3u   /* restart-service child (PR-F) */
#define JE_IMAGE_SURVIVOR    4u   /* surviving peer child (PR-F) */
#define JE_IMAGE_SPOOFER     5u   /* confused-deputy probe child (PR-F) */
#define JE_IMAGE_CRASHLOOP   6u   /* crash-to-terminal child (PR-F) */

/*
 * v0 service messages (ADR 0010 D7/D11). NOT a stable ABI. All little-endian,
 * fixed-size, no capabilities unless stated. op is the first u32 of every message
 * so a receiver can demux before trusting the rest.
 */
#define JE_SVC_OP_HEARTBEAT 0x54524248u   /* "HRBT": survivor liveness ping */
#define JE_SVC_OP_CONFIG    0x46474643u   /* "CFGF": supervisor -> child control */

/* HEARTBEAT (survivor -> supervisor), JE_SVC_HEARTBEAT_LEN bytes, no caps. */
typedef struct {
    uint32_t op;        /* JE_SVC_OP_HEARTBEAT */
    uint32_t seq;       /* monotonic per incarnation; wraps are fine */
} je_svc_heartbeat;
#define JE_SVC_HEARTBEAT_LEN 8u
_Static_assert(sizeof(je_svc_heartbeat) == JE_SVC_HEARTBEAT_LEN, "HEARTBEAT size");

/* CONFIG (supervisor -> restart-service), JE_SVC_CONFIG_LEN bytes, no caps.
 * Tells the child which incarnation it is and when (if ever) to fault. */
typedef struct {
    uint32_t op;           /* JE_SVC_OP_CONFIG */
    uint32_t incarnation;  /* 0-based launch count for this child */
    uint32_t crash_before; /* fault if incarnation < crash_before; else stay up */
    uint32_t reserved;     /* must be 0 */
} je_svc_config;
#define JE_SVC_CONFIG_LEN 16u
_Static_assert(sizeof(je_svc_config) == JE_SVC_CONFIG_LEN, "CONFIG size");

/*
 * The supervisor report (ADR 0010 D12). There is no console syscall, so the
 * supervisor publishes progress into a fixed, kernel-known struct in its own RW
 * data; the kernel reads it (CR3 switch, IF=0) through the fault-recovering
 * usercopy and prints the M4f markers. EVERYTHING here is UNTRUSTED: the kernel
 * validates magic/version/phase/seq and bounds, never uses a field as a pointer,
 * length, index or authority, and the D12 LOSS decision never depends on it.
 *
 * Placement contract (enforced by the kernel's image re-validation for image id
 * JE_IMAGE_SUPERVISOR): the struct lies at JE_SVC_REPORT_VADDR, inside a
 * read-write LOAD segment, below the guard page (0x401fb000), 8-byte aligned,
 * and does not straddle a page boundary. The supervisor links it into a
 * dedicated .je_report section at that vaddr.
 *
 * Torn-read protection: `seq` is odd while the supervisor is mutating the struct
 * and even when it is consistent; the kernel reads seq, then the body, then seq
 * again, and accepts only a stable even value. Single CPU + `mov cr3`
 * serialization means no hardware barrier is needed, but the supervisor MUST use
 * volatile stores and a compiler barrier so `seq`/`phase` are not reordered.
 * (A future SMP port would need release/acquire here.)
 */
#define JE_SVC_REPORT_MAGIC 0x4634464Au   /* "JF4F" */
#define JE_SVC_REPORT_V0    0u
#define JE_SVC_REPORT_VADDR 0x40010000ull /* RW data page, < guard 0x401fb000 */

/* phase: written last in each consistent update; the kernel's success path keys
 * on DONE. Monotonic within one supervisor run. */
#define JE_SVC_PHASE_BOOT    0u   /* seeded, nothing proven yet */
#define JE_SVC_PHASE_READY   1u   /* all children launched, readiness collected */
#define JE_SVC_PHASE_RUNNING 2u   /* steady-state monitoring */
#define JE_SVC_PHASE_ARM     3u   /* failed-launch rollback handshake armed */
#define JE_SVC_PHASE_DONE    4u   /* all scenarios passed; a clean EXIT follows */
#define JE_SVC_PHASE_FAILED  5u   /* a scenario failed; fail_code is set */

typedef struct {
    uint32_t magic;        /* JE_SVC_REPORT_MAGIC */
    uint32_t version;      /* JE_SVC_REPORT_V0 */
    uint32_t seq;          /* odd while writing, even when consistent */
    uint32_t phase;        /* one of JE_SVC_PHASE_*; written last */
    uint32_t fail_code;    /* 0 unless phase==FAILED; a JE_SVC_FAIL_* value */
    uint32_t restarts;     /* observed restart-service restarts */
    uint32_t heartbeats;   /* survivor heartbeats seen across the crash window */
    uint32_t spoofer_bits; /* spoofer self-check bitmask (evidence, not proof) */
    uint32_t crash_attempts; /* crash-loop launch attempts observed */
    uint32_t reserved;     /* must be 0 */
} je_svc_report;
#define JE_SVC_REPORT_LEN 40u
_Static_assert(sizeof(je_svc_report) == JE_SVC_REPORT_LEN, "report struct size");

/* fail_code values (diagnostic only; printed as a number). */
#define JE_SVC_FAIL_NONE          0u
#define JE_SVC_FAIL_READINESS     1u  /* a child missed its readiness deadline */
#define JE_SVC_FAIL_RESTART       2u  /* restart-service never came back up */
#define JE_SVC_FAIL_SURVIVOR      3u  /* survivor died during the crash window */
#define JE_SVC_FAIL_SLEEP_CHANNEL 4u  /* non-TIMED_OUT on the private sleep end */
#define JE_SVC_FAIL_ROLLBACK      5u  /* failed-launch rollback did not hold */
#define JE_SVC_FAIL_CRASHLOOP     6u  /* crash-loop attempt count was wrong */

/*
 * Failed-launch rollback handshake (ADR 0010 D5 evidence, driven by the kernel).
 * The supervisor sets phase=ARM and spins reading a kernel-written go word at
 * JE_SVC_ROLLBACK_GO_VADDR; the kernel arms je_spawn_failpoint_set and writes
 * JE_SVC_ROLLBACK_GO. The supervisor then does EXACTLY ONE spawn, which must
 * return H_NOSLOTS with handle 0, and records the outcome. The go word shares the
 * report page; same untrusted/validated contract.
 */
#define JE_SVC_ROLLBACK_GO_VADDR 0x40010040ull /* same RW page as the report */
#define JE_SVC_ROLLBACK_GO       0xA5A5A5A5u

/* Supervisor endpoint-creation quota (ADR 0010 D2 formula). Asserted in the
 * supervisor against JE_EP_CHILD_QUOTA_MAX-independent need; see PR-F IMPL. */
#define JE_SVC_EP_LIMIT 21u

#endif /* JEIOS_SVC_H */
