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
#define JE_IMAGE_BROKER_SVC  7u   /* brokered service (PR-G) */
#define JE_IMAGE_BROKER_CLI  8u   /* broker client (PR-G) */

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
 * v0 broker protocol (ADR 0010 D11 + the 2026-11-01 protocol note). All ring-3
 * supervisor policy; the kernel gains no broker status or identity. A cap send
 * COPIES a descriptor and the receiver mints on recv (ADR 0009 A5), so every
 * cap-bearing grant is a TWO-PHASE transaction acknowledged before the supervisor
 * closes its source end. Fixed-size, little-endian, op first so a receiver can
 * demux before trusting the rest. Brokered ends carry exactly SEND|RECV (no
 * TRANSFER/INSPECT); the supervisor receives requests with hcap=0, and the client
 * and service receive cap-bearing grants with hcap=1.
 */
#define JE_SVC_OP_BROKER_REQUEST   0x51455242u /* "BREQ": client -> supervisor */
#define JE_SVC_OP_BROKER_GRANT     0x544e5247u /* "GRNT": sup -> client, +1 cap */
#define JE_SVC_OP_BROKER_RESTARTED 0x54535352u /* "RSST": sup -> client, +1 cap */
#define JE_SVC_OP_BROKER_UNAVAIL   0x5641564eu /* "NVAV": sup -> client, no cap, no cap-charge */
#define JE_SVC_OP_BROKER_EXHAUSTED 0x48535845u /* "EXSH": sup -> client, no cap, cap spent */
#define JE_SVC_OP_BROKER_ALREADY   0x44524c41u /* "ALRD": dup for a live generation */
#define JE_SVC_OP_SERVICE_BIND     0x444e4942u /* "BIND": sup -> service, +1 cap */
#define JE_SVC_OP_BROKER_ACK       0x4b434142u /* "BACK": recipient -> sup, cap minted */

/* BROKER_REQUEST (client -> supervisor), no caps (received hcap=0). */
typedef struct {
    uint32_t op;          /* JE_SVC_OP_BROKER_REQUEST */
    uint32_t service_id;  /* bounds-checked against the brokerable-service table */
} je_svc_broker_request;
#define JE_SVC_BROKER_REQUEST_LEN 8u
_Static_assert(sizeof(je_svc_broker_request) == JE_SVC_BROKER_REQUEST_LEN,
               "BROKER_REQUEST size");

/* GRANT / RESTARTED (supervisor -> client), each carrying exactly ONE cap (the
 * client's SEND|RECV end). service_generation is a supervisor-minted monotonic
 * u32; the client treats it as metadata only (it never authenticates the service).
 * RESTARTED uses the same layout with the RESTARTED op. */
typedef struct {
    uint32_t op;                 /* GRANT or RESTARTED */
    uint32_t service_generation; /* supervisor per-service gen, monotonic */
} je_svc_broker_grant;
#define JE_SVC_BROKER_GRANT_LEN 8u
_Static_assert(sizeof(je_svc_broker_grant) == JE_SVC_BROKER_GRANT_LEN,
               "BROKER_GRANT size");

/* SERVICE_BIND (supervisor -> service), carrying ONE cap (the service's SEND|RECV
 * end) and the generation the service must associate with it. */
typedef struct {
    uint32_t op;                 /* JE_SVC_OP_SERVICE_BIND */
    uint32_t service_generation; /* authoritative; the service trusts ONLY this */
} je_svc_service_bind;
#define JE_SVC_SERVICE_BIND_LEN 8u
_Static_assert(sizeof(je_svc_service_bind) == JE_SVC_SERVICE_BIND_LEN,
               "SERVICE_BIND size");

/* UNAVAILABLE / EXHAUSTED / ALREADY_GRANTED / ACK: op-only replies (no cap).
 * UNAVAILABLE carries a diagnostic reason; it does NOT consume the client's
 * lifetime cap. EXHAUSTED means the lifetime cap is spent. ACK is the recipient's
 * confirmation that it minted the cap (two-phase transaction). */
typedef struct {
    uint32_t op;      /* one of the no-cap broker ops above */
    uint32_t detail;  /* UNAVAILABLE: a reason; others: 0 */
} je_svc_broker_reply;
#define JE_SVC_BROKER_REPLY_LEN 8u
_Static_assert(sizeof(je_svc_broker_reply) == JE_SVC_BROKER_REPLY_LEN,
               "BROKER_REPLY size");

/* UNAVAILABLE reasons (diagnostic only). */
#define JE_SVC_UNAVAIL_NOT_READY  1u /* service starting/backoff/not yet READY */
#define JE_SVC_UNAVAIL_TERMINAL   2u /* service failed-terminal: no instance */
#define JE_SVC_UNAVAIL_BAD_SERVICE 3u /* service_id out of range / not brokerable */
#define JE_SVC_UNAVAIL_UNAUTH     4u /* client not authorized for this service */


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
#define JE_SVC_DONE_EXIT          0x4d3446u /* nonzero clean supervisor exit */

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

/* PR-F bounded scenario policy. These values are deliberately small enough to
 * fit the static VM/thread/process pools while still exercising one recovery
 * and one terminal crash-loop path. */
#define JE_SVC_RESTART_LIMIT 1u
#define JE_SVC_RESTART_CRASH_BEFORE 1u
#define JE_SVC_CRASHLOOP_RESTART_LIMIT 2u
#define JE_SVC_READY_DEADLINE 400u
#define JE_SVC_SCENARIO_DEADLINE 2000u
#define JE_SVC_MONITOR_DEADLINE  12000u
#define JE_SVC_BACKOFF_TICKS 1u
#define JE_SVC_LOSS_GO 0xD1200001u
#define JE_SVC_SPOOF_MAGIC 0x53504652u
#define JE_SVC_SPOOF_EXPECTED_MASK 0x0007u

/* PR-G broker scenario policy (TEST_MODE 13). One brokered service (restart_limit
 * 1) and two clients: one reconnects through a service restart and then exhausts
 * its lifetime cap; the other obtains a single grant and keeps it. The per-(client,
 * service) lifetime cap is restart_limit+1 = 2; the supervisor's live-channel
 * ledger (the true bound, ADR 0010 D2 note) refuses beyond JE_BROKER_MAX. */
#define JE_SVC_BROKER_SERVICE_ID   0u            /* the single brokerable service */
#define JE_SVC_BROKER_SERVICE_COUNT 1u           /* brokerable-service table size */
#define JE_SVC_BROKER_CLIENT_CAP   (JE_SVC_RESTART_LIMIT + 1u) /* per client life */
#define JE_SVC_BROKER_ACK_DEADLINE 400u          /* ticks to await a mint ACK */
#define JE_SVC_DONE_EXIT_G 0x6734446Au           /* mode-13 supervisor clean EXIT code */
_Static_assert(JE_SVC_BROKER_CLIENT_CAP <= JE_BROKER_MAX,
               "a single client cannot exceed the live broker-channel bound");

_Static_assert(JE_SVC_EP_LIMIT >= (4u * 3u) + 2u + 1u,
               "supervisor endpoint budget covers channels and sleep pair");

#endif /* JEIOS_SVC_H */
