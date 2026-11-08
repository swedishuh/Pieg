#ifndef PIEG_USB_H
#define PIEG_USB_H

#include <stdint.h>

#include "blockdev.h"

#define USB_MAX_DISKS 4

/*
 * Minimal USB 2.0 stack: EHCI host controller (PCI class 0C:03:20) plus
 * USB mass storage (Bulk-Only Transport, SCSI transparent command set).
 * Polled, one transfer at a time, high-speed devices on root ports only
 * (no external hubs, no low/full-speed devices, no xHCI/USB3 controllers).
 * Besides mass storage it drives one HID boot-protocol mouse. A low- or
 * full-speed mouse (most of them) is handed to the companion controller and
 * is not seen; that needs a UHCI/OHCI driver or hub support.
 *
 * Disks are NOT registered with blockdev: fsinit does that. Disk i is
 * named "usb<i>"; 512-byte sectors only.
 */
unsigned int usb_init(void);
unsigned int usb_disk_count(void);
blockdev_t *usb_disk_get(unsigned int index);

/* The HID boot mouse, if one was found. usb_mouse_poll() takes about 1 ms. */
int usb_mouse_present(void);
int usb_mouse_poll(uint8_t *report, unsigned int *len);

#endif
