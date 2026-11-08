#include <stdbool.h>
#include <stdint.h>

#include "framebuffer.h"
#include "gfx.h"
#include "mouse.h"
#include "mousecore.h"
#include "usb.h"

extern void vga_write(const char *text);

#define PS2_DATA   0x60
#define PS2_STATUS 0x64

#define ST_OUT_FULL 0x01
#define ST_IN_FULL  0x02
#define ST_AUX      0x20

static mouse_pointer_t pointer;
static ps2_decoder_t decoder;
static int sources;
static bool ps2_ok;
static bool usb_ok;
static bool seen;               /* the pointer has moved at least once */

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;

    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* A write to port 0x80 takes about a microsecond on real hardware. */
static void udelay(uint32_t us)
{
    while (us--)
        outb(0x80, 0);
}

/* ================================================================
 * i8042 aux port
 * ================================================================ */

static bool wait_input_ready(uint32_t us)
{
    while (us--) {
        if (!(inb(PS2_STATUS) & ST_IN_FULL))
            return true;
        udelay(1);
    }

    return false;
}

/* Reads one byte from the aux port; keyboard bytes in the way are dropped. */
static bool aux_read(uint8_t *out, uint32_t us)
{
    while (us--) {
        uint8_t st = inb(PS2_STATUS);

        if (st & ST_OUT_FULL) {
            uint8_t v = inb(PS2_DATA);

            if (st & ST_AUX) {
                *out = v;
                return true;
            }
        } else {
            udelay(1);
        }
    }

    return false;
}

/* Reads one byte whatever its source (controller replies, not mouse data). */
static bool read_any(uint8_t *out, uint32_t us)
{
    while (us--) {
        if (inb(PS2_STATUS) & ST_OUT_FULL) {
            *out = inb(PS2_DATA);
            return true;
        }

        udelay(1);
    }

    return false;
}

static bool controller_cmd(uint8_t cmd)
{
    if (!wait_input_ready(20000))
        return false;

    outb(PS2_STATUS, cmd);
    return true;
}

static bool controller_data(uint8_t data)
{
    if (!wait_input_ready(20000))
        return false;

    outb(PS2_DATA, data);
    return true;
}

/* Sends one byte to the mouse and waits for its ACK (0xFA). */
static bool mouse_byte(uint8_t b)
{
    for (unsigned int tries = 0; tries < 3; tries++) {
        uint8_t r;

        if (!controller_cmd(0xD4) || !controller_data(b))
            return false;

        if (!aux_read(&r, 100000))
            return false;

        if (r == 0xFA)
            return true;

        if (r != 0xFE)              /* 0xFE = resend; anything else is junk */
            return false;
    }

    return false;
}

static bool mouse_set_rate(uint8_t rate)
{
    return mouse_byte(0xF3) && mouse_byte(rate);
}

static void flush_output(void)
{
    for (unsigned int i = 0; i < 32 && (inb(PS2_STATUS) & ST_OUT_FULL); i++) {
        inb(PS2_DATA);
        udelay(5);
    }
}

static void ps2_init(void)
{
    uint8_t cfg, id;

    if (inb(PS2_STATUS) == 0xFF)    /* floating bus: no i8042 at all */
        return;

    flush_output();

    /* Enable the aux port and make sure its clock runs; keep IRQs off. */
    if (!controller_cmd(0xA8))
        return;

    if (!controller_cmd(0x20))
        return;

    if (!read_any(&cfg, 100000))
        return;

    cfg = (uint8_t)(cfg & ~0x22u);

    if (!controller_cmd(0x60) || !controller_data(cfg))
        return;

    flush_output();

    /* Reset: ACK, then self-test 0xAA and the device id 0x00. */
    if (!mouse_byte(0xFF))
        return;

    if (!aux_read(&id, 1000000) || id != 0xAA)
        return;

    if (!aux_read(&id, 100000))
        return;

    mouse_byte(0xF6);               /* defaults */

    /* The magic rate sequence 200, 100, 80 turns a wheel mouse into id 3. */
    int wheel = 0;

    if (mouse_set_rate(200) && mouse_set_rate(100) && mouse_set_rate(80) &&
        mouse_byte(0xF2) && aux_read(&id, 100000) && id == 3)
        wheel = 1;

    mouse_set_rate(100);

    if (!mouse_byte(0xF4))          /* start streaming */
        return;

    flush_output();
    ps2_decoder_reset(&decoder, wheel);
    ps2_ok = true;
    sources |= MOUSE_SRC_PS2;
    vga_write(wheel ? "[MOUSE] PS/2 mouse/touchpad (wheel)\n"
                    : "[MOUSE] PS/2 mouse/touchpad\n");
}

/* ================================================================
 * Shared sink
 * ================================================================ */

static void apply(int dx, int dy, int buttons)
{
    mouse_pointer_move(&pointer, dx, dy, buttons);
    seen = true;
    framebuffer_cursor_set(pointer.x, pointer.y, true);
}

void mouse_ps2_byte(unsigned char byte)
{
    int dx, dy, buttons, wheel;

    if (!ps2_ok)
        return;

    if (ps2_decoder_feed(&decoder, byte, &dx, &dy, &buttons, &wheel))
        apply(dx, dy, buttons);
}

void mouse_ps2_drain(void)
{
    if (!ps2_ok)
        return;

    /* Only aux bytes: a keyboard byte at the head is left for the keyboard. */
    for (unsigned int i = 0; i < 16; i++) {
        if ((inb(PS2_STATUS) & (ST_OUT_FULL | ST_AUX)) !=
            (ST_OUT_FULL | ST_AUX))
            break;

        mouse_ps2_byte(inb(PS2_DATA));
    }
}

void mouse_poll(void)
{
    static uint32_t last;
    uint8_t report[64];
    unsigned int len = 0;

    if (!usb_ok)
        return;

    uint32_t now = gfx_ms();

    if (now - last < 6)             /* a mouse reports every 8 ms or so */
        return;

    last = now;

    if (usb_mouse_poll(report, &len)) {
        int dx, dy, buttons;

        if (hid_boot_decode(report, len, &dx, &dy, &buttons))
            apply(dx, dy, buttons);
    }
}

int mouse_state(int *x, int *y, int *buttons)
{
    if (!sources)
        return 0;

    mouse_ps2_drain();
    mouse_poll();

    if (x)
        *x = pointer.x;
    if (y)
        *y = pointer.y;
    if (buttons)
        *buttons = pointer.buttons;

    return sources;
}

void mouse_init(void)
{
    mouse_pointer_reset(&pointer, (int)framebuffer_width(),
                        (int)framebuffer_height());
    ps2_init();

    if (usb_mouse_present()) {
        usb_ok = true;
        sources |= MOUSE_SRC_USB;
    }

    if (!sources)
        vga_write("[MOUSE] no mouse or touchpad found\n");
}
