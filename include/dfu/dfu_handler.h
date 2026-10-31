/* dfu_handler.h -- Common types and lifecycle API */

#ifndef DFU_HANDLER_H
#define DFU_HANDLER_H

#include <stddef.h>
#include <stdint.h>

#include "device/device.h"
#include "device/chip_db.h"

/* Phase constants */
#define DFU_PHASE_INIT   0
#define DFU_PHASE_RESET  1
#define DFU_PHASE_SETUP  2
#define DFU_PHASE_SPRAY  3
#define DFU_PHASE_PATCH  4
#define DFU_PHASE_DONE   5

/*
 * dfu_ctx_t -- shared context for DFU processing.
 *
 * Holds the device, chip info, current phase, and the assembled
 * payload buffer.
 */
typedef struct {
    device_info_t       *dev;
    const chip_info_t   *chip;
    int                  phase;       /* DFU_PHASE_* */
    uint8_t             *payload_buf;
    size_t               payload_len;
    int                  skip_clrstatus; /* 1 = DFU already in dfuIDLE, skip CLR_STATUS in stage 3 */
} dfu_ctx_t;

/*
 * dfu_init -- Populate ctx from device info. Looks up chip_info via
 * cpid, verifies chip compatibility, allocates payload buffer.
 * Returns 0 on success, -1 on failure.
 */
int dfu_init(dfu_ctx_t *ctx, device_info_t *dev);

/*
 * dfu_deliver -- Execute the processing stages (reset, spray, setup, patch).
 * Returns 0 on success, -1 on failure.
 */
int dfu_deliver(dfu_ctx_t *ctx);

/*
 * dfu_cleanup -- Free payload buffer and zero the context.
 */
void dfu_cleanup(dfu_ctx_t *ctx);

#endif /* DFU_HANDLER_H */
