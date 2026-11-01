/* iokit_usb.h -- Direct IOKit USB backend for Apple Silicon
 *
 * On Apple Silicon USB-C, libusb_cancel_transfer triggers AbortPipeZero
 * in the darwin backend, which produces kIOReturnNotResponding and
 * permanently corrupts the XHCI EP0 OUT transfer ring.  This makes
 * all subsequent EP0 OUT data-phase transfers (DFU_DNLOAD) fail with
 * STALL or timeout.
 *
 * This module provides an alternative USB backend using IOKit's
 * DeviceRequestTO directly.  IOKit's native timeout uses the XHCI
 * Stop Endpoint command, which properly cleans up the transfer ring
 * without corrupting EP0.
 *
 * Usage: call iokit_usb_activate() after closing any libusb handle on
 * the DFU device (exclusive access).  While active, all USB transfer
 * functions in usb_helpers.c and checkm8_stages.c route through IOKit
 * instead of libusb.
 */

#ifndef IOKIT_USB_H
#define IOKIT_USB_H

#if defined(__APPLE__)

#include <stdint.h>

int  iokit_usb_activate(void);
void iokit_usb_deactivate(void);
int  iokit_usb_is_active(void);

int  iokit_usb_ctrl_transfer(uint8_t bmRequestType, uint8_t bRequest,
                             uint16_t wValue, uint16_t wIndex,
                             void *data, uint16_t wLength,
                             uint32_t timeout_ms);

int  iokit_usb_abort_pipe_zero(void);
int  iokit_usb_read_serial(uint8_t iserial_idx, char *out, size_t outsz);
int  iokit_usb_reset_device(void);
int  iokit_usb_reenumerate(void);

#endif /* __APPLE__ */
#endif /* IOKIT_USB_H */
