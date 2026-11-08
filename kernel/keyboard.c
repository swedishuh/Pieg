#include <stdbool.h>
#include <stdint.h>

#include "fat.h"
#include "fsinit.h"
#include "kbdlayout.h"

#include "mouse.h"

extern void framebuffer_flush(void);
extern void vga_write(const char *text);

#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64
#define STATUS_AUX_DATA      0x20   /* output buffer holds mouse data */

static int shift_pressed;
static int ctrl_pressed;
static int altgr_pressed;
static int extended_scancode;

#define KEY_SCROLL_UP   ((char)0x80)
#define KEY_SCROLL_DOWN ((char)0x81)
#define KEY_UP          ((char)0x82)    /* plain arrows, for programs */
#define KEY_DOWN        ((char)0x83)
#define KEY_ESC         ((char)0x1B)

/* The active layout: the built-in US one until a layout file is loaded. */
static kbd_layout_t loaded;
static const kbd_layout_t *active = &kbd_layout_us;

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0"
                      : "=a"(value) : "Nd"(port));
    return value;
}

void keyboard_init(void)
{
    shift_pressed = 0;
    ctrl_pressed = 0;
    altgr_pressed = 0;
    extended_scancode = 0;
}

char keyboard_getchar(void)
{
    uint8_t status = inb(KEYBOARD_STATUS_PORT);

    /* Idle: USB mouse, then put everything drawn so far on screen (cheap if
     * nothing is dirty; this is also what moves the mouse cursor). */
    if (!(status & 1)) {
        mouse_poll();
        framebuffer_flush();
        return 0;
    }

    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    /* The same port carries PS/2 mouse and touchpad bytes. */
    if (status & STATUS_AUX_DATA) {
        mouse_ps2_byte(scancode);
        return 0;
    }

    /* Set-1 extended-key prefix. */
    if (scancode == 0xE0) {
        extended_scancode = 1;
        return 0;
    }

    if (extended_scancode) {
        extended_scancode = 0;

        /* Right or left Ctrl uses the extended/non-extended 0x1D code. */
        if (scancode == 0x1D) {
            ctrl_pressed = 1;
            return 0;
        }
        if (scancode == 0x9D) {
            ctrl_pressed = 0;
            return 0;
        }

        /* Right Alt (AltGr) selects the third layer of the layout. */
        if (scancode == 0x38) {
            altgr_pressed = 1;
            return 0;
        }
        if (scancode == 0xB8) {
            altgr_pressed = 0;
            return 0;
        }

        /* Ctrl + Up/Down scrolls the terminal viewport. */
        if (!(scancode & 0x80) && ctrl_pressed) {
            if (scancode == 0x48)
                return KEY_SCROLL_UP;
            if (scancode == 0x50)
                return KEY_SCROLL_DOWN;
        }

        if (!(scancode & 0x80)) {
            if (scancode == 0x48)
                return KEY_UP;
            if (scancode == 0x50)
                return KEY_DOWN;
        }

        /* Ignore other extended keys. */
        return 0;
    }

    if (scancode == 0x1D) {
        ctrl_pressed = 1;
        return 0;
    }
    if (scancode == 0x9D) {
        ctrl_pressed = 0;
        return 0;
    }

    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
        return 0;
    }

    if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = 0;
        return 0;
    }

    /* Ignore key-release events. */
    if (scancode & 0x80)
        return 0;

    if (scancode >= 128)
        return 0;

    if (scancode == 0x01 && !ctrl_pressed)
        return KEY_ESC;

    /* Ctrl+letter produces the control code (Ctrl+C = 3, Ctrl+U = 21, ...). */
    if (ctrl_pressed) {
        char base = active->normal[scancode];

        if (base >= 'a' && base <= 'z')
            return (char)(base - 'a' + 1);

        return 0;
    }

    if (altgr_pressed && active->altgr[scancode])
        return active->altgr[scancode];

    return shift_pressed
        ? active->shift[scancode]
        : active->normal[scancode];
}

/* ================================================================
 * Layout loading
 * ================================================================ */

static char layout_file[KBD_FILE_MAX];

static char lower_char(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Sets code to the file's base name ("/INPUTDEV/KEYBOARD/DE.KBD" -> "de"). */
static void code_from_path(const char *path, char *code)
{
    const char *base = path;
    size_t n = 0;

    for (const char *p = path; *p; p++) {
        if (*p == '/')
            base = p + 1;
    }

    while (base[n] && base[n] != '.' && n + 1 < KBD_CODE_MAX) {
        code[n] = lower_char(base[n]);
        n++;
    }

    code[n] = '\0';
}

int keyboard_layout_load(const char *path)
{
    bool is_dir = false;
    uint32_t size = 0;
    uint32_t done = 0;
    kbd_layout_t parsed;

    if (fs_stat(path, &is_dir, &size) != FAT_OK || is_dir)
        return KBD_E_OPEN;

    if (size > sizeof(layout_file))
        return KBD_E_SIZE;

    if (size && (fs_read(path, 0, layout_file, size, &done) != FAT_OK ||
                 done != size))
        return KBD_E_OPEN;

    int r = kbd_parse(layout_file, size, &parsed);

    if (r)
        return r;

    code_from_path(path, parsed.code);

    if (!parsed.name[0]) {
        for (size_t i = 0; i < KBD_CODE_MAX; i++)
            parsed.name[i] = parsed.code[i];
    }

    /* Only a complete, valid layout ever replaces the active one. */
    loaded = parsed;
    active = &loaded;
    return 0;
}

int keyboard_layout_info(char *code, char *name)
{
    if (code) {
        for (size_t i = 0; i < KBD_CODE_MAX; i++)
            code[i] = active->code[i];
    }

    if (name) {
        for (size_t i = 0; i < KBD_NAME_MAX; i++)
            name[i] = active->name[i];
    }

    return 0;
}

/*
 * Called once after the filesystem is up. /INPUTDEV/KEYBOARD/KEYMAP.CFG holds
 * the name of the layout `keymap` last saved; anything unreadable or invalid
 * leaves the built-in US layout in place.
 */
#define KBD_DIR_PREFIX "/INPUTDEV/KEYBOARD/"
#define KBD_CONFIG     KBD_DIR_PREFIX "KEYMAP.CFG"

void keyboard_load_saved(void)
{
    char cfg[16];
    char path[sizeof(KBD_DIR_PREFIX) + 8 + 4 + 1];
    uint32_t done = 0;
    size_t n = 0;
    size_t o = 0;

    if (!fs_is_mounted())
        return;

    if (fs_read(KBD_CONFIG, 0, cfg, sizeof(cfg), &done) != FAT_OK || !done)
        return;

    /* The name is at most 8 letters, digits, '_' or '-'; stop at anything else. */
    while (n < done && n < 8) {
        char c = cfg[n];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            break;

        n++;
    }

    if (!n)
        return;

    for (size_t i = 0; KBD_DIR_PREFIX[i]; i++)
        path[o++] = KBD_DIR_PREFIX[i];

    for (size_t i = 0; i < n; i++)
        path[o++] = cfg[i];

    path[o++] = '.';
    path[o++] = 'K';
    path[o++] = 'B';
    path[o++] = 'D';
    path[o] = '\0';

    if (keyboard_layout_load(path) == 0) {
        vga_write("[KBD] layout: ");
        vga_write(active->name);
        vga_write("\n");
    }
}
