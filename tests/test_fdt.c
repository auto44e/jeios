/* SPDX-License-Identifier: Apache-2.0 */
/* PRE-WIRING PLACEHOLDER (issue #52): the hosted FDT parser suite is written by
 * a separate worker against include/jeios/fdt.h. Registered in `make check` now
 * so the target exists before either side lands. */
#include <jeios/fdt.h>
#include <stdio.h>

int main(void)
{
    puts("fdt: PLACEHOLDER (suite not yet written)");
    return 0;
}
