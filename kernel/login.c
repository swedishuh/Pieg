#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "login.h"

extern void vga_putchar(char c);
extern void vga_write(const char *text);
extern void vga_clear(void);
extern void vga_backspace(void);
extern char keyboard_getchar(void);
extern void terminal_run(void);

static login_mode_t mode = LOGIN_KERNEL;

login_mode_t login_mode(void) { return mode; }
const char *login_user(void) { return mode == LOGIN_KERNEL ? "kernel" : "default"; }

static bool same(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a++ != *b++)
            return false;
    }

    return *a == *b;
}

static void read_line(char *dst, size_t cap, bool echo)
{
    size_t n = 0;

    for (;;) {
        char c = keyboard_getchar();
        unsigned char u = (unsigned char)c;

        if (!c)
            continue;

        if (c == '\n' || c == '\r') {
            vga_putchar('\n');
            break;
        }

        if (c == '\b' || u == 127) {
            if (n) {
                n--;
                if (echo)
                    vga_backspace();
            }
            continue;
        }

        if (u < 32 || u > 126 || n + 1 >= cap)
            continue;

        dst[n++] = c;

        if (echo)
            vga_putchar(c);
    }

    dst[n] = '\0';
}

static void login_once(void)
{
    char user[32];
    char pass[32];

    vga_clear();
    vga_write("PIEG OS\n\n");
    vga_write("  type  kernel   to boot kernel mode (ring 0, no protection)\n");
    vga_write("  type  default  with password  user  for the full userspace (ring 3)\n\n");

    for (;;) {
        vga_write("login: ");
        read_line(user, sizeof(user), true);

        if (same(user, "kernel")) {
            mode = LOGIN_KERNEL;
            break;
        }

        if (same(user, "default")) {
            vga_write("Password: ");
            read_line(pass, sizeof(pass), false);

            if (same(pass, "user")) {
                mode = LOGIN_USER;
                break;
            }
        }

        vga_write("Login incorrect\n\n");
    }

    terminal_run();
}

void login_run(void)
{
    for (;;)
        login_once();
}
