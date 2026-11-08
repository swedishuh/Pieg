#include <stdbool.h>
#include <stdint.h>

#include "framebuffer.h"
#include "gfx.h"

extern void vga_redraw(void);
extern const uint8_t *vga_glyph(char c);

static bool active;

int gfx_begin(uint32_t *width, uint32_t *height)
{
    if (!framebuffer_is_available())
        return -1;

    if (width)
        *width = framebuffer_width();
    if (height)
        *height = framebuffer_height();

    active = true;
    framebuffer_clear(0);
    framebuffer_flush();
    return 0;
}

void gfx_end(void)
{
    if (!active)
        return;

    active = false;
    vga_redraw();
    framebuffer_flush();
}

void gfx_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    if (active)
        framebuffer_fill_rect(x, y, w, h, rgb);
}

void gfx_text(uint32_t x, uint32_t y, const char *text, uint32_t rgb,
              uint32_t scale)
{
    if (!active || !text)
        return;

    if (scale < 1)
        scale = 1;
    if (scale > 8)
        scale = 8;

    for (; *text; text++, x += 6 * scale) {
        const uint8_t *g = vga_glyph(*text);

        if (!g)
            continue;

        for (uint32_t col = 0; col < 5; col++) {
            uint32_t row = 0;

            /* Bit 0 is the top row; merge vertical runs into one rect. */
            while (row < 7) {
                if (!(g[col] & (1u << row))) {
                    row++;
                    continue;
                }

                uint32_t start = row;

                while (row < 7 && (g[col] & (1u << row)))
                    row++;

                framebuffer_fill_rect(x + col * scale, y + start * scale,
                                      scale, (row - start) * scale, rgb);
            }
        }
    }
}

void gfx_present(void)
{
    framebuffer_flush();
}

/*
 * Time comes from PIT channel 0, free-running as a 16-bit down counter at
 * 1.19318 MHz. Each call adds the ticks since the previous one, so it must
 * be called at least every ~50 ms (the counter wraps); animation loops do.
 */
static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint16_t last_count;
static uint64_t ticks;
static bool pit_ready;

uint32_t gfx_ms(void)
{
    if (!pit_ready) {
        outb(0x43, 0x34);               /* ch0, lobyte/hibyte, mode 2 */
        outb(0x40, 0);
        outb(0x40, 0);                  /* reload 65536 */
        pit_ready = true;
        last_count = 0;
    }

    outb(0x43, 0x00);                   /* latch ch0 */

    uint16_t now = inb(0x40);

    now |= (uint16_t)((uint16_t)inb(0x40) << 8);
    ticks += (uint16_t)(last_count - now);      /* counts down */
    last_count = now;

    return (uint32_t)(ticks * 1000u / 1193182u);
}
