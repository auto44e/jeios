/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/user.h>
#include <jeios/svc.h>
#include <jeios/broker_test.h>
#include <jeios/uart_test.h>

static const char supervisor_rodata[] __attribute__((used, section(".rodata")))
    = "JeiOS supervisor";
static volatile je_svc_report report
    __attribute__((section(".je_report.report"), aligned(8)));
static volatile uint8_t report_padding[24u]
    __attribute__((section(".je_report.padding"), aligned(1)));
static volatile uint32_t rollback_go
    __attribute__((section(".je_report.go"), aligned(4)));

static void compiler_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

typedef struct {
    uint32_t restarts;
    uint32_t heartbeats;
    uint32_t spoofer_bits;
    uint32_t crash_attempts;
} report_state;

static void publish(uint32_t phase, uint32_t fail_code,
                    const report_state *state)
{
    if (!state) return;
    uint32_t seq = report.seq;
    if ((seq & 1u) != 0u) ++seq;
    report.seq = seq + 1u;
    report.magic = JE_SVC_REPORT_MAGIC;
    report.version = JE_SVC_REPORT_V0;
    report.fail_code = fail_code;
    report.restarts = state->restarts;
    report.heartbeats = state->heartbeats;
    report.spoofer_bits = state->spoofer_bits;
    report.crash_attempts = state->crash_attempts;
    report.reserved = 0u;
    compiler_barrier();
    report.phase = phase;
    compiler_barrier();
    report.seq = seq + 2u;
}

static uint64_t sleep_tick(uint64_t self_recv)
{
    uint64_t marker = 0u;
    uint64_t meta = 0u;
    uint64_t status = je_try_recv(self_recv, &marker, sizeof marker, 1u, &meta);
    return status;
}

static uint64_t status_of(uint64_t process, je_abi_proc_status *out)
{
    return je_proc_status(process, out);
}

static uint32_t ready_message(uint64_t endpoint)
{
    je_svc_ready ready = {0};
    uint64_t meta = 0u;
    uint64_t status = je_try_recv(endpoint, &ready, sizeof ready, 0u, &meta);
    if (status == JE_ABI_H_EMPTY) return 0u;
    if (status != JE_ABI_OK
        || (meta & UINT64_C(0xffffffff)) != JE_SVC_READY_LEN
        || (meta >> 32) != 0u
        || ready.op != JE_SVC_OP_READY || ready.version != JE_SVC_PROTO_V0)
        return UINT32_MAX;
    return 1u;
}

static uint32_t spawn_one(uint64_t launcher, uint32_t image, uint64_t source,
                          uint32_t rights, uint64_t *process, uint64_t *peer)
{
    uint64_t packed = 0u;
    je_abi_spawn_grant grant = {0};
    uint64_t status;
    if (je_endpoint_create(&packed) != JE_ABI_OK) return 0u;
    uint64_t child_end = (uint32_t)packed;
    *peer = packed >> 32;
    grant.handle = (uint32_t)child_end;
    grant.rights = rights;
    status = je_proc_spawn(launcher, image, &grant, 1u,
                           JE_ABI_SPAWN_QUOTAS(15u, 0u), process);
    if (status != JE_ABI_OK) {
        (void)je_handle_close(child_end);
        (void)je_handle_close(*peer);
        *peer = 0u;
        return 0u;
    }
    (void)je_handle_close(child_end);
    (void)source;
    return 1u;
}

#if JE_TEST_MODE == 14
static uint64_t sleep_tick(uint64_t self_recv);
static uint32_t wait_ready(uint64_t endpoint, uint64_t process,
                           uint64_t self_recv, uint64_t deadline);
static uint32_t wait_dead(uint64_t process, uint64_t self_recv,
                          uint64_t deadline);
static int64_t failed(report_state *report_values, uint32_t code);
typedef struct {
    uint32_t op;
    uint32_t value;
} d9_message;
#define D9_DEGRADED 1u
#define D9_PRESENT 2u

static uint32_t d9_wait_result(uint64_t endpoint, uint64_t process,
                               uint64_t self_recv)
{
    d9_message result = {0};
    uint64_t meta = 0u;
    uint64_t start = 0u, now = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        uint64_t status = je_try_recv(endpoint, &result, sizeof result, 0u, &meta);
        if (status == JE_ABI_OK) {
            if ((meta & UINT64_C(0xffffffff)) != sizeof result
                || (meta >> 32) != 0u || result.op != UINT32_C(0x54534552))
                return 0u;
            return result.value;
        }
        if (status != JE_ABI_H_EMPTY) return 0u;
        (void)process;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_READY_DEADLINE)
            return 0u;
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT) return 0u;
    }
}

static uint32_t d9_run_one(uint64_t launcher, uint64_t admin,
                           uint64_t self_recv, uint32_t mode)
{
    uint64_t process = 0u, endpoint = 0u, device = 0u;
    if (!spawn_one(launcher, JE_IMAGE_UART_DRIVER, 0u,
                   JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV,
                   &process, &endpoint)
        || wait_ready(endpoint, process, self_recv, JE_SVC_READY_DEADLINE) != 1u)
        return 101u;
    if (je_dev_assign(admin, process, &device) != JE_ABI_OK || device == 0u)
        return 102u;
    d9_message grant = {
        UINT32_C(0x44525952) + mode,
        (uint32_t)device
    };
    if (je_try_send(endpoint, &grant, sizeof grant) != JE_ABI_OK)
        return 103u;
    uint32_t result = d9_wait_result(endpoint, process, self_recv);
    if (result == 0u) return 104u;
    if (result == D9_DEGRADED) {
        /* A missing COM2 is an explicit degraded profile, never a successful
         * isolation run. The driver has already refused the probe and exited. */
        if (wait_dead(process, self_recv, JE_SVC_READY_DEADLINE) != 1u)
            return 108u;
        (void)je_handle_close(endpoint);
        (void)je_handle_close(process);
        return result;
    }
    if (mode == 1u) {
        if (je_dev_revoke(admin) != JE_ABI_OK) return 105u;
        d9_message go = { UINT32_C(0x54534552), 0u };
        if (je_try_send(endpoint, &go, sizeof go) != JE_ABI_OK
            || wait_dead(process, self_recv, JE_SVC_READY_DEADLINE) != 1u)
            return 106u;
    } else if (mode == 13u) {
        /* The hostile driver never ACKs and leaves the UART source enabled.
         * These timer sleeps are the survivor-progress proof: IRQ3 is masked
         * after its first delivery, but the supervisor continues to run. */
        for (uint32_t tick = 0u; tick < 3u; ++tick)
            if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT)
                return 109u;
        if (je_dev_revoke(admin) != JE_ABI_OK) return 110u;
        d9_message go = { UINT32_C(0x54534552), 0u };
        if (je_try_send(endpoint, &go, sizeof go) != JE_ABI_OK
            || wait_dead(process, self_recv, JE_SVC_READY_DEADLINE) != 1u)
            return 111u;
    } else if (wait_dead(process, self_recv, JE_SVC_READY_DEADLINE) != 1u) {
        return 107u;
    }
    (void)je_handle_close(endpoint);
    (void)je_handle_close(process);
    return result;
}

static int64_t device_mode14(uint64_t launcher, uint64_t admin,
                             uint64_t self_recv, report_state *values)
{
    for (uint32_t mode = 0u; mode <= 13u; ++mode) {
        uint32_t result = d9_run_one(launcher, admin, self_recv, mode);
        if (result != D9_PRESENT)
            return failed(values, UINT32_C(0x1400) + mode * 0x100u + result);
    }
    publish(JE_SVC_PHASE_DONE, JE_SVC_FAIL_NONE, values);
    return (int64_t)JE_SVC_DONE_EXIT_G;
}
#endif

#define WAIT_FAILED         0u
#define WAIT_OK             1u
#define WAIT_SLEEP_CHANNEL  2u
#define WAIT_SURVIVOR       3u

