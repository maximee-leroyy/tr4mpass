/* iokit_usb.c -- Direct IOKit USB backend for Apple Silicon EP0 protection */

#if defined(__APPLE__)

#include <string.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <libusb.h>

#include "dfu/iokit_usb.h"
#include "util/log.h"

static IOUSBDeviceInterface320 **g_device;
static CFRunLoopSourceRef       g_async_source;
static int                      g_active;

int iokit_usb_activate(void)
{
    CFMutableDictionaryRef matching;
    io_service_t serv;
    IOCFPlugInInterface **plugin;
    SInt32 score;
    IOReturn kr;
    int16_t vid = 0x05AC;
    int16_t pid = 0x1227;
    CFNumberRef cf_vid, cf_pid;

    if (g_active)
        return 0;

    matching = IOServiceMatching(kIOUSBDeviceClassName);
    if (!matching) {
        log_error("iokit_usb: IOServiceMatching failed");
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
        log_error("iokit_usb: DFU device not found (05ac:1227)");
        return -1;
    }

    kr = IOCreatePlugInInterfaceForService(serv, kIOUSBDeviceUserClientTypeID,
                                           kIOCFPlugInInterfaceID,
                                           &plugin, &score);
    IOObjectRelease(serv);
    if (kr != kIOReturnSuccess || !plugin) {
        log_error("iokit_usb: plugin interface failed: 0x%x", kr);
        return -1;
    }

    kr = (*plugin)->QueryInterface(plugin,
            CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID320),
            (LPVOID *)&g_device);
    IODestroyPlugInInterface(plugin);
    if (kr != kIOReturnSuccess || !g_device) {
        log_error("iokit_usb: device interface failed: 0x%x", kr);
        return -1;
    }

    /* Open the device with a short retry so the IOKit port can settle. */
    {
        int open_retries;
        int opened = 0;
        for (open_retries = 0; open_retries < 20; open_retries++) {
            kr = (*g_device)->USBDeviceOpen(g_device);
            if (kr == kIOReturnSuccess) {
                opened = 1;
                break;
            }
            log_debug("iokit_usb: USBDeviceOpen retry %d/20: 0x%x", open_retries + 1, kr);
            usleep(50000);
        }
        if (!opened) {
            log_error("iokit_usb: USBDeviceOpen failed: 0x%x", kr);
            (*g_device)->Release(g_device);
            g_device = NULL;
            return -1;
        }
    }

    kr = (*g_device)->SetConfiguration(g_device, 1);
    log_info("iokit_usb: SetConfiguration(1) ret=0x%x", kr);

    kr = (*g_device)->CreateDeviceAsyncEventSource(g_device, &g_async_source);
    if (kr == kIOReturnSuccess && g_async_source) {
        CFRunLoopAddSource(CFRunLoopGetCurrent(), g_async_source,
                           kCFRunLoopDefaultMode);
    }

    g_active = 1;
    log_info("iokit_usb: activated (direct IOKit backend, no libusb cancel)");
    return 0;
}

void iokit_usb_deactivate(void)
{
    if (!g_active)
        return;

    if (g_async_source) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), g_async_source,
                              kCFRunLoopDefaultMode);
        CFRelease(g_async_source);
        g_async_source = NULL;
    }
    if (g_device) {
        (*g_device)->USBDeviceClose(g_device);
        (*g_device)->Release(g_device);
        g_device = NULL;
    }
    g_active = 0;
    log_info("iokit_usb: deactivated");
}

int iokit_usb_is_active(void)
{
    return g_active;
}

