/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/user.h>
#include <jeios/svc.h>

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

static void publish(uint32_t phase, uint32_t fail_code)
{
    uint32_t seq = report.seq;
    if ((seq & 1u) != 0u) ++seq;
    report.seq = seq + 1u;
    report.fail_code = fail_code;
    report.restarts = report.restarts;
    report.heartbeats = report.heartbeats;
    report.spoofer_bits = report.spoofer_bits;
    report.crash_attempts = report.crash_attempts;
    compiler_barrier();
    report.phase = phase;
    compiler_barrier();
    report.seq = seq + 2u;
}

static uint64_t sleep_tick(uint64_t self_recv)
{
    uint64_t marker = 0u;
    uint64_t status = je_try_recv(self_recv, &marker, sizeof marker, 1u, &marker);
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

static uint32_t wait_ready(uint64_t endpoint, uint64_t process,
                           uint64_t self_recv, uint64_t deadline)
{
    uint64_t start = 0u;
    uint64_t now = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        uint32_t ready = ready_message(endpoint);
        if (ready != 0u) return ready == 1u;
        je_abi_proc_status state = {0};
        if (status_of(process, &state) != JE_ABI_OK
            || state.state != JE_ABI_PROC_LIVE)
            return 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK || now - start >= deadline)
            return 0u;
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT) return 0u;
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
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        je_abi_proc_status state = {0};
        if (process_dead(process, &state)) return 1u;
        if (je_clock_ticks(&now) != JE_ABI_OK || now - start >= deadline)
            return 0u;
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT) return 0u;
    }
}

static uint32_t wait_restart_window(uint64_t restart_process, uint64_t survivor_endpoint,
                                    uint64_t self_recv)
{
    uint64_t start = 0u;
    uint64_t now = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        je_abi_proc_status state = {0};
        je_svc_heartbeat heartbeat = {0};
        uint64_t meta = 0u;
        uint64_t status = je_try_recv(survivor_endpoint, &heartbeat,
                                      sizeof heartbeat, 0u, &meta);
        if (status == JE_ABI_OK
            && (meta & UINT64_C(0xffffffff)) == JE_SVC_HEARTBEAT_LEN
            && (meta >> 32) == 0u
            && heartbeat.op == JE_SVC_OP_HEARTBEAT)
            ++report.heartbeats;
        else if (status != JE_ABI_H_EMPTY && status != JE_ABI_H_PEER_CLOSED)
            return 0u;
        if (process_dead(restart_process, &state)) return 1u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_SCENARIO_DEADLINE)
            return 0u;
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT) return 0u;
    }
}

