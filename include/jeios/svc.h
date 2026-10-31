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

/* ADR 0010 D2 broker bound, shared with the supervisor (moved from endpoint.h in PR-F). */

/* Built-in image ids (ADR 0010 D5/D10). Dense; ids are the PROC_SPAWN image_id. */
#define JE_IMAGE_SPAWN_CHILD 0u   /* M4c asm child (PR-C) */
#define JE_IMAGE_C_HELLO     1u   /* first ring-3 C image (PR-E) */
/* 2 = supervisor (PR-F) */

#endif /* JEIOS_SVC_H */