static uint32_t wait_ready(uint64_t endpoint, uint64_t process,
                           uint64_t self_recv, uint64_t deadline)
{
    uint64_t start = 0u;
    uint64_t now = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return WAIT_FAILED;
    for (;;) {
        uint32_t ready = ready_message(endpoint);
        if (ready != 0u) return ready == 1u ? WAIT_OK : WAIT_FAILED;
        je_abi_proc_status state = {0};
        if (status_of(process, &state) != JE_ABI_OK
            || state.state != JE_ABI_PROC_LIVE)
            return WAIT_FAILED;
        if (je_clock_ticks(&now) != JE_ABI_OK || now - start >= deadline)
            return WAIT_FAILED;
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT)
            return WAIT_SLEEP_CHANNEL;
    }
}

static uint32_t process_dead(uint64_t process, je_abi_proc_status *out)
{
    if (status_of(process, out) != JE_ABI_OK) return 0u;
    return out->state != JE_ABI_PROC_LIVE;
}

static uint32_t wait_dead(uint64_t process, uint64_t self_recv, uint64_t deadline)
{
    uint64_t start = 0u;
    uint64_t now = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return WAIT_FAILED;
    for (;;) {
        je_abi_proc_status state = {0};
        if (process_dead(process, &state)) return WAIT_OK;
        if (je_clock_ticks(&now) != JE_ABI_OK || now - start >= deadline)
            return WAIT_FAILED;
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT)
            return WAIT_SLEEP_CHANNEL;
    }
}

static uint32_t wait_restart_window(uint64_t restart_process, uint64_t survivor_process,
                                    uint64_t survivor_endpoint, uint64_t self_recv,
                                    report_state *report_values)
{
    uint64_t start = 0u;
    uint64_t now = 0u;
    if (!report_values || je_clock_ticks(&start) != JE_ABI_OK)
        return WAIT_FAILED;
    for (;;) {
        je_abi_proc_status state = {0};
        je_abi_proc_status survivor_state = {0};
        je_svc_heartbeat heartbeat = {0};
        uint64_t meta = 0u;
        if (status_of(survivor_process, &survivor_state) != JE_ABI_OK
            || survivor_state.state != JE_ABI_PROC_LIVE)
            return WAIT_SURVIVOR;
        uint64_t status = je_try_recv(survivor_endpoint, &heartbeat,
                                      sizeof heartbeat, 0u, &meta);
        if (status == JE_ABI_OK
            && (meta & UINT64_C(0xffffffff)) == JE_SVC_HEARTBEAT_LEN
            && (meta >> 32) == 0u
            && heartbeat.op == JE_SVC_OP_HEARTBEAT)
            ++report_values->heartbeats;
        else if (status == JE_ABI_H_PEER_CLOSED)
            return WAIT_SURVIVOR;
        else if (status != JE_ABI_H_EMPTY)
            return WAIT_FAILED;
        if (process_dead(restart_process, &state)) return WAIT_OK;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_SCENARIO_DEADLINE)
            return WAIT_FAILED;
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT)
            return WAIT_SLEEP_CHANNEL;
    }
}

static int64_t failed(report_state *report_values, uint32_t code);

#if JE_TEST_MODE == 13
typedef struct {
    uint64_t process;
    uint64_t control;
    uint32_t role;
    uint32_t delivered;
    uint32_t exercised;
    uint32_t closed;
} broker_client_state;

typedef struct {
    uint64_t process;
    uint64_t control;
    uint32_t generation;
    uint32_t restarts;
} broker_service_state;

typedef struct {
    uint64_t client_process;
    uint64_t service_process;
    uint32_t service_id;
    uint32_t service_generation;
    uint32_t state;
    uint32_t client_dead_observed;
    uint32_t service_dead_observed;
} broker_ledger_entry;

#define BROKER_LEDGER_FREE    0u
#define BROKER_LEDGER_PENDING 1u
#define BROKER_LEDGER_LIVE    2u

static uint32_t broker_next_transaction = 1u;

static uint32_t broker_reply(uint64_t control, uint32_t op, uint32_t detail)
{
    je_svc_broker_reply reply = { op, detail };
    return je_try_send(control, &reply, sizeof reply) == JE_ABI_OK;
}

static uint32_t broker_wait_ack(uint64_t control, uint64_t self_recv,
                                uint32_t transaction)
{
    uint64_t start = 0u;
    uint64_t now = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        uint8_t bytes[JE_SVC_CONFIG_LEN] = {0u};
        uint32_t received[1] = {0u};
        je_svc_broker_reply reply = {0};
        uint64_t meta = 0u;
        uint64_t status = je_try_recv_caps(control, bytes, sizeof bytes,
                                           received, 1u, 0u, &meta);
        if (status == JE_ABI_OK) {
            uint32_t length = (uint32_t)meta;
            uint32_t cap_count = (uint32_t)(meta >> 32);
            if (cap_count != 0u) {
                (void)je_handle_close(control);
                if (received[0] != 0u) (void)je_handle_close(received[0]);
                return 0u;
            }
            if (length != JE_SVC_BROKER_REPLY_LEN) continue;
            memcpy(&reply, bytes, sizeof reply);
            if (reply.op == JE_SVC_OP_BROKER_ACK
                && reply.detail == transaction)
                return 1u;
            /* A stale ACK, or another no-cap control record, is consumed and
             * discarded. It cannot satisfy this transaction. */
            continue;
        }
        if (status == JE_ABI_H_PEER_CLOSED) return 0u;
        if (status != JE_ABI_H_EMPTY) {
            (void)je_handle_close(control);
            return 0u;
        }
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_BROKER_ACK_DEADLINE)
            return 0u;
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT) return 0u;
    }
}

static uint32_t broker_alloc_transaction(uint32_t *out)
{
    if (!out || broker_next_transaction == 0u) return 0u;
    *out = broker_next_transaction++;
    if (*out == 0u) return 0u;
    return 1u;
}

static uint32_t broker_ledger_find(const broker_ledger_entry *ledger,
                                   uint64_t client_process,
                                   uint32_t service_id,
                                   uint32_t generation)
{
    for (uint32_t i = 0u; i < JE_BROKER_MAX; ++i)
        if (ledger[i].state != BROKER_LEDGER_FREE
            && ledger[i].client_process == client_process
            && ledger[i].service_id == service_id
            && ledger[i].service_generation == generation)
            return i;
    return JE_BROKER_MAX;
}

static void broker_ledger_collect(broker_ledger_entry *ledger)
{
    for (uint32_t i = 0u; i < JE_BROKER_MAX; ++i) {
        if (ledger[i].state == BROKER_LEDGER_FREE) continue;
        je_abi_proc_status client = {0};
        je_abi_proc_status service = {0};
        if (ledger[i].client_dead_observed == 0u
            && je_proc_status(ledger[i].client_process, &client) != JE_ABI_OK) {
            /* A stale INSPECT handle cannot be allowed to pin a bounded
             * ledger forever. The corresponding process identity is no longer
             * inspectable by this supervisor. */
            ledger[i] = (broker_ledger_entry){0};
            continue;
        }
        if (ledger[i].client_dead_observed == 0u
            && client.state == JE_ABI_PROC_DEAD)
            ledger[i].client_dead_observed = 1u;
        if (ledger[i].service_dead_observed == 0u
            && je_proc_status(ledger[i].service_process, &service) != JE_ABI_OK) {
            ledger[i] = (broker_ledger_entry){0};
            continue;
        }
        if (ledger[i].service_dead_observed == 0u
            && service.state == JE_ABI_PROC_DEAD)
            ledger[i].service_dead_observed = 1u;
        if (ledger[i].client_dead_observed != 0u
            && ledger[i].service_dead_observed != 0u)
            ledger[i] = (broker_ledger_entry){0};
    }
}

static void broker_ledger_mark_service_dead(broker_ledger_entry *ledger,
                                            uint64_t service_process)
{
    for (uint32_t i = 0u; i < JE_BROKER_MAX; ++i)
        if (ledger[i].state != BROKER_LEDGER_FREE
            && ledger[i].service_process == service_process)
            ledger[i].service_dead_observed = 1u;
}

static uint32_t broker_ledger_live(const broker_ledger_entry *ledger)
{
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < JE_BROKER_MAX; ++i)
        if (ledger[i].state != BROKER_LEDGER_FREE) ++count;
    return count;
}

