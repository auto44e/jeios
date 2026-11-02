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
#define JE_IMAGE_UART_DRIVER 9u   /* D9 ring-3 COM2 driver (PR-H mode-14 harness) */
#define JE_IMAGE_UART_SVC    10u  /* D9 brokered UART service (PR-I) */
#define JE_IMAGE_UART_CLIENT 11u  /* D9 UART client (PR-I) */

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
#define JE_SVC_OP_BROKER_HARDENED  0x48455242u /* "BREH": client -> sup, hardened (D9 PR-I) */
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
    uint32_t transaction;        /* supervisor-minted two-phase transaction id */
} je_svc_broker_grant;
#define JE_SVC_BROKER_GRANT_LEN 12u
_Static_assert(sizeof(je_svc_broker_grant) == JE_SVC_BROKER_GRANT_LEN,
               "BROKER_GRANT size");

/* SERVICE_BIND (supervisor -> service), carrying ONE cap (the service's SEND|RECV
 * end) and the generation the service must associate with it. */
typedef struct {
    uint32_t op;                 /* JE_SVC_OP_SERVICE_BIND */
    uint32_t service_generation; /* authoritative; the service trusts ONLY this */
    uint32_t transaction;        /* supervisor-minted two-phase transaction id */
} je_svc_service_bind;
#define JE_SVC_SERVICE_BIND_LEN 12u
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
#define JE_SVC_UNAVAIL_HARDENED   5u /* hardened/IOMMU isolation requested; D9 refuses
                                      * UNSUPPORTED "legacy no-DMA PIO" (PR-I) */

/*
 * v0 UART service protocol (ADR 0010 D9 / PR-I). Two channel kinds:
 *  - CONTROL (supervisor <-> driver, bidirectional SEND|RECV): device attach and
 *    health. ATTACH carries the DEVICE handle value as plain data (the driver
 *    trusts it only from CONTROL). The driver replies ATTACH_OK{PRESENT|DEGRADED}
 *    once it has probed COM2; the supervisor treats the service as brokerable only
 *    after ATTACH_OK PRESENT. HEALTH_PING{seq}/PONG{seq} drive the D9 timeout:
 *    the supervisor requires a PONG within JE_SVC_UART_DEADLINE ticks, else kills
 *    and restarts. DETACHED announces a voluntary device close.
 *  - BROKERED (client <-> driver, SEND|RECV no TRANSFER): WRITE/READ requests.
 * All fixed-size, little-endian, op first, request-id (seq) for reply matching.
 */
#define JE_SVC_OP_UART_ATTACH    0x48545541u /* "AUTH"~: sup -> driver, +DEVICE handle */
#define JE_SVC_OP_UART_ATTACH_OK 0x4b4f5541u /* "AUOK": driver -> sup, probe result */
#define JE_SVC_OP_UART_PING      0x474e5055u /* "UPNG": sup -> driver */
#define JE_SVC_OP_UART_PONG      0x474e4f55u /* "UONG": driver -> sup */
#define JE_SVC_OP_UART_DETACHED  0x48544455u /* "UDTH": driver -> sup, voluntary detach */
#define JE_SVC_OP_UART_WRITE     0x54525755u /* "UWRT": client -> driver */
#define JE_SVC_OP_UART_READ      0x44525255u /* "URRD": client -> driver */
#define JE_SVC_OP_UART_REPLY     0x594c5055u /* "UPLY": driver -> client */

#define JE_SVC_UART_PRESENT  2u   /* COM2 probed and present */
#define JE_SVC_UART_DEGRADED 1u   /* COM2 read 0xFF: no device; no grants */

/* CONTROL: ATTACH (supervisor -> driver), DEVICE handle value as data. */
typedef struct { uint32_t op; uint32_t device_handle; } je_svc_uart_attach;
#define JE_SVC_UART_ATTACH_LEN 8u
_Static_assert(sizeof(je_svc_uart_attach) == JE_SVC_UART_ATTACH_LEN, "UART ATTACH size");

/* CONTROL: ATTACH_OK / PING / PONG / DETACHED all share this 8-byte shape. For
 * ATTACH_OK `arg` = PRESENT/DEGRADED; for PING/PONG `arg` = seq; else 0. */
typedef struct { uint32_t op; uint32_t arg; } je_svc_uart_control;
#define JE_SVC_UART_CONTROL_LEN 8u
_Static_assert(sizeof(je_svc_uart_control) == JE_SVC_UART_CONTROL_LEN, "UART control size");