static int64_t failed(uint32_t code)
{
    publish(JE_SVC_PHASE_FAILED, code);
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
    uint64_t config;
    uint32_t incarnation = 0u;
    je_svc_config restart_config = {0};
    uint64_t status;

    (void)report_padding;
    (void)supervisor_rodata;
    if (version != JE_ABI_STARTUP_V1 || n != 1u
        || h1 != 0u || h2 != 0u || h3 != 0u)
        return failed(JE_SVC_FAIL_READINESS);
    report.magic = JE_SVC_REPORT_MAGIC;
    report.version = JE_SVC_REPORT_V0;
    report.seq = 0u;
    report.reserved = 0u;
    report.restarts = 0u;
    report.heartbeats = 0u;
    report.spoofer_bits = 0u;
    report.crash_attempts = 0u;
    publish(JE_SVC_PHASE_BOOT, JE_SVC_FAIL_NONE);
    if (je_endpoint_create(&self_packed) != JE_ABI_OK)
        return failed(JE_SVC_FAIL_SLEEP_CHANNEL);
    self_recv = (uint32_t)self_packed;
    if (!spawn_one(h0, JE_IMAGE_RESTART_SVC, 0u,
                   JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV,
                   &restart_proc, &restart_ep))
        return failed(JE_SVC_FAIL_RESTART);
    if (!spawn_one(h0, JE_IMAGE_SURVIVOR, 0u, JE_ABI_RIGHT_SEND,
                   &survivor_proc, &survivor_ep))
        return failed(JE_SVC_FAIL_SURVIVOR);

    restart_config.op = JE_SVC_OP_CONFIG;
    restart_config.incarnation = incarnation;
    restart_config.crash_before = JE_SVC_RESTART_CRASH_BEFORE;
    restart_config.reserved = 0u;
    if (je_try_send(restart_ep, &restart_config, sizeof restart_config) != JE_ABI_OK)
        return failed(JE_SVC_FAIL_RESTART);
    if (!wait_ready(survivor_ep, survivor_proc, self_recv, JE_SVC_READY_DEADLINE))
        return failed(JE_SVC_FAIL_SURVIVOR);
    if (!wait_restart_window(restart_proc, survivor_ep, self_recv))
        return failed(JE_SVC_FAIL_RESTART);
    (void)je_proc_kill(restart_proc);
    (void)je_handle_close(restart_proc);
    (void)je_handle_close(restart_ep);
    ++incarnation;
    ++report.restarts;
    if (!spawn_one(h0, JE_IMAGE_RESTART_SVC, 0u,
                   JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV,
                   &restart_proc, &restart_ep))
        return failed(JE_SVC_FAIL_RESTART);
    restart_config.incarnation = incarnation;
    if (je_try_send(restart_ep, &restart_config, sizeof restart_config) != JE_ABI_OK
        || !wait_ready(restart_ep, restart_proc, self_recv, JE_SVC_READY_DEADLINE))
        return failed(JE_SVC_FAIL_RESTART);
    publish(JE_SVC_PHASE_READY, JE_SVC_FAIL_NONE);
#if JE_TEST_MODE == 12
    if (rollback_go == JE_SVC_LOSS_GO) __asm__ volatile("ud2");
#endif
    publish(JE_SVC_PHASE_RUNNING, JE_SVC_FAIL_NONE);

    if (!spawn_one(h0, JE_IMAGE_SPOOFER, 0u, JE_ABI_RIGHT_SEND,
                   &spoofer_proc, &spoofer_ep))
        return failed(JE_SVC_FAIL_READINESS);
    if (!wait_dead(spoofer_proc, self_recv, JE_SVC_READY_DEADLINE))
        return failed(JE_SVC_FAIL_READINESS);
    {
        je_abi_proc_status spoofer_status = {0};
        if (je_proc_status(spoofer_proc, &spoofer_status) != JE_ABI_OK
            || spoofer_status.state != JE_ABI_PROC_DYING
            || spoofer_status.reason != JE_ABI_DEATH_EXIT)
            return failed(JE_SVC_FAIL_READINESS);
        report.spoofer_bits = (uint32_t)spoofer_status.code & UINT32_C(0xffff);
    }
    (void)je_proc_kill(spoofer_proc);
    (void)je_handle_close(spoofer_ep);
    (void)je_handle_close(spoofer_proc);

    for (uint32_t attempt = 0u; attempt <= JE_SVC_CRASHLOOP_RESTART_LIMIT; ++attempt) {
        uint64_t crash_proc = 0u, crash_ep = 0u;
        ++report.crash_attempts;
        if (!spawn_one(h0, JE_IMAGE_CRASHLOOP, 0u, JE_ABI_RIGHT_SEND,
                       &crash_proc, &crash_ep))
            return failed(JE_SVC_FAIL_CRASHLOOP);
        if (!wait_dead(crash_proc, self_recv, JE_SVC_READY_DEADLINE))
            return failed(JE_SVC_FAIL_CRASHLOOP);
        (void)je_proc_kill(crash_proc);
        (void)je_handle_close(crash_ep);
        (void)je_handle_close(crash_proc);
    }
    if (report.crash_attempts != JE_SVC_CRASHLOOP_RESTART_LIMIT + 1u)
        return failed(JE_SVC_FAIL_CRASHLOOP);

    publish(JE_SVC_PHASE_ARM, JE_SVC_FAIL_NONE);
    for (;;) {
        if (rollback_go == JE_SVC_ROLLBACK_GO) break;
        if (sleep_tick(self_recv) != JE_ABI_H_TIMED_OUT)
            return failed(JE_SVC_FAIL_SLEEP_CHANNEL);
    }
    config = 0u;
    status = je_proc_spawn(h0, JE_IMAGE_C_HELLO, (const void *)0, 0u,
                           JE_ABI_SPAWN_QUOTAS(15u, 0u), &config);
    if (status != JE_ABI_H_NOSLOTS || config != 0u)
        return failed(JE_SVC_FAIL_ROLLBACK);
    (void)je_handle_close(restart_proc);
    (void)je_handle_close(restart_ep);
    (void)je_handle_close(survivor_proc);
    (void)je_handle_close(survivor_ep);
    (void)je_handle_close(self_recv);
    (void)je_handle_close((uint32_t)(self_packed >> 32));
    (void)je_handle_close(h0);
    publish(JE_SVC_PHASE_DONE, JE_SVC_FAIL_NONE);
    return 0;
}