static uint32_t broker_ledger_reserve(broker_ledger_entry *ledger,
                                      uint64_t client_process,
                                      uint64_t service_process,
                                      uint32_t service_id,
                                      uint32_t generation)
{
    for (uint32_t i = 0u; i < JE_BROKER_MAX; ++i) {
        if (ledger[i].state == BROKER_LEDGER_FREE) {
            ledger[i] = (broker_ledger_entry){
                client_process, service_process, service_id, generation,
                BROKER_LEDGER_PENDING, 0u, 0u
            };
            return i;
        }
    }
    return JE_BROKER_MAX;
}

static uint32_t broker_send_channel(broker_service_state *service,
                                    broker_client_state *client,
                                    uint64_t self_recv,
                                    broker_ledger_entry *ledger,
                                    uint32_t service_id,
                                    uint32_t response_op)
{
    /*
     * PROCESS handle values are the stable broker-ledger keys. The supervisor
     * keeps every referenced PROCESS handle open until teardown or after all
     * ledger entries referencing it have been collected, so the value cannot
     * be reused for another child instance while an entry is live or pending.
     */
    uint32_t ledger_slot = broker_ledger_reserve(
        ledger, client->process, service->process, service_id,
        service->generation);
    if (ledger_slot >= JE_BROKER_MAX) {
        return 0u;
    }
    uint32_t transaction = 0u;
    if (!broker_alloc_transaction(&transaction)) {
        ledger[ledger_slot] = (broker_ledger_entry){0};
        return 0u;
    }

    uint64_t packed = 0u;
    if (je_endpoint_create(&packed) != JE_ABI_OK) {
        ledger[ledger_slot] = (broker_ledger_entry){0};
        return 0u;
    }
    uint64_t service_end = (uint32_t)packed;
    uint64_t client_end = packed >> 32;
    uint64_t service_cap = service_end
        | ((uint64_t)(JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV) << 32);
    je_svc_service_bind bind = {
        JE_SVC_OP_SERVICE_BIND, service->generation, transaction
    };
    if (je_try_send_caps(service->control, &bind, sizeof bind,
                         &service_cap, 1u) != JE_ABI_OK) {
        (void)je_handle_close(service_end);
        (void)je_handle_close(client_end);
        ledger[ledger_slot] = (broker_ledger_entry){0};
        return 0u;
    }
    if (!broker_wait_ack(service->control, self_recv, transaction)) {
        (void)je_handle_close(service_end);
        (void)je_handle_close(client_end);
        return 0u;
    }

    uint64_t client_cap = client_end
        | ((uint64_t)(JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV) << 32);
    je_svc_broker_grant grant = {
        response_op, service->generation, transaction
    };
    if (je_try_send_caps(client->control, &grant, sizeof grant,
                         &client_cap, 1u) != JE_ABI_OK) {
        (void)je_handle_close(service_end);
        (void)je_handle_close(client_end);
        return 0u;
    }
    if (!broker_wait_ack(client->control, self_recv, transaction)) {
        (void)je_handle_close(service_end);
        (void)je_handle_close(client_end);
        return 0u;
    }

    (void)je_handle_close(service_end);
    (void)je_handle_close(client_end);
    ledger[ledger_slot].state = BROKER_LEDGER_LIVE;
    ++client->delivered;
    return 1u;
}

static uint32_t broker_restart_service(broker_service_state *service,
                                        uint64_t launcher, uint64_t self_recv,
                                        broker_ledger_entry *ledger)
{
    je_abi_proc_status status = {0};
    if (je_proc_status(service->process, &status) != JE_ABI_OK
        || status.state == JE_ABI_PROC_LIVE
        || service->restarts >= JE_SVC_RESTART_LIMIT
        || service->generation == UINT32_MAX)
        return 0u;
    (void)je_proc_kill(service->process);
    uint64_t old_process = service->process;
    (void)je_handle_close(service->control);
    uint64_t old_start = 0u;
    if (je_clock_ticks(&old_start) != JE_ABI_OK) return 0u;
    for (;;) {
        if (je_proc_status(old_process, &status) != JE_ABI_OK) return 0u;
        if (status.state == JE_ABI_PROC_DEAD) break;
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - old_start >= JE_SVC_BROKER_ACK_DEADLINE)
            return 0u;
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT) return 0u;
    }
    /*
     * Keep old_process open: live or PENDING ledger entries still use this
     * supervisor-owned handle value as their stable process-instance key.
     * Teardown owns the final close unless a later collection proves that no
     * entry references it.
     */
    broker_ledger_mark_service_dead(ledger, old_process);
    service->process = 0u;
    service->control = 0u;
    uint64_t process = 0u;
    uint64_t control = 0u;
    if (!spawn_one(launcher, JE_IMAGE_BROKER_SVC, 0u,
                   JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV,
                   &process, &control))
        return 0u;
    if (wait_ready(control, process, self_recv, JE_SVC_READY_DEADLINE)
        != WAIT_OK) {
        (void)je_proc_kill(process);
        (void)je_handle_close(control);
        (void)je_handle_close(process);
        return 0u;
    }
    service->process = process;
    service->control = control;
    service->generation++;
    service->restarts++;
    return 1u;
}

static uint32_t broker_handle_request(broker_service_state *service,
                                      broker_client_state *client,
                                      uint64_t launcher, uint64_t self_recv,
                                      broker_ledger_entry *ledger,
                                      const je_svc_broker_request *request,
                                      uint32_t *a_exhausted)
{
    if (request->service_id >= JE_SVC_BROKER_SERVICE_COUNT)
        return broker_reply(client->control, JE_SVC_OP_BROKER_UNAVAIL,
                            JE_SVC_UNAVAIL_BAD_SERVICE);
    if (client->delivered >= JE_SVC_BROKER_CLIENT_CAP) {
        if (client->role == JE_BROKER_TEST_ROLE_A) *a_exhausted = 1u;
        return broker_reply(client->control, JE_SVC_OP_BROKER_EXHAUSTED, 0u);
    }
    uint32_t existing = broker_ledger_find(
        ledger, client->process, request->service_id, service->generation);
    if (existing < JE_BROKER_MAX
        && ledger[existing].state != BROKER_LEDGER_FREE)
        return broker_reply(client->control, JE_SVC_OP_BROKER_ALREADY, 0u);
    broker_ledger_collect(ledger);
    if (broker_ledger_live(ledger) >= JE_BROKER_MAX)
        return broker_reply(client->control, JE_SVC_OP_BROKER_UNAVAIL,
                            JE_SVC_UNAVAIL_NOT_READY);
    je_abi_proc_status service_status = {0};
    if (je_proc_status(service->process, &service_status) != JE_ABI_OK)
        return broker_reply(client->control, JE_SVC_OP_BROKER_UNAVAIL,
                            JE_SVC_UNAVAIL_NOT_READY);
    if (service_status.state != JE_ABI_PROC_LIVE) {
        if (!broker_restart_service(service, launcher, self_recv, ledger))
            return broker_reply(client->control, JE_SVC_OP_BROKER_UNAVAIL,
                                service->restarts >= JE_SVC_RESTART_LIMIT
                                    ? JE_SVC_UNAVAIL_TERMINAL
                                    : JE_SVC_UNAVAIL_NOT_READY);
    }
    uint32_t response = service->generation == 1u
        ? JE_SVC_OP_BROKER_GRANT : JE_SVC_OP_BROKER_RESTARTED;
    if (!broker_send_channel(service, client, self_recv, ledger,
                             request->service_id, response))
        return broker_reply(client->control, JE_SVC_OP_BROKER_UNAVAIL,
                            JE_SVC_UNAVAIL_NOT_READY);
    return 1u;
}

