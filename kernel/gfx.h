#ifndef PIEG_GFX_H
#define PIEG_GFX_H

#include <stdint.h>

/*
 * Minimal graphics for programs. gfx_begin() takes the screen over from the
 * text console; gfx_end() gives it back and repaints the console. Colors are
 * 0xRRGGBB. All drawing is clipped. Nothing is visible until gfx_present().
 */

int  gfx_begin(uint32_t *width, uint32_t *height);  /* 0, or -1 without a framebuffer */
void gfx_end(void);
void gfx_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
void gfx_text(uint32_t x, uint32_t y, const char *text, uint32_t rgb,
              uint32_t scale);                      /* 5x7 font, 6*scale px per char */
void gfx_present(void);
uint32_t gfx_ms(void);                              /* milliseconds, wraps at 2^32 */

#endif
