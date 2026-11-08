#include "login.h"

extern void vga_putchar(char c);
extern void vga_write(const char *text);
extern void vga_clear(void);
extern char keyboard_getchar(void);
extern void terminal_run(void);

static login_mode_t mode = LOGIN_KERNEL;

login_mode_t login_mode(void) { return mode; }
const char *login_user(void) { return mode == LOGIN_KERNEL ? "kernel" : "default"; }

/* Waits for any key that produces a character, then starts the shell. */
static void login_once(void)
{
    vga_clear();
    vga_write("PIEG OS\n\n");
    vga_write("Press any key to continue...");

    while (!keyboard_getchar())
        ;

    vga_putchar('\n');
    mode = LOGIN_USER;
    terminal_run();
}

void login_run(void)
{
    for (;;)
        login_once();
}