static uint32_t broker_poll_client(broker_service_state *service,
                                   broker_client_state *client,
                                   uint64_t launcher, uint64_t self_recv,
                                   broker_ledger_entry *ledger,
                                   uint32_t *a_exhausted)
{
    uint8_t bytes[JE_SVC_CONFIG_LEN] = {0u};
    uint32_t received_caps[1] = {0u};
    uint64_t meta = 0u;
    uint64_t status = je_try_recv_caps(client->control, bytes, sizeof bytes,
                                       received_caps, 1u, 0u, &meta);
    if (status == JE_ABI_H_EMPTY) return 1u;
    if (status == JE_ABI_H_PEER_CLOSED) return 2u;
    if (status != JE_ABI_OK) {
        (void)je_handle_close(client->control);
        client->closed = 1u;
        return 2u;
    }
    if ((meta >> 32) != 0u) {
        if (received_caps[0] != 0u) (void)je_handle_close(received_caps[0]);
        (void)je_handle_close(client->control);
        client->closed = 1u;
        return 2u;
    }
    if ((meta & UINT64_C(0xffffffff)) == sizeof(je_svc_config)) {
        je_svc_config progress = {0};
        memcpy(&progress, bytes, sizeof progress);
        if (progress.op == JE_SVC_OP_CONFIG
            && progress.incarnation == JE_BROKER_TEST_EXERCISED
            && progress.crash_before == 0u && progress.reserved == 0u) {
            client->exercised = 1u;
            return 4u;
        }
        return 1u;
    }
    if ((meta & UINT64_C(0xffffffff)) != JE_SVC_BROKER_REQUEST_LEN)
        return 1u;
    je_svc_broker_request request = {0};
    memcpy(&request, bytes, sizeof request);
    if (request.op == JE_SVC_OP_BROKER_ACK) return 1u;
    if (request.op != JE_SVC_OP_BROKER_REQUEST) return 1u;
    return broker_handle_request(service, client, launcher, self_recv,
                                 ledger, &request, a_exhausted);
}

static int64_t broker_mode13(uint64_t launcher, uint64_t self_recv,
                             report_state *report_values)
{
    broker_service_state service = {0};
    broker_client_state clients[2] = {0};
    broker_ledger_entry ledger[JE_BROKER_MAX] = {0};
    uint32_t a_exhausted = 0u;
    uint32_t finish_sent = 0u;
    uint64_t start = 0u;
    uint64_t now = 0u;

    if (!spawn_one(launcher, JE_IMAGE_BROKER_SVC, 0u,
                   JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV,
                   &service.process, &service.control)
        || wait_ready(service.control, service.process, self_recv,
                      JE_SVC_READY_DEADLINE) != WAIT_OK)
        return failed(report_values, JE_SVC_FAIL_RESTART);
    service.generation = 1u;

    for (uint32_t i = 0u; i < 2u; ++i) {
        if (!spawn_one(launcher, JE_IMAGE_BROKER_CLI, 0u,
                       JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV,
                       &clients[i].process, &clients[i].control)
            || wait_ready(clients[i].control, clients[i].process, self_recv,
                          JE_SVC_READY_DEADLINE) != WAIT_OK)
            return failed(report_values, JE_SVC_FAIL_READINESS);
        clients[i].role = i;
        je_svc_config role = {
            JE_SVC_OP_CONFIG, i, 0u, 0u
        };
        if (je_try_send(clients[i].control, &role, sizeof role) != JE_ABI_OK)
            return failed(report_values, JE_SVC_FAIL_READINESS);
    }
    publish(JE_SVC_PHASE_READY, JE_SVC_FAIL_NONE, report_values);
    if (je_yield() != JE_ABI_OK)
        return failed(report_values, JE_SVC_FAIL_READINESS);
    publish(JE_SVC_PHASE_RUNNING, JE_SVC_FAIL_NONE, report_values);
    if (je_clock_ticks(&start) != JE_ABI_OK)
        return failed(report_values, JE_SVC_FAIL_READINESS);

    for (;;) {
        broker_ledger_collect(ledger);
        if (broker_ledger_live(ledger) > JE_BROKER_MAX)
            return failed(report_values, JE_SVC_FAIL_READINESS);
        je_abi_proc_status service_status = {0};
        if (je_proc_status(service.process, &service_status) != JE_ABI_OK)
            return failed(report_values, JE_SVC_FAIL_RESTART);
        if (service_status.state != JE_ABI_PROC_LIVE) {
            if (!a_exhausted || service.restarts >= JE_SVC_RESTART_LIMIT
                || !broker_restart_service(&service, launcher, self_recv, ledger))
                return failed(report_values, JE_SVC_FAIL_RESTART);
            ++report_values->restarts;
        }
        for (uint32_t i = 0u; i < 2u; ++i) {
            if (clients[i].closed != 0u) continue;
            uint32_t result = broker_poll_client(
                &service, &clients[i], launcher, self_recv, ledger,
                &a_exhausted);
            if (result == 0u) {
                report_values->crash_attempts = 0x100u + result;
                return failed(report_values, JE_SVC_FAIL_READINESS);
            }
        }
        if (!a_exhausted && clients[0].exercised && clients[1].exercised) {
            je_svc_config trigger = {
                JE_SVC_OP_CONFIG, 0u, JE_SVC_RESTART_CRASH_BEFORE, 0u
            };
            if (je_try_send(service.control, &trigger, sizeof trigger)
                != JE_ABI_OK)
                return failed(report_values, JE_SVC_FAIL_RESTART);
            a_exhausted = 2u; /* trigger sent; restart is now expected */
        }
        if (a_exhausted == 1u && !finish_sent) {
            je_svc_config finish = {
                JE_SVC_OP_CONFIG, JE_BROKER_TEST_FINISH, 0u, 0u
            };
            if (je_try_send(clients[1].control, &finish, sizeof finish)
                != JE_ABI_OK)
                return failed(report_values, JE_SVC_FAIL_READINESS);
            finish_sent = 1u;
        }
        je_abi_proc_status client_a = {0};
        je_abi_proc_status client_b = {0};
        if (je_proc_status(clients[0].process, &client_a) != JE_ABI_OK
            || je_proc_status(clients[1].process, &client_b) != JE_ABI_OK)
            return failed(report_values, JE_SVC_FAIL_READINESS);
        if (a_exhausted == 2u) {
            /* The third request sets a_exhausted to 1 after restart. */
            if (clients[0].delivered == JE_SVC_BROKER_CLIENT_CAP)
                a_exhausted = 1u;
        }
        if (finish_sent && client_a.state != JE_ABI_PROC_LIVE
            && client_b.state != JE_ABI_PROC_LIVE)
            break;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_SCENARIO_DEADLINE)
            return failed(report_values, JE_SVC_FAIL_RESTART);
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT)
            return failed(report_values, JE_SVC_FAIL_SLEEP_CHANNEL);
    }
    publish(JE_SVC_PHASE_DONE, JE_SVC_FAIL_NONE, report_values);
    return (int64_t)JE_SVC_DONE_EXIT_G;
}

#endif

#if JE_TEST_MODE == 15
typedef struct {
    uint64_t process;
    uint64_t control;
    uint64_t device;
    uint32_t generation;
    uint32_t restarts;
    uint32_t live;
    uint64_t old_process[JE_SVC_UART_RESTART_LIMIT];
    uint32_t old_count;
} uart_service_state;

typedef struct {
    uint64_t process;
    uint64_t control;
    uint32_t progress;
    uint32_t delivered;
    uint32_t closed;
    uint32_t configured;
} uart_client_state;

typedef struct {
    uint64_t client_process;
    uint64_t service_process;
    uint32_t service_generation;
    uint32_t state;
    uint32_t client_dead;
    uint32_t service_dead;
} uart_ledger_entry;

#define UART_LEDGER_FREE 0u
#define UART_LEDGER_LIVE 1u

#define UART_SCENARIO_DEADLINE (JE_SVC_UART_DEADLINE * 2u)

typedef struct {
    uint32_t timeout_done;
    uint32_t detach_done;
    uint32_t revoke_done;
    uint32_t hardened_done;
    uint32_t client_i2_done;
} uart_demo_checklist;

static uint32_t uart_transaction = 1u;

static uint32_t uart_send_config(uint64_t endpoint, uint32_t value)
{
    je_svc_config message = { JE_SVC_OP_CONFIG, value, 0u, 0u };
    return je_try_send(endpoint, &message, sizeof message) == JE_ABI_OK;
}

