#include "kbdlayout.h"

/* ================================================================
 * Built-in US layout (also the base every layout file starts from)
 * ================================================================ */

const kbd_layout_t kbd_layout_us = {
    .code = "us",
    .name = "US English (QWERTY)",
    .normal = {
        [0x29] = '`',
        [0x02] = '1', [0x03] = '2', [0x04] = '3',
        [0x05] = '4', [0x06] = '5', [0x07] = '6',
        [0x08] = '7', [0x09] = '8', [0x0A] = '9',
        [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
        [0x0E] = '\b',
        [0x10] = 'q', [0x11] = 'w', [0x12] = 'e',
        [0x13] = 'r', [0x14] = 't', [0x15] = 'y',
        [0x16] = 'u', [0x17] = 'i', [0x18] = 'o',
        [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
        [0x1C] = '\n',
        [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd',
        [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
        [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
        [0x27] = ';', [0x28] = '\'',
        [0x2B] = '\\',
        [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c',
        [0x2F] = 'v', [0x30] = 'b', [0x31] = 'n',
        [0x32] = 'm', [0x33] = ',', [0x34] = '.',
        [0x35] = '/', [0x39] = ' '
    },
    .shift = {
        [0x29] = '~',
        [0x02] = '!', [0x03] = '@', [0x04] = '#',
        [0x05] = '$', [0x06] = '%', [0x07] = '^',
        [0x08] = '&', [0x09] = '*', [0x0A] = '(',
        [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
        [0x0E] = '\b',
        [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E',
        [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y',
        [0x16] = 'U', [0x17] = 'I', [0x18] = 'O',
        [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
        [0x1C] = '\n',
        [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D',
        [0x21] = 'F', [0x22] = 'G', [0x23] = 'H',
        [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
        [0x27] = ':', [0x28] = '"',
        [0x2B] = '|',
        [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C',
        [0x2F] = 'V', [0x30] = 'B', [0x31] = 'N',
        [0x32] = 'M', [0x33] = '<', [0x34] = '>',
        [0x35] = '?', [0x39] = ' '
    }
};

/* ================================================================
 * Layout file parser
 * ================================================================ */

static int is_blank(char c)
{
    return c == ' ' || c == '\t';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

/* Next blank-separated token on the line; returns its length (0 = none). */
static size_t next_token(const char **p, const char *end, const char **tok)
{
    const char *s = *p;

    while (s < end && is_blank(*s))
        s++;

    *tok = s;

    while (s < end && !is_blank(*s))
        s++;

    *p = s;
    return (size_t)(s - *tok);
}

static int token_is(const char *tok, size_t n, const char *word)
{
    size_t i = 0;

    while (word[i] && i < n && tok[i] == word[i])
        i++;

    return word[i] == '\0' && i == n;
}

/* Token -> character. Returns 0 on success, 1 when the token is invalid. */
static int parse_char(const char *t, size_t n, char *out)
{
    int value;

    if (n == 1 && t[0] >= 33 && t[0] <= 126) {
        *out = t[0];
        return 0;
    }

    if (token_is(t, n, "none")) {
        *out = 0;
        return 0;
    }

    if (token_is(t, n, "space")) {
        *out = ' ';
        return 0;
    }

    if (n == 2 && t[0] == '\\') {
        switch (t[1]) {
        case '\\': *out = '\\'; return 0;
        case 'n':  *out = '\n'; return 0;
        case 'b':  *out = '\b'; return 0;
        default:   return 1;
        }
    }

    if (n == 4 && t[0] == '\\' && t[1] == 'x') {
        int hi = hex_value(t[2]);
        int lo = hex_value(t[3]);

        if (hi < 0 || lo < 0)
            return 1;

        value = hi * 16 + lo;

        if (value == 0 || value == '\b' || value == '\n' ||
            (value >= 32 && value <= 126)) {
            *out = (char)value;
            return 0;
        }
    }

    return 1;
}

static void copy_name(char *dst, const char *src, size_t n)
{
    size_t i = 0;

    while (i < n && i + 1 < KBD_NAME_MAX) {
        char c = src[i];

        dst[i++] = (c >= 32 && c <= 126) ? c : '?';
    }

    dst[i] = '\0';
}

int kbd_parse(const char *text, size_t length, kbd_layout_t *out)
{
    const char *p = text;
    const char *end = text + length;
    unsigned int line = 0;

    *out = kbd_layout_us;
    out->code[0] = '\0';
    out->name[0] = '\0';

    while (p < end) {
        const char *eol = p;
        const char *tok;
        size_t n;

        line++;

        while (eol < end && *eol != '\n')
            eol++;

        const char *next = (eol < end) ? eol + 1 : eol;
        const char *lend = eol;

        if (lend > p && lend[-1] == '\r')
            lend--;

        n = next_token(&p, lend, &tok);

        if (n == 0 || tok[0] == '#') {
            p = next;
            continue;
        }

        if (token_is(tok, n, "name")) {
            while (p < lend && is_blank(*p))
                p++;

            const char *e = lend;

            while (e > p && is_blank(e[-1]))
                e--;

            copy_name(out->name, p, (size_t)(e - p));
            p = next;
            continue;
        }

        /* "SC normal shift [altgr]": SC is 1-2 hex digits, 01-7F. */
        int all_hex = 1;
        unsigned int sc = 0;

        for (size_t i = 0; i < n; i++) {
            int d = hex_value(tok[i]);

            if (d < 0) {
                all_hex = 0;
                break;
            }

            sc = (sc << 4) | (unsigned int)d;
        }

        if (!all_hex)
            return (int)((line << 8) | KBD_E_SYNTAX);

        if (n > 2 || sc == 0 || sc >= KBD_KEYS)
            return (int)((line << 8) | KBD_E_SCANCODE);

        char c[3] = { 0, 0, 0 };
        int have = 0;

        for (; have < 3; have++) {
            n = next_token(&p, lend, &tok);

            if (n == 0)
                break;

            if (parse_char(tok, n, &c[have]))
                return (int)((line << 8) | KBD_E_CHAR);
        }

        n = next_token(&p, lend, &tok);

        if (have < 2 || n != 0)
            return (int)((line << 8) | KBD_E_SYNTAX);

        out->normal[sc] = c[0];
        out->shift[sc] = c[1];
        out->altgr[sc] = (have == 3) ? c[2] : 0;
        p = next;
    }

    return KBD_OK;
}
