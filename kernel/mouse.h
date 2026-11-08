#ifndef PIEG_MOUSE_H
#define PIEG_MOUSE_H

#define MOUSE_SRC_PS2 1         /* i8042 aux port: PS/2 mice and touchpads */
#define MOUSE_SRC_USB 2         /* USB HID boot mouse on an EHCI root port */

/*
 * Pointer input. mouse_init() looks for a PS/2 mouse or touchpad (touchpads
 * answer as a standard PS/2 mouse) and uses a USB mouse if usb_init() found
 * one. Everything is polled: keyboard_getchar() feeds PS/2 bytes in as they
 * arrive and calls mouse_poll() while idle.
 *
 * The pointer is drawn by the framebuffer code as a green ball with an X,
 * from the first movement on.
 */
void mouse_init(void);

/* Idle-time work: USB reports. Cheap, rate limited, safe to call often. */
void mouse_poll(void);

/* One byte from the i8042 aux port (status bit 5 set). */
void mouse_ps2_byte(unsigned char byte);

/* Drains pending PS/2 aux bytes without touching keyboard bytes. */
void mouse_ps2_drain(void);

/*
 * API call. Returns 0 when there is no pointing device, otherwise a mask of
 * MOUSE_SRC_*. x, y are screen pixels, buttons bit 0 left, 1 right, 2 middle.
 * Any pointer may be NULL.
 */
int mouse_state(int *x, int *y, int *buttons);

#endif