static uint32_t uart_wait_control(uint64_t endpoint, uint32_t expected_op,
                                  uint32_t expected_arg, uint64_t self_recv)
{
    uint64_t start = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        je_svc_uart_control message = {0};
        uint32_t minted[1] = {0u};
        uint64_t meta = 0u;
        uint64_t status = je_try_recv_caps(endpoint, &message, sizeof message,
                                           minted, 1u, 0u, &meta);
        if (status == JE_ABI_OK) {
            if ((meta & UINT64_C(0xffffffff)) != JE_SVC_UART_CONTROL_LEN
                || (meta >> 32) != 0u || message.op != expected_op
                || message.arg != expected_arg)
                return 0u;
            return 1u;
        }
        if (status == JE_ABI_H_PEER_CLOSED || status != JE_ABI_H_EMPTY)
            return 0u;
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_UART_DEADLINE)
            return 0u;
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT) return 0u;
    }
}

static uint32_t uart_wait_dead(uint64_t process, uint64_t self_recv)
{
    uint64_t start = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        je_abi_proc_status state = {0};
        if (je_proc_status(process, &state) != JE_ABI_OK) return 0u;
        if (state.state == JE_ABI_PROC_DEAD) return 1u;
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_UART_DEADLINE)
            return 0u;
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT) return 0u;
    }
}

static uint32_t uart_wait_ack(uint64_t endpoint, uint32_t transaction,
                              uint64_t self_recv)
{
    uint64_t start = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        je_svc_broker_reply reply = {0};
        uint32_t minted[1] = {0u};
        uint64_t meta = 0u;
        uint64_t status = je_try_recv_caps(endpoint, &reply, sizeof reply,
                                           minted, 1u, 0u, &meta);
        if (status == JE_ABI_OK) {
            if ((meta & UINT64_C(0xffffffff)) == JE_SVC_BROKER_REPLY_LEN
                && (meta >> 32) == 0u
                && reply.op == JE_SVC_OP_BROKER_ACK
                && reply.detail == transaction)
                return 1u;
            return 0u;
        }
        if (status == JE_ABI_H_PEER_CLOSED || status != JE_ABI_H_EMPTY)
            return 0u;
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_UART_DEADLINE)
            return 0u;
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT) return 0u;
    }
}

static uint32_t uart_spawn_ready(uint64_t launcher, uint64_t self_recv,
                                 uint64_t *process, uint64_t *control)
{
    if (!spawn_one(launcher, JE_IMAGE_UART_SVC, 0u,
                   JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV,
                   process, control))
        return 0u;
    return wait_ready(*control, *process, self_recv, JE_SVC_UART_DEADLINE)
        == WAIT_OK;
}

static uint32_t uart_attach(uint64_t admin, uint64_t process,
                            uint64_t control, uint64_t self_recv,
                            uint64_t *device)
{
    if (!device) return 0u;
    if (je_dev_assign(admin, process, device) != JE_ABI_OK || *device == 0u)
        return 0u;
    je_svc_uart_attach attach = {
        JE_SVC_OP_UART_ATTACH, (uint32_t)*device
    };
    if (je_try_send(control, &attach, sizeof attach) != JE_ABI_OK
        || !uart_wait_control(control, JE_SVC_OP_UART_ATTACH_OK,
                              JE_SVC_UART_PRESENT, self_recv))
        return 0u;
    return 1u;
}

static uint32_t uart_ledger_mark_dead(uart_ledger_entry *ledger,
                                      uint64_t service_process)
{
    for (uint32_t i = 0u; i < JE_SVC_UART_CLIENT_CAP; ++i)
        if (ledger[i].state != UART_LEDGER_FREE
            && ledger[i].service_process == service_process)
            ledger[i].service_dead = 1u;
    return 1u;
}

static void uart_ledger_collect(uart_ledger_entry *ledger,
                                uart_client_state *client,
                                uart_service_state *service)
{
    for (uint32_t i = 0u; i < JE_SVC_UART_CLIENT_CAP; ++i) {
        if (ledger[i].state == UART_LEDGER_FREE) continue;
        je_abi_proc_status client_status = {0};
        je_abi_proc_status service_status = {0};
        if (!ledger[i].client_dead
            && je_proc_status(ledger[i].client_process, &client_status)
                == JE_ABI_OK
            && client_status.state == JE_ABI_PROC_DEAD)
            ledger[i].client_dead = 1u;
        if (!ledger[i].service_dead
            && je_proc_status(ledger[i].service_process, &service_status)
                == JE_ABI_OK
            && service_status.state == JE_ABI_PROC_DEAD)
            ledger[i].service_dead = 1u;
        if (ledger[i].client_dead && ledger[i].service_dead)
            ledger[i] = (uart_ledger_entry){0};
    }
    for (uint32_t i = 0u; i < service->old_count; ++i) {
        if (service->old_process[i] == 0u) continue;
        uint32_t referenced = 0u;
        for (uint32_t j = 0u; j < JE_SVC_UART_CLIENT_CAP; ++j)
            if (ledger[j].state != UART_LEDGER_FREE
                && ledger[j].service_process == service->old_process[i])
                referenced = 1u;
        if (referenced == 0u) {
            (void)je_handle_close(service->old_process[i]);
            service->old_process[i] = 0u;
        }
    }
    (void)client;
}

static uint32_t uart_ledger_slot(uart_ledger_entry *ledger,
                                 uint64_t client_process,
                                 uint64_t service_process,
                                 uint32_t generation)
{
    for (uint32_t i = 0u; i < JE_SVC_UART_CLIENT_CAP; ++i) {
        if (ledger[i].state == UART_LEDGER_FREE) {
            ledger[i] = (uart_ledger_entry){
                client_process, service_process, generation,
                UART_LEDGER_LIVE, 0u, 0u
            };
            return i;
        }
    }
    return JE_SVC_UART_CLIENT_CAP;
}

static uint32_t uart_broker_channel(uart_service_state *service,
                                    uart_client_state *client,
                                    uint64_t self_recv,
                                    uart_ledger_entry *ledger,
                                    uint32_t *out_op,
                                    uint32_t *out_channel)
{
    if (!service || !client || !out_op || !out_channel
        || client->delivered >= JE_SVC_UART_CLIENT_CAP)
        return 0u;
    uint32_t slot = uart_ledger_slot(ledger, client->process,
                                     service->process, service->generation);
    if (slot >= JE_SVC_UART_CLIENT_CAP || uart_transaction == 0u)
        return 0u;
    uint32_t transaction = uart_transaction++;
    uint64_t packed = 0u;
    if (je_endpoint_create(&packed) != JE_ABI_OK) return 0u;
    uint64_t service_end = (uint32_t)packed;
    uint64_t client_end = packed >> 32;
    uint64_t service_cap = service_end
        | ((uint64_t)(JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV) << 32);
    je_svc_service_bind bind = {
        JE_SVC_OP_SERVICE_BIND, service->generation, transaction
    };
    if (je_try_send_caps(service->control, &bind, sizeof bind,
                         &service_cap, 1u) != JE_ABI_OK
        || !uart_wait_ack(service->control, transaction, self_recv)) {
        (void)je_handle_close(service_end);
        (void)je_handle_close(client_end);
        ledger[slot] = (uart_ledger_entry){0};
        return 0u;
    }
    uint32_t response = service->generation == 1u
        ? JE_SVC_OP_BROKER_GRANT : JE_SVC_OP_BROKER_RESTARTED;
    uint64_t client_cap = client_end
        | ((uint64_t)(JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV) << 32);
    je_svc_broker_grant grant = { response, service->generation, transaction };
    if (je_try_send_caps(client->control, &grant, sizeof grant,
                         &client_cap, 1u) != JE_ABI_OK) {
        (void)je_handle_close(service_end);
        (void)je_handle_close(client_end);
        ledger[slot] = (uart_ledger_entry){0};
        return 0u;
    }
    if (!uart_wait_ack(client->control, transaction, self_recv)) {
        (void)je_handle_close(service_end);
        (void)je_handle_close(client_end);
        ledger[slot] = (uart_ledger_entry){0};
        return 0u;
    }
    (void)je_handle_close(service_end);
    (void)je_handle_close(client_end);
    ++client->delivered;
    *out_op = response;
    *out_channel = 0u;
    return 1u;
}

