#include "libpieg.h"

/*
 * mouse - checks the mouse or touchpad. Prints what was found, then the
 * pointer position and buttons while you move it. q or Esc quits.
 */

static int dist(int a, int b)
{
    return a > b ? a - b : b - a;
}

static void report(int x, int y, int buttons)
{
    p_puts("x=");
    p_u64((uint64_t)x);
    p_puts(" y=");
    p_u64((uint64_t)y);
    p_puts("  ");

    if (!buttons)
        p_puts("no buttons");
    if (buttons & 1)
        p_puts("left ");
    if (buttons & 2)
        p_puts("right ");
    if (buttons & 4)
        p_puts("middle");

    p_putc('\n');
}

int main(int argc, char **argv)
{
    int x = 0, y = 0, buttons = 0;
    int last_x = -1000, last_y = -1000, last_buttons = -1;
    int found;

    (void)argc;
    (void)argv;

    if (pieg->version < 9 || !pieg->mouse_state) {
        p_putln("mouse: needs a newer kernel (API 9)");
        return 1;
    }

    found = pieg->mouse_state(&x, &y, &buttons);

    if (!found) {
        p_putln("No mouse or touchpad found.");
        p_putln("Supported: PS/2 mice and touchpads, and USB 2.0 (EHCI)");
        p_putln("high-speed mice on a root port.");
        return 1;
    }

    p_puts("Found:");

    if (found & 1)
        p_puts(" PS/2 mouse or touchpad");
    if (found & 2)
        p_puts(" USB mouse");

    p_putc('\n');
    p_putln("Move it and press buttons. q or Esc quits.");

    for (;;) {
        char c = pieg->getchar();

        if (c == 'q' || c == 0x1B)
            break;

        pieg->mouse_state(&x, &y, &buttons);

        if (dist(x, last_x) >= 24 || dist(y, last_y) >= 24 ||
            buttons != last_buttons) {
            report(x, y, buttons);
            last_x = x;
            last_y = y;
            last_buttons = buttons;
        }
    }

    return 0;
}
