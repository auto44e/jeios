/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/features.h>
je_status je_features_select(je_feature_inputs in, uint64_t required,
                             uint64_t *enabled)
{
    if (enabled == 0) return JE_ERR_INVALID;
    *enabled = 0; /* No partial result on failure. */
    uint64_t usable = in.hardware & in.os_ready & in.policy_allowed;
    if ((usable & required) != required) return JE_ERR_UNSUPPORTED;
    *enabled = usable;
    return JE_OK;
}