static uint32_t uart_restart_service(uart_service_state *service,
                                     uint64_t launcher, uint64_t admin,
                                     uint64_t self_recv,
                                     uart_ledger_entry *ledger)
{
    if (!service || service->restarts >= JE_SVC_UART_RESTART_LIMIT
        || service->old_count >= JE_SVC_UART_RESTART_LIMIT)
        return 0u;
    je_abi_proc_status state = {0};
    if (je_proc_status(service->process, &state) != JE_ABI_OK) return 0u;
    if (state.state == JE_ABI_PROC_LIVE
        && je_proc_kill(service->process) != JE_ABI_OK)
        return 0u;
    if (!uart_wait_dead(service->process, self_recv)) return 0u;
    uart_ledger_mark_dead(ledger, service->process);
    service->old_process[service->old_count++] = service->process;
    (void)je_handle_close(service->control);
    service->control = 0u;
    service->live = 0u;
    uint64_t process = 0u;
    uint64_t control = 0u;
    uint64_t device = 0u;
    if (!uart_spawn_ready(launcher, self_recv, &process, &control)
        || !uart_attach(admin, process, control, self_recv, &device))
        return 0u;
    service->process = process;
    service->control = control;
    service->generation++;
    service->restarts++;
    service->live = 1u;
    return 1u;
}

static uint32_t uart_handle_request(uart_service_state *service,
                                    uart_client_state *client,
                                    uint64_t launcher, uint64_t admin,
                                    uint64_t self_recv,
                                    uart_ledger_entry *ledger)
{
    je_abi_proc_status status = {0};
    if (service->live != 0u
        && (je_proc_status(service->process, &status) != JE_ABI_OK
            || status.state != JE_ABI_PROC_LIVE))
        service->live = 0u;
    if (service->live == 0u
        && !uart_restart_service(service, launcher, admin, self_recv, ledger)) {
        return 0u;
    }
    uint32_t response = 0u;
    uint32_t channel = 0u;
    return uart_broker_channel(service, client, self_recv, ledger,
                               &response, &channel);
}

static uint32_t uart_wait_progress(uart_client_state *client,
                                   uint32_t expected, uint64_t self_recv)
{
    uint64_t start = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    while (client->progress != expected) {
        uint8_t bytes[JE_SVC_CONFIG_LEN] = {0u};
        uint32_t minted[1] = {0u};
        uint64_t meta = 0u;
        uint64_t status = je_try_recv_caps(client->control, bytes,
                                           sizeof bytes, minted, 1u, 0u,
                                           &meta);
        if (status == JE_ABI_OK) {
            je_svc_config message = {0};
            if ((meta & UINT64_C(0xffffffff)) != JE_SVC_CONFIG_LEN
                || (meta >> 32) != 0u)
                return 0u;
            memcpy(&message, bytes, sizeof message);
            if (message.op != JE_SVC_OP_CONFIG || message.crash_before != 0u
                || message.reserved != 0u)
                return 0u;
            client->progress = message.incarnation;
            continue;
        }
        if (status != JE_ABI_H_EMPTY) return 0u;
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_UART_DEADLINE)
            return 0u;
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT) return 0u;
    }
    return 1u;
}

static uint32_t uart_timeout_demo(uart_service_state *service,
                                  uint64_t self_recv,
                                  uart_ledger_entry *ledger)
{
    uint64_t start = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    je_svc_uart_control ping = { JE_SVC_OP_UART_PING, 1u };
    if (je_try_send(service->control, &ping, sizeof ping) != JE_ABI_OK)
        return 0u;
    for (;;) {
        je_svc_uart_control reply = {0};
        uint32_t minted[1] = {0u};
        uint64_t meta = 0u;
        uint64_t status = je_try_recv_caps(service->control, &reply,
                                           sizeof reply, minted, 1u, 0u,
                                           &meta);
        if (status == JE_ABI_OK) {
            if ((meta & UINT64_C(0xffffffff)) != JE_SVC_UART_CONTROL_LEN
                || (meta >> 32) != 0u || reply.op != JE_SVC_OP_UART_PONG
                || reply.arg != 1u)
                return 0u;
            return 0u;
        }
        if (status != JE_ABI_H_EMPTY) return 0u;
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK) return 0u;
        if (now - start >= JE_SVC_UART_DEADLINE) break;
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT) return 0u;
    }
    if (je_proc_kill(service->process) != JE_ABI_OK
        || !uart_wait_dead(service->process, self_recv))
        return 0u;
    uart_ledger_mark_dead(ledger, service->process);
    service->live = 0u;
    (void)je_handle_close(service->control);
    service->control = 0u;
    return 1u;
}

static uint32_t uart_detach_demo(uart_service_state *service,
                                 uart_client_state *client, uint64_t admin,
                                 uint64_t self_recv)
{
    je_svc_uart_attach detach = { JE_SVC_OP_UART_ATTACH, 0u };
    if (je_try_send(service->control, &detach, sizeof detach) != JE_ABI_OK
        || !uart_wait_control(service->control, JE_SVC_OP_UART_DETACHED, 0u,
                              self_recv)
        || !uart_send_config(client->control, JE_UART_COMMAND_DETACH)
        || !uart_wait_progress(client, JE_UART_PROGRESS_D_SENT, self_recv))
        return 0u;
    uint64_t device = 0u;
    if (!uart_attach(admin, service->process, service->control, self_recv,
                     &device))
        return 0u;
    if (je_dev_revoke(admin) != JE_ABI_OK
        || !uart_send_config(client->control, JE_UART_COMMAND_REVOKED))
        return 0u;
    return 1u;
}

static uint32_t uart_poll_hardened(uart_service_state *service,
                                   uart_client_state *client, uint64_t admin,
                                   uart_demo_checklist *checklist)
{
    uint8_t bytes[JE_SVC_UART_REQUEST_LEN] = {0u};
    uint32_t minted[1] = {0u};
    uint64_t meta = 0u;
    uint64_t status = je_try_recv_caps(client->control, bytes, sizeof bytes,
                                       minted, 1u, 0u, &meta);
    if (status == JE_ABI_H_EMPTY) return 1u;
    if (status == JE_ABI_H_PEER_CLOSED) {
        client->closed = 1u;
        return 1u;
    }
    if (status != JE_ABI_OK || (meta >> 32) != 0u)
        return 0x101u;
    if ((meta & UINT64_C(0xffffffff)) == JE_SVC_CONFIG_LEN) {
        je_svc_config role = {0};
        memcpy(&role, bytes, sizeof role);
        if (role.op != JE_SVC_OP_CONFIG) return 0u;
        if (!client->configured) {
            if (role.incarnation != JE_UART_CLIENT_HARDENED)
                return 0u;
            client->configured = 1u;
            return 1u;
        }
        if (role.incarnation != JE_UART_PROGRESS_DONE)
            return 0u;
        if (role.crash_before != 0u || role.reserved != 0u) return 0u;
        client->progress = role.incarnation;
        if (checklist) checklist->hardened_done = 1u;
        return 1u;
    }
    if ((meta & UINT64_C(0xffffffff)) != JE_SVC_BROKER_REQUEST_LEN)
        return 0u;
    je_svc_broker_request request = {0};
    memcpy(&request, bytes, sizeof request);
    if (request.op != JE_SVC_OP_BROKER_HARDENED
        || request.service_id != JE_SVC_UART_SERVICE_ID)
        return 0u;
    uint64_t profile = 0u;
    uint64_t result = je_dev_assign_flags(
        admin, service->process, JE_ABI_DEV_REQUIRE_DMA_CONTAINED, &profile);
    if (result != JE_ABI_UNSUPPORTED
        || profile != JE_ABI_DEV_PROFILE_LEGACY_PIO)
        return 0u;
    je_svc_broker_reply unavailable = {
        JE_SVC_OP_BROKER_UNAVAIL, JE_SVC_UNAVAIL_HARDENED
    };
    return je_try_send(client->control, &unavailable, sizeof unavailable)
        == JE_ABI_OK ? 1u : 0u;
}

