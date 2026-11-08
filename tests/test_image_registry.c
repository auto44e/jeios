/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Fix-gate F10: hosted coverage of the built-in image registry
 * (kernel/core/image.c je_builtin_images_init / _get / _count), linked against
 * the REAL validator (kernel/core/vm.c) with FAKE linker symbols.
 *
 * image.c's inputs are const link-time symbols, so each failure case needs its
 * own link: this one TU is built once per case with -DJE_REG_CASE_<name>
 * (Makefile IMAGE_REG_CASES), mirroring the test_handle_rw pattern. Defining the
 * symbols with the exact types image.c declares (const, unsigned long len) keeps
 * the cross-TU types compatible (C11 6.2.7); nothing here mutates a const object.
 *
 * The spawn-child span is delimited by two linker symbols. The test reproduces
 * that shape with assembler `.set` aliases into one backing array, so the span
 * length is an INDEPENDENT oracle (the .set offset constants), not derived from
 * the pointer arithmetic under test.
 *
 * Cases:
 *   ok             well-formed registry: init OK, count/get return every entry,
 *                  spawn-child blob/len/desc match the span; idempotent re-init.
 *   span_equal     end == start (empty span)    -> JE_ERR_INVALID, nothing published.
 *   span_inverted  end <  start (inverted span) -> JE_ERR_INVALID, nothing published.
 *   len_mismatch   the LAST validated entry (broker-client) declares a blob
 *                  length shorter than its descriptor's source bytes -> init
 *                  fails AFTER every earlier entry was staged; the registry must
 *                  not be published (count 0, get NULL), and a retry still fails.
 *   report_missing the supervisor descriptor lacks the RW segment holding the
 *                  service report -> JE_ERR_INVALID (Stage 2 report hook).
 */
#include <jeios/image.h>
#include <jeios/svc.h>
#include <jeios/vm.h>
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#if defined(JE_REG_CASE_ok)
#define SPAN_START_OFF 0
#define SPAN_END_OFF   32
#elif defined(JE_REG_CASE_span_equal)
#define SPAN_START_OFF 16
#define SPAN_END_OFF   16
#elif defined(JE_REG_CASE_span_inverted)
#define SPAN_START_OFF 32
#define SPAN_END_OFF   0
#elif defined(JE_REG_CASE_len_mismatch) || defined(JE_REG_CASE_report_missing)
#define SPAN_START_OFF 0
#define SPAN_END_OFF   32
#else
#error "define exactly one JE_REG_CASE_<name>"
#endif

#define STR_(x) #x
#define STR(x) STR_(x)

/* Backing bytes for the fake spawn-child span. External linkage only so the
 * top-level asm below can name it. */
unsigned char je_test_span_backing[64];
__asm__(".globl je_user_spawn_child_start\n"
        ".set je_user_spawn_child_start, je_test_span_backing + " STR(SPAN_START_OFF) "\n"
        ".globl je_user_spawn_child_end\n"
        ".set je_user_spawn_child_end, je_test_span_backing + " STR(SPAN_END_OFF) "\n");
extern unsigned char je_user_spawn_child_start[];
extern unsigned char je_user_spawn_child_end[];

/* A generic valid descriptor: RX code page at arena offset 0 with BLOB_LEN
 * initialized bytes and entry 0, plus one RW page at the service-report vaddr so
 * the supervisor report hook is satisfied too. */
#define BLOB_LEN 16u
#define REPORT_OFF (JE_SVC_REPORT_VADDR - JE_VM_ARENA_BASE)
#define GOOD_DESC {                                                            \
    .version = JE_VM_IMAGE_VERSION, .segment_count = 2u, .entry_offset = 0u,  \
    .segments = {                                                              \
        { 0u, BLOB_LEN, 0u, JE_PAGE_SIZE, JE_VM_R | JE_VM_X, 0u },             \
        { 0u, 0u, REPORT_OFF, JE_PAGE_SIZE, JE_VM_R | JE_VM_W, 0u } } }
/* Code-only: valid for vm_image_validate, but no RW segment over the report. */
#define NO_REPORT_DESC {                                                       \
    .version = JE_VM_IMAGE_VERSION, .segment_count = 1u, .entry_offset = 0u,  \
    .segments = { { 0u, BLOB_LEN, 0u, JE_PAGE_SIZE, JE_VM_R | JE_VM_X, 0u } } }

#define FAKE_IMAGE(n, len_value, desc_init)                                    \
    const unsigned char je_img_##n##_blob[BLOB_LEN] = { 0xf4u };               \
    const unsigned long je_img_##n##_len = (len_value);                        \
    const je_vm_image je_img_##n##_desc = desc_init;

FAKE_IMAGE(c_hello, BLOB_LEN, GOOD_DESC)
#if defined(JE_REG_CASE_report_missing)
FAKE_IMAGE(supervisor, BLOB_LEN, NO_REPORT_DESC)
#else
FAKE_IMAGE(supervisor, BLOB_LEN, GOOD_DESC)
#endif
FAKE_IMAGE(restart, BLOB_LEN, GOOD_DESC)
FAKE_IMAGE(survivor, BLOB_LEN, GOOD_DESC)
FAKE_IMAGE(spoofer, BLOB_LEN, GOOD_DESC)
FAKE_IMAGE(crashloop, BLOB_LEN, GOOD_DESC)
FAKE_IMAGE(broker_service, BLOB_LEN, GOOD_DESC)
#if defined(JE_REG_CASE_len_mismatch)
FAKE_IMAGE(broker_client, BLOB_LEN - 1u, GOOD_DESC)   /* 1 byte short of source_size */
#else
FAKE_IMAGE(broker_client, BLOB_LEN, GOOD_DESC)
#endif
FAKE_IMAGE(uart_driver, BLOB_LEN, GOOD_DESC)
FAKE_IMAGE(uart_service, BLOB_LEN, GOOD_DESC)
FAKE_IMAGE(uart_client, BLOB_LEN, GOOD_DESC)

