#ifndef PIEG_MOUSECORE_H
#define PIEG_MOUSECORE_H

#include <stdint.h>

/*
 * Hardware-free part of the pointer code: PS/2 packet decoding, USB HID boot
 * report decoding, acceleration and the pointer position. Kept apart from
 * mouse.c so tests/mousetest.c can run it on the host.
 */

#define MOUSE_LEFT   1
#define MOUSE_RIGHT  2
#define MOUSE_MIDDLE 4

typedef struct {
    int x, y;                   /* pixels, 0 .. w-1 / 0 .. h-1 */
    int w, h;
    int buttons;                /* MOUSE_* bits */
} mouse_pointer_t;

typedef struct {
    uint8_t buf[4];
    unsigned int n;
    int wheel;                  /* 1 = IntelliMouse, packets are 4 bytes */
} ps2_decoder_t;

void mouse_pointer_reset(mouse_pointer_t *p, int w, int h);

/* Applies a relative move (dy positive = down) with acceleration. */
void mouse_pointer_move(mouse_pointer_t *p, int dx, int dy, int buttons);

/* Pointer speed curve: slow moves stay exact, fast ones go further. */
int mouse_accel(int delta);

void ps2_decoder_reset(ps2_decoder_t *d, int wheel);

/*
 * Feeds one byte from the mouse. Returns 1 when it completed a packet and
 * fills dx, dy (dy positive = down), buttons and wheel; 0 otherwise. Bytes
 * that cannot start a packet are dropped, so the stream resynchronizes.
 */
int ps2_decoder_feed(ps2_decoder_t *d, uint8_t byte, int *dx, int *dy,
                     int *buttons, int *wheel);

/* USB HID boot-protocol mouse report: buttons, dx, dy[, wheel]. */
int hid_boot_decode(const uint8_t *report, unsigned int len, int *dx, int *dy,
                    int *buttons);

#endif