static uint32_t uart_poll_normal(uart_service_state *service,
                                 uart_client_state *client, uint64_t launcher,
                                 uint64_t admin, uint64_t self_recv,
                                 uart_ledger_entry *ledger,
                                 uart_demo_checklist *checklist)
{
    uint8_t bytes[JE_SVC_UART_REQUEST_LEN] = {0u};
    uint32_t minted[1] = {0u};
    uint64_t meta = 0u;
    uint64_t status = je_try_recv_caps(client->control, bytes, sizeof bytes,
                                       minted, 1u, 0u, &meta);
    if (status == JE_ABI_H_EMPTY) return 1u;
    if (status == JE_ABI_H_PEER_CLOSED) {
        client->closed = 1u;
        return 1u;
    }
    if (status != JE_ABI_OK || (meta >> 32) != 0u)
        return 0u;
    if ((meta & UINT64_C(0xffffffff)) == JE_SVC_CONFIG_LEN) {
        je_svc_config progress = {0};
        memcpy(&progress, bytes, sizeof progress);
        if (progress.op != JE_SVC_OP_CONFIG || progress.crash_before != 0u
            || progress.reserved != 0u)
            return 0u;
        client->progress = progress.incarnation;
        if (client->progress == JE_UART_PROGRESS_I1
            && checklist && checklist->timeout_done == 0u) {
            rollback_go = JE_UART_COMMAND_HANG;
            if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT)
                return 0u;
            uint32_t timeout_result =
                uart_timeout_demo(service, self_recv, ledger);
            if (timeout_result != 1u) return timeout_result;
            checklist->timeout_done = 1u;
        } else if (client->progress == JE_UART_PROGRESS_I2
                   && checklist && checklist->detach_done == 0u) {
            if (!uart_detach_demo(service, client, admin, self_recv))
                return 0u;
            checklist->detach_done = 1u;
        }
        return 1u;
    }
    if ((meta & UINT64_C(0xffffffff)) != JE_SVC_BROKER_REQUEST_LEN)
        return 0u;
    je_svc_broker_request request = {0};
    memcpy(&request, bytes, sizeof request);
    if (request.op != JE_SVC_OP_BROKER_REQUEST
        || request.service_id != JE_SVC_UART_SERVICE_ID)
        return 0u;
    return uart_handle_request(service, client, launcher, admin, self_recv,
                               ledger);
}

static int64_t uart_mode15(uint64_t launcher, uint64_t admin,
                           uint64_t self_recv, report_state *report_values)
{
    uart_service_state service = {0};
    uart_client_state normal = {0};
    uart_client_state hardened = {0};
    uart_ledger_entry ledger[JE_SVC_UART_CLIENT_CAP] = {0};
    uart_demo_checklist checklist = {0};
    uint64_t scenario_start = 0u;
    if (je_clock_ticks(&scenario_start) != JE_ABI_OK)
        return failed(report_values, JE_SVC_FAIL_UART_DEADLINE);
    if (!uart_spawn_ready(launcher, self_recv, &service.process,
                          &service.control)
        || !uart_attach(admin, service.process, service.control, self_recv,
                        &service.device))
        return failed(report_values, JE_SVC_FAIL_RESTART);
    service.generation = 1u;
    service.live = 1u;

    if (!spawn_one(launcher, JE_IMAGE_UART_CLIENT, 0u,
                   JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV,
                   &normal.process, &normal.control)
        || wait_ready(normal.control, normal.process, self_recv,
                      JE_SVC_UART_DEADLINE) != WAIT_OK
        || !uart_send_config(normal.control, JE_UART_CLIENT_NORMAL))
        return failed(report_values, JE_SVC_FAIL_READINESS);
    if (!spawn_one(launcher, JE_IMAGE_UART_CLIENT, 0u,
                   JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV,
                   &hardened.process, &hardened.control)
        || wait_ready(hardened.control, hardened.process, self_recv,
                      JE_SVC_UART_DEADLINE) != WAIT_OK
        || !uart_send_config(hardened.control, JE_UART_CLIENT_HARDENED))
        return failed(report_values, JE_SVC_FAIL_READINESS);
    hardened.configured = 1u;

    publish(JE_SVC_PHASE_READY, JE_SVC_FAIL_NONE, report_values);
    if (je_yield() != JE_ABI_OK) return failed(report_values, JE_SVC_FAIL_READINESS);
    publish(JE_SVC_PHASE_RUNNING, JE_SVC_FAIL_NONE, report_values);
    for (;;) {
        uart_ledger_collect(ledger, &normal, &service);
        if (!hardened.closed) {
            uint32_t hardened_result =
                uart_poll_hardened(&service, &hardened, admin, &checklist);
            if (hardened_result != 1u)
                return failed(report_values, JE_SVC_FAIL_READINESS);
        }
        if (!normal.closed) {
            uint32_t normal_result =
                uart_poll_normal(&service, &normal, launcher, admin,
                                 self_recv, ledger, &checklist);
            if (normal_result != 1u)
                return failed(report_values, JE_SVC_FAIL_READINESS);
        }
        if (checklist.client_i2_done == 0u
            && (normal.progress == JE_UART_PROGRESS_I2
                || normal.progress == JE_UART_PROGRESS_D_SENT
                || normal.progress == JE_UART_PROGRESS_R_SENT
                || normal.progress == JE_UART_PROGRESS_DONE))
            checklist.client_i2_done = 1u;
        if (checklist.revoke_done == 0u
            && normal.progress == JE_UART_PROGRESS_R_SENT
            && service.generation == 3u)
            checklist.revoke_done = 1u;
        je_abi_proc_status normal_status = {0};
        je_abi_proc_status hard_status = {0};
        if (je_proc_status(normal.process, &normal_status) != JE_ABI_OK
            || je_proc_status(hardened.process, &hard_status) != JE_ABI_OK)
            return failed(report_values, JE_SVC_FAIL_READINESS);
        if (hardened.progress == JE_UART_PROGRESS_DONE
            && hard_status.state != JE_ABI_PROC_LIVE)
            hardened.closed = 1u;
        if (normal.progress == JE_UART_PROGRESS_DONE
            && normal_status.state != JE_ABI_PROC_LIVE)
            normal.closed = 1u;
        if (normal.closed && hardened.closed && service.live != 0u
            && service.generation == 3u
            && checklist.timeout_done != 0u
            && checklist.detach_done != 0u
            && checklist.revoke_done != 0u
            && checklist.hardened_done != 0u
            && checklist.client_i2_done != 0u) {
            publish(JE_SVC_PHASE_DONE, JE_SVC_FAIL_NONE, report_values);
            return (int64_t)JE_SVC_UART_DONE_EXIT;
        }
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - scenario_start >= UART_SCENARIO_DEADLINE)
            return failed(report_values, JE_SVC_FAIL_UART_DEADLINE);
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT)
            return failed(report_values, JE_SVC_FAIL_SLEEP_CHANNEL);
    }
}
#endif

static int64_t failed(report_state *report_values, uint32_t code)
{
    publish(JE_SVC_PHASE_FAILED, code, report_values);
    return (int64_t)code;
}