/* BROKERED: WRITE request (client -> driver). len <= JE_SVC_UART_PAYLOAD_MAX. */
#define JE_SVC_UART_PAYLOAD_MAX 48u
typedef struct {
    uint32_t op;    /* UART_WRITE or UART_READ */
    uint32_t seq;   /* request id, echoed in the reply */
    uint32_t len;   /* WRITE: bytes to send; READ: max bytes requested */
    uint32_t reserved;
    uint8_t  bytes[JE_SVC_UART_PAYLOAD_MAX];
} je_svc_uart_request;
#define JE_SVC_UART_REQUEST_LEN 64u
_Static_assert(sizeof(je_svc_uart_request) == JE_SVC_UART_REQUEST_LEN, "UART request size");

/* BROKERED: reply (driver -> client). status is a JE_SVC_UART_IO_* value. */
typedef struct {
    uint32_t op;     /* JE_SVC_OP_UART_REPLY */
    uint32_t seq;    /* matches the request */
    uint32_t status; /* JE_SVC_UART_IO_* */
    uint32_t len;    /* bytes written (WRITE) or returned (READ) */
    uint8_t  bytes[JE_SVC_UART_PAYLOAD_MAX];
} je_svc_uart_reply;
#define JE_SVC_UART_REPLY_LEN 64u
_Static_assert(sizeof(je_svc_uart_reply) == JE_SVC_UART_REPLY_LEN, "UART reply size");

#define JE_SVC_UART_IO_OK        0u
#define JE_SVC_UART_IO_EMPTY     1u  /* READ: no RX byte available (non-blocking) */
#define JE_SVC_UART_IO_WOULDBLOCK 2u /* WRITE: THRE not ready within the bounded poll */
#define JE_SVC_UART_IO_INVALID   3u  /* malformed request */
#define JE_SVC_UART_IO_DETACHED  4u  /* driver has no device authority */


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
#define JE_SVC_FAIL_UART_DEADLINE 7u  /* PR-I UART scenario deadline expired */
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

/* PR-I UART service policy (TEST_MODE 15). A separate restart limit so the UART
 * timeout + revoke demos can each restart without touching mode-13's shared
 * JE_SVC_RESTART_LIMIT (ADR D11: "each service has restart_limit"). The UART
 * client's lifetime broker cap is limit+1 so it can reconnect across both. */
#define JE_SVC_UART_RESTART_LIMIT 2u
#define JE_SVC_UART_DEADLINE 600u    /* ticks the supervisor waits for a PONG */
#define JE_SVC_UART_CLIENT_CAP (JE_SVC_UART_RESTART_LIMIT + 1u)
#define JE_SVC_UART_DONE_EXIT 0x67344475u /* mode-15 supervisor clean EXIT code */
_Static_assert(JE_SVC_UART_CLIENT_CAP <= JE_BROKER_MAX,
               "UART client cap within the live broker-channel bound");

/* PR-G broker scenario policy (TEST_MODE 13). One brokered service (restart_limit
 * 1) and two clients: one reconnects through a service restart and then exhausts
 * its lifetime cap; the other obtains a single grant and keeps it. The per-(client,
 * service) lifetime cap is restart_limit+1 = 2; the supervisor's live-channel
 * ledger (the true bound, ADR 0010 D2 note) refuses beyond JE_BROKER_MAX. */
#define JE_SVC_BROKER_SERVICE_ID   0u            /* the single brokerable service */
#define JE_SVC_BROKER_SERVICE_COUNT 1u           /* brokerable-service table size */
#define JE_SVC_BROKER_CLIENT_CAP   (JE_SVC_RESTART_LIMIT + 1u) /* per client life */
#define JE_SVC_BROKER_ACK_DEADLINE 400u          /* ticks to await a mint ACK */
#define JE_SVC_BROKER_CLIENT_WAIT_DEADLINE \
    (2u * JE_SVC_BROKER_ACK_DEADLINE)             /* client-side grant wait */
/* Flex (revisit): a future PROC_STATUS could additionally return the record
 * identity (index,gen) in RDX as an additive, v0 extension, giving ring-3 a
 * stable process-identity primitive instead of relying on handle-value
 * stability. Deferred 2026-11-01 (owner): PR-G keys its broker ledger on the
 * supervisor-owned PROCESS handle instead, so no ABI change is needed now.
 * Revisit if another component needs process identity at ring 3. */
#define JE_SVC_DONE_EXIT_G 0x6734446Au           /* mode-13 supervisor clean EXIT code */
_Static_assert(JE_SVC_BROKER_CLIENT_CAP <= JE_BROKER_MAX,
               "a single client cannot exceed the live broker-channel bound");

_Static_assert(JE_SVC_EP_LIMIT >= (4u * 3u) + 2u + 1u,
               "supervisor endpoint budget covers channels and sleep pair");

#endif /* JEIOS_SVC_H */
