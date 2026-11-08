#include <stddef.h>
#include <stdint.h>

#include "mousecore.h"

void mouse_pointer_reset(mouse_pointer_t *p, int w, int h)
{
    p->w = w > 0 ? w : 640;
    p->h = h > 0 ? h : 480;
    p->x = p->w / 2;
    p->y = p->h / 2;
    p->buttons = 0;
}

int mouse_accel(int delta)
{
    int a = delta < 0 ? -delta : delta;
    int out = a;

    if (a > 2)
        out = a + ((a - 2) * a) / 16;

    if (out > 120)
        out = 120;

    return delta < 0 ? -out : out;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void mouse_pointer_move(mouse_pointer_t *p, int dx, int dy, int buttons)
{
    p->x = clamp(p->x + mouse_accel(dx), 0, p->w - 1);
    p->y = clamp(p->y + mouse_accel(dy), 0, p->h - 1);
    p->buttons = buttons & 7;
}

void ps2_decoder_reset(ps2_decoder_t *d, int wheel)
{
    d->n = 0;
    d->wheel = wheel;
}

int ps2_decoder_feed(ps2_decoder_t *d, uint8_t byte, int *dx, int *dy,
                     int *buttons, int *wheel)
{
    unsigned int need = d->wheel ? 4u : 3u;

    /* Bit 3 of the first byte is always set; anything else is out of step. */
    if (d->n == 0 && !(byte & 0x08))
        return 0;

    d->buf[d->n++] = byte;

    if (d->n < need)
        return 0;

    d->n = 0;

    uint8_t flags = d->buf[0];

    /* Overflow in either axis: the deltas are meaningless, keep the buttons. */
    if (flags & 0xC0) {
        *dx = 0;
        *dy = 0;
    } else {
        *dx = (int)d->buf[1] - (int)((flags << 4) & 0x100);
        *dy = -((int)d->buf[2] - (int)((flags << 3) & 0x100));
    }

    *buttons = flags & 7;
    *wheel = 0;

    if (d->wheel) {
        int w = d->buf[3] & 0x0F;

        *wheel = w >= 8 ? w - 16 : w;
    }

    return 1;
}

int hid_boot_decode(const uint8_t *report, unsigned int len, int *dx, int *dy,
                    int *buttons)
{
    if (!report || len < 3)
        return 0;

    *buttons = report[0] & 7;
    *dx = (int8_t)report[1];
    *dy = (int8_t)report[2];
    return 1;
}
