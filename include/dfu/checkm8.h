/* checkm8.h -- checkm8 DFU handler for A5-A11 */

#ifndef CHECKM8_H
#define CHECKM8_H

#include "device/device.h"
#include "dfu/dfu_handler.h"

/*
 * checkm8_run -- Top-level entry point. Sets up context, runs all four
 * stages (reset, spray, setup, patch), verifies completion, cleans up.
 * Returns 0 on success, -1 on failure.
 */
int checkm8_run(device_info_t *dev);

/*
 * Individual processing stages. Each advances ctx->phase on success.
 * All return 0 on success, -1 on failure.
 */

/*
 * Stage 1: Reset DFU state machine to a known clean state.
 * Sends DFU_DNLOAD with suffix-length zeroes, polls through manifest
 * states, then resets to dfuIDLE.
 */
int checkm8_stage_reset(dfu_ctx_t *ctx);

/*
 * Stage 2: Setup -- prepare the DFU state. Sends an async DFU_DNLOAD
 * with DFU_MAX_TRANSFER_SZ bytes, then overwrite padding.
 */
int checkm8_stage_setup(dfu_ctx_t *ctx);

/*
 * Stage 3: Memory spray. Two paths based on chip config:
 *   Path A (large_leak > 0): stall-based GET_DESCRIPTOR spray.
 *   Path B (large_leak == 0): send/stall cycles for memory layout.
 */
int checkm8_stage_spray(dfu_ctx_t *ctx);

/*
 * Stage 4: Write controlled data, then send the payload
 * (chip-specific header + code) in DFU_MAX_TRANSFER_SZ chunks.
 * After completion the serial string contains the success marker.
 */
int checkm8_stage_patch(dfu_ctx_t *ctx);

/*
 * checkm8_verify -- Re-read the USB serial string descriptor and
 * check whether it contains the success marker.
 * Returns 1 if complete, 0 if not complete, -1 on read error.
 */
int checkm8_verify(device_info_t *dev);

#endif /* CHECKM8_H */