#define EXPECTED_COUNT 12u

static void assert_unpublished(void)
{
    assert(je_builtin_image_count() == 0u);
    for (uint32_t i = 0u; i <= EXPECTED_COUNT; ++i)
        assert(je_builtin_image_get(i) == NULL);
    assert(je_builtin_image_get(UINT32_MAX) == NULL);
}

#if defined(JE_REG_CASE_ok)
static void check_entry(uint32_t id, const char *name, const unsigned char *blob)
{
    const je_builtin_image *img = je_builtin_image_get(id);
    assert(img != NULL);
    assert(img->name != NULL && strcmp(img->name, name) == 0);
    assert(img->blob == blob);
    assert(je_vm_image_validate(&img->desc, img->blob_len) == JE_OK);
    if (id != JE_IMAGE_SPAWN_CHILD) {
        assert(img->blob_len == BLOB_LEN);
        assert(img->desc.segment_count == 2u);
        assert(img->desc.segments[0].source_size == BLOB_LEN);
    }
}

static void run_case(void)
{
    assert_unpublished();                       /* nothing before init */
    assert(je_builtin_images_init() == JE_OK);
    assert(je_builtin_image_count() == EXPECTED_COUNT);

    /* Spawn-child: blob is the start symbol, length is the .set oracle. */
    const je_builtin_image *sc = je_builtin_image_get(JE_IMAGE_SPAWN_CHILD);
    assert(sc != NULL);
    assert(sc->blob == je_test_span_backing + SPAN_START_OFF);
    assert(sc->blob_len == (size_t)(SPAN_END_OFF - SPAN_START_OFF));
    assert(sc->desc.version == JE_VM_IMAGE_VERSION);
    assert(sc->desc.segment_count == 2u && sc->desc.entry_offset == 0u);
    assert(sc->desc.segments[0].source_size == (uint64_t)(SPAN_END_OFF - SPAN_START_OFF));
    assert(sc->desc.segments[0].perm == (JE_VM_R | JE_VM_X));
    assert(sc->desc.segments[1].target_offset == JE_PAGE_SIZE);
    assert(sc->desc.segments[1].perm == (JE_VM_R | JE_VM_W));

    check_entry(JE_IMAGE_SPAWN_CHILD, "spawn-child", je_test_span_backing + SPAN_START_OFF);
    check_entry(JE_IMAGE_C_HELLO,     "c-hello",        je_img_c_hello_blob);
    check_entry(JE_IMAGE_SUPERVISOR,  "supervisor",     je_img_supervisor_blob);
    check_entry(JE_IMAGE_RESTART_SVC, "restart-svc",    je_img_restart_blob);
    check_entry(JE_IMAGE_SURVIVOR,    "survivor",       je_img_survivor_blob);
    check_entry(JE_IMAGE_SPOOFER,     "spoofer",        je_img_spoofer_blob);
    check_entry(JE_IMAGE_CRASHLOOP,   "crashloop",      je_img_crashloop_blob);
    check_entry(JE_IMAGE_BROKER_SVC,  "broker-service", je_img_broker_service_blob);
    check_entry(JE_IMAGE_BROKER_CLI,  "broker-client",  je_img_broker_client_blob);
    check_entry(JE_IMAGE_UART_DRIVER, "uart-driver",    je_img_uart_driver_blob);
    check_entry(JE_IMAGE_UART_SVC,    "uart-service",   je_img_uart_service_blob);
    check_entry(JE_IMAGE_UART_CLIENT, "uart-client",    je_img_uart_client_blob);
    assert(je_builtin_image_get(EXPECTED_COUNT) == NULL);
    assert(je_builtin_image_get(UINT32_MAX) == NULL);

    /* Idempotent: a second init is OK and changes nothing. */
    assert(je_builtin_images_init() == JE_OK);
    assert(je_builtin_image_count() == EXPECTED_COUNT);
    assert(je_builtin_image_get(JE_IMAGE_SPAWN_CHILD) == sc);
    assert(sc->blob_len == (size_t)(SPAN_END_OFF - SPAN_START_OFF));
}
#else
static void run_case(void)
{
    assert_unpublished();
    assert(je_builtin_images_init() == JE_ERR_INVALID);
    assert_unpublished();                       /* fail-closed: no partial publish */
    /* Not memoised as ready: a retry re-validates and still fails. */
    assert(je_builtin_images_init() == JE_ERR_INVALID);
    assert_unpublished();
}
#endif

int main(void)
{
    run_case();
    printf("test_image_registry (%s): ok\n",
#if defined(JE_REG_CASE_ok)
           "ok"
#elif defined(JE_REG_CASE_span_equal)
           "span_equal"
#elif defined(JE_REG_CASE_span_inverted)
           "span_inverted"
#elif defined(JE_REG_CASE_len_mismatch)
           "len_mismatch"
#else
           "report_missing"
#endif
           );
    return 0;
}
