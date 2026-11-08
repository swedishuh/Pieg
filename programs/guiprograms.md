# GUI programs

PIEG has a small graphics API (API version 8). A GUI program is an ordinary
program in `programs/`: it takes the screen from the text console, draws
frames, and gives the screen back when it exits. See `programs/setup.c`
(the first-boot keyboard picker) for a full example.

## API

All calls go through the `pieg` pointer from `libpieg.h`. Colors are `0xRRGGBB`.

| Call | Does |
|------|------|
| `gfx_begin(&w, &h)` | Takes the screen, clears it to black, returns the size in pixels. Returns 0, or -1 when there is no framebuffer. |
| `gfx_end()` | Gives the screen back and repaints the text console. |
| `gfx_rect(x, y, w, h, rgb)` | Filled rectangle. Clipped to the screen. |
| `gfx_text(x, y, text, rgb, scale)` | 5x7 font, ASCII 32-126. Each character is `6*scale` pixels wide and `7*scale` tall. `scale` is 1-8. |
| `gfx_present()` | Shows everything drawn since the last call. Nothing is visible before this. |
| `gfx_ms()` | Millisecond clock. Wraps at 2^32. |

There is no pixel readback, no images and no per-pixel call. Build everything
from rectangles and text.

## Input

`getchar()` never blocks and returns 0 when no key is ready. Besides the
layout's characters it returns:

| Code | Key |
|------|-----|
| `0x82` | Up |
| `0x83` | Down |
| `0x1B` | Esc |
| `'\n'` | Enter |
| `'\b'` | Backspace |

Compare as `unsigned char`: plain `char` is signed and `c == 0x82` is never true.
There are no key-release events and no Left/Right yet.

## Timing

- Call `gfx_ms()` at least every 50 ms. It accumulates ticks from a 16-bit
  hardware counter, so a longer gap loses time. A frame loop does this
  naturally.
- Programs can't sleep. Poll `getchar()` and `gfx_ms()` in a loop and draw
  when enough time has passed (30 fps is `now - last >= 33`).
- Redrawing the whole screen every frame is fine; only the changed area is
  sent to the display.

## Rules

- Check `pieg->version >= 8` before using any `gfx_*` call.
- Always check the result of `gfx_begin()`. Fall back to text output when it
  fails.
- Call `gfx_end()` before returning. Without it the console is not repainted.
- Don't mix `putchar`/`write` with graphics between `gfx_begin()` and
  `gfx_end()`.
- Programs run in ring 0 (login `kernel`) or ring 3 (login `default`). The
  gfx calls work the same in both. Keep pointers you pass (like the `text`
  string) inside your own program.

## Minimal program

Save as `programs/demo.c`. `make` installs it as `/PROGRAMS/DEMO.ELF`; run it
with `demo`.

```c
#include "libpieg.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    uint32_t w, h;

    if (pieg->version < 8 || pieg->gfx_begin(&w, &h))
        return 1;

    uint32_t x = 0, last = pieg->gfx_ms();

    for (;;) {
        char c = pieg->getchar();
        uint32_t now = pieg->gfx_ms();

        if (c == 0x1B)
            break;

        if (now - last >= 33) {
            last = now;
            x = (x + 4) % (w - 40);

            pieg->gfx_rect(0, 0, w, h, 0x04070a);
            pieg->gfx_rect(x, h / 2, 40, 40, 0x39ff9a);
            pieg->gfx_text(8, 8, "esc to quit", 0x9fd8bf, 2);
            pieg->gfx_present();
        }
    }

    pieg->gfx_end();
    return 0;
}
```

## Style

The built-in look (setup) is near-black background `0x04070a`, panel
`0x070d12`, edge `0x1d4a38`, dim text `0x3b6b58`, text `0x9fd8bf`, accent
`0x39ff9a`. Pick the text scale from the screen height (`h / 300`, clamped to
1-4) so it reads on any resolution.
