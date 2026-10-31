/*
 * unlock.h -- Processing module interface.
 *
 * Defines a function-pointer-based plugin architecture for processing strategies.
 * Each module implements probe() to check compatibility and execute() to run.
 * Modules are registered at startup and selected based on device properties.
 */

#ifndef UNLOCK_H
#define UNLOCK_H

#include "device/device.h"

#define UNLOCK_MAX_MODULES  16
#define UNLOCK_NAME_LEN     64
#define UNLOCK_DESC_LEN     256

typedef struct unlock_module {
    char name[UNLOCK_NAME_LEN];
    char description[UNLOCK_DESC_LEN];

    /*
     * probe -- Check if this module can handle the given device.
     * Returns 1 if compatible, 0 if not, -1 on error.
     */
    int (*probe)(device_info_t *dev);

    /*
     * execute -- Run the processing on the given device.
     * Returns 0 on success, non-zero on failure.
     */
    int (*execute)(device_info_t *dev);
} unlock_module_t;

/*
 * unlock_register -- Register a processing module.
 * Returns 0 on success, -1 if the registry is full.
 */
int unlock_register(const unlock_module_t *mod);

/*
 * unlock_select -- Find the first registered module whose probe() returns 1.
 * Returns a pointer to the module, or NULL if none match.
 */
const unlock_module_t *unlock_select(device_info_t *dev);

/*
 * unlock_execute -- Run the given module's execute() function.
 * Returns the module's return code, or -1 if mod is NULL.
 */
int unlock_execute(const unlock_module_t *mod, device_info_t *dev);

/*
 * unlock_list -- Print all registered modules to stdout.
 */
void unlock_list(void);

/*
 * unlock_count -- Return the number of registered modules.
 */
int unlock_count(void);

#endif /* UNLOCK_H */
