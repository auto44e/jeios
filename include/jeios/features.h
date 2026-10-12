#ifndef JEIOS_FEATURES_H
#define JEIOS_FEATURES_H
#include <stdint.h>
#include <jeios/status.h>
/* CPU reports are raw; usable means both hardware AND OS prerequisites hold. */
typedef struct {
    uint64_t hardware;
    uint64_t os_ready;
    uint64_t policy_allowed;
} je_feature_inputs;
/* Selection is pure and does not execute privileged instructions. */
je_status je_features_select(je_feature_inputs in, uint64_t required,
                             uint64_t *enabled);
#endif