int64_t je_main(uint64_t version, uint64_t n, uint64_t h0, uint64_t h1,
                uint64_t h2, uint64_t h3)
{
    uint64_t self_packed = 0u;
    uint64_t self_recv = 0u;
    uint64_t restart_proc = 0u, survivor_proc = 0u;
    uint64_t restart_ep = 0u, survivor_ep = 0u;
    uint64_t spoofer_proc = 0u, spoofer_ep = 0u;
    uint64_t supervisor_self = 0u;
    uint64_t config;
    uint32_t incarnation = 0u;
    je_svc_config restart_config = {0};
    report_state report_values = {0};
    uint64_t status;

    (void)report_padding;
    (void)supervisor_rodata;
    if (version != JE_ABI_STARTUP_V1 || n != 2u
        || h1 == 0u || h2 != 0u || h3 != 0u)
        return failed(&report_values, JE_SVC_FAIL_READINESS);
    report.seq = 0u;
    publish(JE_SVC_PHASE_BOOT, JE_SVC_FAIL_NONE, &report_values);
    /* The seeded launcher occupies slot 0. Mint SELF before any endpoint so
     * this scenario checks the exact slot-2/gen-1 handle it relies on. */
    if (je_handle_self(&supervisor_self) != JE_ABI_OK
        || supervisor_self != UINT64_C(0x00010002))
        return failed(&report_values, JE_SVC_FAIL_READINESS);
    if (je_endpoint_create(&self_packed) != JE_ABI_OK)
        return failed(&report_values, JE_SVC_FAIL_SLEEP_CHANNEL);
    self_recv = (uint32_t)self_packed;
#if JE_TEST_MODE == 13
    return broker_mode13(h0, self_recv, &report_values);
#elif JE_TEST_MODE == 14
    return device_mode14(h0, h1, self_recv, &report_values);
#elif JE_TEST_MODE == 15
    return uart_mode15(h0, h1, self_recv, &report_values);
#endif
    if (!spawn_one(h0, JE_IMAGE_RESTART_SVC, 0u,
                   JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV,
                   &restart_proc, &restart_ep))
        return failed(&report_values, JE_SVC_FAIL_RESTART);
    if (!spawn_one(h0, JE_IMAGE_SURVIVOR, 0u, JE_ABI_RIGHT_SEND,
                   &survivor_proc, &survivor_ep))
        return failed(&report_values, JE_SVC_FAIL_SURVIVOR);

    restart_config.op = JE_SVC_OP_CONFIG;
    restart_config.incarnation = incarnation;
    restart_config.crash_before = JE_SVC_RESTART_CRASH_BEFORE;
    restart_config.reserved = 0u;
    if (je_try_send(restart_ep, &restart_config, sizeof restart_config) != JE_ABI_OK)
        return failed(&report_values, JE_SVC_FAIL_RESTART);
    {
        uint32_t wait = wait_ready(survivor_ep, survivor_proc, self_recv,
                                   JE_SVC_READY_DEADLINE);
        if (wait != WAIT_OK)
            return failed(&report_values, wait == WAIT_SLEEP_CHANNEL
                          ? JE_SVC_FAIL_SLEEP_CHANNEL
                          : JE_SVC_FAIL_SURVIVOR);
    }
    {
        uint32_t wait = wait_restart_window(restart_proc, survivor_proc,
                                            survivor_ep, self_recv,
                                            &report_values);
        if (wait != WAIT_OK)
            return failed(&report_values, wait == WAIT_SLEEP_CHANNEL
                          ? JE_SVC_FAIL_SLEEP_CHANNEL
                          : JE_SVC_FAIL_SURVIVOR);
    }
    (void)je_proc_kill(restart_proc);
    (void)je_handle_close(restart_proc);
    (void)je_handle_close(restart_ep);
    ++incarnation;
    ++report_values.restarts;
    if (!spawn_one(h0, JE_IMAGE_RESTART_SVC, 0u,
                   JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV,
                   &restart_proc, &restart_ep))
        return failed(&report_values, JE_SVC_FAIL_RESTART);
    restart_config.incarnation = incarnation;
    if (je_try_send(restart_ep, &restart_config, sizeof restart_config) != JE_ABI_OK)
        return failed(&report_values, JE_SVC_FAIL_RESTART);
    {
        uint32_t wait = wait_ready(restart_ep, restart_proc, self_recv,
                                   JE_SVC_READY_DEADLINE);
        if (wait != WAIT_OK)
            return failed(&report_values, wait == WAIT_SLEEP_CHANNEL
                          ? JE_SVC_FAIL_SLEEP_CHANNEL
                          : JE_SVC_FAIL_RESTART);
    }
    publish(JE_SVC_PHASE_READY, JE_SVC_FAIL_NONE, &report_values);
#if JE_TEST_MODE == 12
    if (rollback_go == JE_SVC_LOSS_GO) {
        /* Give slot 0 one scheduler observation of READY before the deliberate
         * fault; the loss harness must prove that readiness was reached. */
        if (je_yield() != JE_ABI_OK)
            return failed(&report_values, JE_SVC_FAIL_READINESS);
        __asm__ volatile("ud2");
    }
#endif
    publish(JE_SVC_PHASE_RUNNING, JE_SVC_FAIL_NONE, &report_values);

    if (!spawn_one(h0, JE_IMAGE_SPOOFER, 0u, JE_ABI_RIGHT_SEND,
                   &spoofer_proc, &spoofer_ep))
        return failed(&report_values, JE_SVC_FAIL_READINESS);
    {
        uint32_t wait = wait_dead(spoofer_proc, self_recv, JE_SVC_READY_DEADLINE);
        if (wait != WAIT_OK)
            return failed(&report_values, wait == WAIT_SLEEP_CHANNEL
                          ? JE_SVC_FAIL_SLEEP_CHANNEL
                          : JE_SVC_FAIL_READINESS);
    }
    {
        je_abi_proc_status spoofer_status = {0};
        if (je_proc_status(spoofer_proc, &spoofer_status) != JE_ABI_OK
            || (spoofer_status.state != JE_ABI_PROC_DYING
                && spoofer_status.state != JE_ABI_PROC_DEAD)
            || spoofer_status.reason != JE_ABI_DEATH_EXIT)
            return failed(&report_values, JE_SVC_FAIL_READINESS);
        report_values.spoofer_bits =
            (uint32_t)spoofer_status.code & UINT32_C(0xffff);
    }
    (void)je_proc_kill(spoofer_proc);
    (void)je_handle_close(spoofer_ep);
    (void)je_handle_close(spoofer_proc);

    for (uint32_t attempt = 0u; attempt <= JE_SVC_CRASHLOOP_RESTART_LIMIT; ++attempt) {
        uint64_t crash_proc = 0u, crash_ep = 0u;
        ++report_values.crash_attempts;
        if (!spawn_one(h0, JE_IMAGE_CRASHLOOP, 0u, JE_ABI_RIGHT_SEND,
                       &crash_proc, &crash_ep))
            return failed(&report_values, JE_SVC_FAIL_CRASHLOOP);
        {
            uint32_t wait = wait_dead(crash_proc, self_recv,
                                      JE_SVC_READY_DEADLINE);
            if (wait != WAIT_OK)
                return failed(&report_values, wait == WAIT_SLEEP_CHANNEL
                              ? JE_SVC_FAIL_SLEEP_CHANNEL
                              : JE_SVC_FAIL_CRASHLOOP);
        }
        (void)je_proc_kill(crash_proc);
        (void)je_handle_close(crash_ep);
        (void)je_handle_close(crash_proc);
    }
    if (report_values.crash_attempts != JE_SVC_CRASHLOOP_RESTART_LIMIT + 1u)
        return failed(&report_values, JE_SVC_FAIL_CRASHLOOP);

    publish(JE_SVC_PHASE_ARM, JE_SVC_FAIL_NONE, &report_values);
    uint64_t arm_start = 0u;
    if (je_clock_ticks(&arm_start) != JE_ABI_OK)
        return failed(&report_values, JE_SVC_FAIL_ROLLBACK);
    for (;;) {
        if (rollback_go == JE_SVC_ROLLBACK_GO) break;
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - arm_start >= JE_SVC_SCENARIO_DEADLINE)
            return failed(&report_values, JE_SVC_FAIL_ROLLBACK);
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT)
            return failed(&report_values, JE_SVC_FAIL_SLEEP_CHANNEL);
    }
    config = 0u;
    status = je_proc_spawn(h0, JE_IMAGE_C_HELLO, (const void *)0, 0u,
                           JE_ABI_SPAWN_QUOTAS(15u, 0u), &config);
    if (status != JE_ABI_H_NOSLOTS || config != 0u)
        return failed(&report_values, JE_SVC_FAIL_ROLLBACK);
    (void)je_handle_close(self_recv);
    (void)je_handle_close((uint32_t)(self_packed >> 32));
    (void)je_handle_close(supervisor_self);
    (void)je_handle_close(h0);
    publish(JE_SVC_PHASE_DONE, JE_SVC_FAIL_NONE, &report_values);
    return (int64_t)JE_SVC_DONE_EXIT;
}
