/*
 * unlock.c -- Module registry implementation.
 *
 * Static array of registered modules. Selection iterates the list and
 * returns the first module whose probe() returns 1.
 */

#include <string.h>
#include "unlock/unlock.h"
#include "util/log.h"

static unlock_module_t g_modules[UNLOCK_MAX_MODULES];
static int g_module_count = 0;

int unlock_register(const unlock_module_t *mod)
{
    if (!mod || !mod->probe || !mod->execute) {
        log_error("[module] Invalid module (missing callbacks)");
        return -1;
    }

    if (g_module_count >= UNLOCK_MAX_MODULES) {
        log_error("[module] Module registry full (%d max)", UNLOCK_MAX_MODULES);
        return -1;
    }

    memcpy(&g_modules[g_module_count], mod, sizeof(unlock_module_t));
    g_module_count++;

    log_info("[module] Registered module: %s", mod->name);
    return 0;
}

const unlock_module_t *unlock_select(device_info_t *dev)
{
    if (!dev) return NULL;

    log_debug("[module] Probing %d module(s) for device CPID=0x%04X",
              g_module_count, dev->cpid);
    for (int i = 0; i < g_module_count; i++) {
        int result;
        log_debug("[module] Probing module '%s'...", g_modules[i].name);
        result = g_modules[i].probe(dev);
        if (result == 1) {
            log_info("[module] Selected module: %s", g_modules[i].name);
            return &g_modules[i];
        }
        if (result < 0) {
            log_warn("[module] probe() failed for '%s'", g_modules[i].name);
        } else {
            log_debug("[module] Module '%s' not compatible", g_modules[i].name);
        }
    }

    return NULL;
}

int unlock_execute(const unlock_module_t *mod, device_info_t *dev)
{
    if (!mod) {
        log_error("[module] No module selected");
        return -1;
    }
    if (!dev) {
        log_error("[module] No device");
        return -1;
    }

    log_info("[module] Executing module: %s", mod->name);
    return mod->execute(dev);
}

void unlock_list(void)
{
    log_info("[module] %d registered module(s):", g_module_count);
    for (int i = 0; i < g_module_count; i++) {
        log_info("  [%d] %s -- %s", i, g_modules[i].name,
                 g_modules[i].description);
    }
}

int unlock_count(void)
{
    return g_module_count;
}
