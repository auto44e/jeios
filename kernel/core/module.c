#include <jeios/module.h>
je_status je_modules_start(const je_module *m, size_t n, size_t *failed)
{
    if (failed == 0) return JE_ERR_INVALID;
    *failed = n;
    if (n != 0 && m == 0) return JE_ERR_INVALID;
    /* Validate ALL descriptors before producing any side effects. */
    for (size_t i = 0; i < n; ++i) {
        if (m[i].name == 0 || m[i].start == 0 || m[i].stop == 0) {
            *failed = i;
            return JE_ERR_INVALID;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        je_status s = m[i].start();
        if (s != JE_OK) {
            *failed = i;
            /* Reverse dependency order; failed entry cleans itself up. */
            for (size_t j = i; j != 0; --j) m[j - 1].stop();
            return s;
        }
    }
    return JE_OK;
}
