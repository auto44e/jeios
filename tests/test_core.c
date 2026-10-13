/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <jeios/features.h>
#include <jeios/module.h>
static int trace[8];
static size_t used;
static je_status a_start(void) { trace[used++] = 1; return JE_OK; }
static je_status b_start(void) { trace[used++] = 2; return JE_OK; }
static je_status fail_start(void) { trace[used++] = 3; return JE_ERR_IO; }
static void a_stop(void) { trace[used++] = -1; }
static void b_stop(void) { trace[used++] = -2; }
static void fail_stop(void) { assert(0 && "failed init owns its partial cleanup"); }
int main(void)
{
    uint64_t enabled = UINT64_MAX;
    const je_feature_inputs in = {7, 3, 5};
    assert(je_features_select(in, 1, &enabled) == JE_OK && enabled == 1);
    assert(je_features_select(in, 2, &enabled) == JE_ERR_UNSUPPORTED && enabled == 0);
    assert(je_features_select(in, 0, 0) == JE_ERR_INVALID);
    assert(je_features_select((je_feature_inputs){0, 0, 0}, 0, &enabled) == JE_OK && enabled == 0);
    const je_module m[] = {{"a", a_start, a_stop}, {"b", b_start, b_stop},
                           {"fail", fail_start, fail_stop}};
    size_t failed;
    assert(je_modules_start(m, 3, &failed) == JE_ERR_IO && failed == 2);
    assert(used == 5 && trace[0] == 1 && trace[1] == 2 && trace[2] == 3 &&
           trace[3] == -2 && trace[4] == -1);
    used = 0;
    assert(je_modules_start(m, 2, &failed) == JE_OK && failed == 2 && used == 2);
    used = 0;
    const je_module bad[] = {{"a", a_start, a_stop}, {"bad", 0, b_stop}};
    assert(je_modules_start(bad, 2, &failed) == JE_ERR_INVALID && failed == 1 && used == 0);
    assert(je_modules_start(0, 1, &failed) == JE_ERR_INVALID);
    assert(je_modules_start(0, 0, &failed) == JE_OK && failed == 0);
    assert(je_modules_start(m, 3, 0) == JE_ERR_INVALID);
    assert(strcmp(je_status_name(JE_ERR_IO), "io-error") == 0);
    assert(strcmp(je_status_name((je_status)999), "unknown-status") == 0);
    puts("core tests: PASS (feature prerequisites, fail-closed selection, reverse rollback)");
    return 0;
}