int iokit_usb_ctrl_transfer(uint8_t bmRequestType, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex,
                            void *data, uint16_t wLength,
                            uint32_t timeout_ms)
{
    IOUSBDevRequestTO req;
    IOReturn kr;

    if (!g_active || !g_device)
        return LIBUSB_ERROR_NO_DEVICE;

    memset(&req, 0, sizeof(req));
    req.bmRequestType = bmRequestType;
    req.bRequest      = bRequest;
    req.wValue        = wValue;
    req.wIndex        = wIndex;
    req.wLength       = wLength;
    req.pData         = data;
    req.completionTimeout = timeout_ms;
    req.noDataTimeout     = timeout_ms;

    kr = (*g_device)->DeviceRequestTO(g_device, &req);

    if (kr == kIOReturnSuccess)
        return (int)req.wLenDone;

    if (kr == (IOReturn)0xe0004051 || kr == kIOReturnTimeout) {
        log_debug("iokit_usb: timeout (wLenDone=%u)", req.wLenDone);
        return LIBUSB_ERROR_TIMEOUT;
    }
    if (kr == kIOUSBPipeStalled) {
        log_debug("iokit_usb: STALL (wLenDone=%u)", req.wLenDone);
        return LIBUSB_ERROR_PIPE;
    }
    if (kr == kIOReturnAborted) {
        log_debug("iokit_usb: aborted (wLenDone=%u)", req.wLenDone);
        return LIBUSB_ERROR_INTERRUPTED;
    }

    /*
     * kIOReturnNoDevice: the IOService behind this handle died -- the
     * device re-enumerated (e.g. after a host-side abort glitched EP0).
     * Nothing was sent on the wire, so transparently find the new
     * service and retry ONCE.  If the device is physically gone,
     * activate() fails and we return NO_DEVICE.
     */
    if (kr == kIOReturnNoDevice) {
        log_info("iokit_usb: kIOReturnNoDevice -- stale IOService, "
                 "re-activating handle and retrying");
        iokit_usb_deactivate();
        usleep(100000); /* let re-enumeration settle */
        if (iokit_usb_activate() != 0)
            return LIBUSB_ERROR_NO_DEVICE;

        memset(&req, 0, sizeof(req));
        req.bmRequestType = bmRequestType;
        req.bRequest      = bRequest;
        req.wValue        = wValue;
        req.wIndex        = wIndex;
        req.wLength       = wLength;
        req.pData         = data;
        req.completionTimeout = timeout_ms;
        req.noDataTimeout     = timeout_ms;

        kr = (*g_device)->DeviceRequestTO(g_device, &req);
        if (kr == kIOReturnSuccess)
            return (int)req.wLenDone;
    }

    log_warn("iokit_usb: DeviceRequestTO failed: IOReturn=0x%x wLenDone=%u",
             kr, req.wLenDone);
    return LIBUSB_ERROR_OTHER;
}

int iokit_usb_abort_pipe_zero(void)
{
    IOReturn kr;
    if (!g_active || !g_device)
        return -1;
    kr = (*g_device)->USBDeviceAbortPipeZero(g_device);
    log_info("iokit_usb: AbortPipeZero ret=0x%x", kr);
    return (kr == kIOReturnSuccess) ? 0 : -1;
}

int iokit_usb_read_serial(uint8_t iserial_idx, char *out, size_t outsz)
{
    uint8_t desc[256];
    int ret;
    size_t i, j;

    if (!g_active || !g_device || iserial_idx == 0 || !out || outsz < 2)
        return -1;

    ret = iokit_usb_ctrl_transfer(0x80, 0x06,
                                  (uint16_t)(0x0300 | iserial_idx),
                                  0x0409, desc, sizeof(desc), 1000);
    if (ret < 2 || desc[1] != 0x03)
        return -1;

    /* USB string descriptors are UTF-16LE — extract ASCII. */
    for (i = 2, j = 0; i + 1 < (size_t)ret && j + 1 < outsz; i += 2) {
        if (desc[i + 1] == 0 && desc[i] >= 0x20 && desc[i] < 0x7F)
            out[j++] = (char)desc[i];
    }
    out[j] = '\0';
    return (int)j;
}

int iokit_usb_reset_device(void)
{
    if (!g_active || !g_device)
        return -1;
    (*g_device)->ResetDevice(g_device);
    return 0;
}

int iokit_usb_reenumerate(void)
{
    if (!g_active || !g_device)
        return -1;
    (*g_device)->USBDeviceReEnumerate(g_device, 0);
    return 0;
}

/*
 * Last-resort recovery: force the BootROM back to a fresh DFU entry.
 *
 * Needed when a stage-4 bail-out leaves EP0 halted (the overwrite STALL
 * halt was never cleared, or the DFU state machine is wedged): every
 * later control request -- including libusb GET_STATUS in stage 1 of the
 * NEXT run -- times out, and the device looks "found" but dead.
 *
 * A port-level ResetDevice + USBDeviceReEnumerate makes the device
 * re-enumerate into clean DFU.  Caller must have closed any libusb
 * handle first (exclusive access); if the IOKit handle is not open yet
 * we open and close it ourselves.
 */
int iokit_usb_recover_bus(void)
{
    int own = 0;

    if (!g_active) {
        if (iokit_usb_activate() != 0) {
            log_warn("iokit_usb: recover_bus -- could not open device");
            return -1;
        }
        own = 1;
    }

    log_info("iokit_usb: recovering device -- ResetDevice + ReEnumerate");
    (*g_device)->ResetDevice(g_device);
    usleep(200000);
    (*g_device)->USBDeviceReEnumerate(g_device, 0);
    usleep(500000);

    if (own)
        iokit_usb_deactivate();
    return 0;
}

#endif /* __APPLE__ */
