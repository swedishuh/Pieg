#ifndef PIEG_KBDLAYOUT_H
#define PIEG_KBDLAYOUT_H

#include <stddef.h>
#include <stdint.h>

#include "api.h"

/*
 * Keyboard layouts.
 *
 * A layout maps Set-1 scancodes (0x00-0x7F) to characters in three layers:
 * plain, Shift, and AltGr (right Alt). 0 means "no character". Only ASCII
 * 0x20-0x7E, '\b' and '\n' are accepted; the terminal and the font cannot
 * show anything else yet.
 *
 * Layout files are plain text (see Keyboard/README.md). A file only lists
 * the keys that differ from the built-in US layout.
 */

#define KBD_KEYS      128
#define KBD_FILE_MAX  8192
#define KBD_CODE_MAX  PIEG_KBD_CODE_MAX
#define KBD_NAME_MAX  PIEG_KBD_NAME_MAX

typedef struct {
    char code[KBD_CODE_MAX];            /* short name, from the file name */
    char name[KBD_NAME_MAX];            /* display name, from the file */
    char normal[KBD_KEYS];
    char shift[KBD_KEYS];
    char altgr[KBD_KEYS];
} kbd_layout_t;

/*
 * Error kinds. Results are (line << 8) | kind, so 0 is success and the
 * line number (1-based, 0 when not tied to a line) travels with the kind.
 */
#define KBD_OK          0
#define KBD_E_OPEN      1   /* file missing or unreadable */
#define KBD_E_SIZE      2   /* larger than KBD_FILE_MAX */
#define KBD_E_SYNTAX    3   /* line is not "name ..." or "SC normal shift [altgr]" */
#define KBD_E_SCANCODE  4   /* scancode is not 01-7F hex */
#define KBD_E_CHAR      5   /* character token is invalid or out of range */

#define KBD_ERR_KIND(r) ((r) & 0xFF)
#define KBD_ERR_LINE(r) ((r) >> 8)

/* The built-in layout, used until a file is loaded. */
extern const kbd_layout_t kbd_layout_us;

/*
 * Parses layout text into *out, starting from the US layout. out->code is
 * left empty and out->name is empty when the file has no "name" line.
 * Returns 0 or an error (see above); *out is only meaningful on success.
 */
int kbd_parse(const char *text, size_t length, kbd_layout_t *out);

#endif
