#ifndef JEIOS_MODULE_H
#define JEIOS_MODULE_H
#include <stddef.h>
#include <jeios/status.h>
/* Static init sequence only. This is NOT a loader or an isolation boundary.
 * Entries are ordered by dependency. Init must undo its OWN partial work on
 * failure; stop undoes successful initialization and must not fail. */
typedef struct {
    const char *name;
    je_status (*start)(void);
    void (*stop)(void);
} je_module;
je_status je_modules_start(const je_module *modules, size_t count,
                           size_t *failed_index);
#endif
