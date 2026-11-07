#ifndef PIEG_USB_H
#define PIEG_USB_H

#include "blockdev.h"

#define USB_MAX_DISKS 4

/*
 * Minimal USB 2.0 stack: EHCI host controller (PCI class 0C:03:20) plus
 * USB mass storage (Bulk-Only Transport, SCSI transparent command set).
 * Polled, one transfer at a time, high-speed devices on root ports only
 * (no external hubs, no low/full-speed devices, no xHCI/USB3 controllers).
 *
 * Disks are NOT registered with blockdev: fsinit does that. Disk i is
 * named "usb<i>"; 512-byte sectors only.
 */
unsigned int usb_init(void);
unsigned int usb_disk_count(void);
blockdev_t *usb_disk_get(unsigned int index);

#endif
