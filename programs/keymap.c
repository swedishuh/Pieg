#include "libpieg.h"

/*
 * keymap [-t] [name | path]
 * keymap --test
 *
 * Shows or changes the keyboard layout. Layouts are text files in
 * /INPUTDEV/KEYBOARD (NAME.KBD). `keymap de` loads DE.KBD and keeps it for
 * the next boot; -t switches only until reboot. A name with a '.' or '/' is
 * treated as a path and is never kept.
 */

#define KBD_DIR   "/INPUTDEV/KEYBOARD"
#define KBD_CFG   "/INPUTDEV/KEYBOARD/KEYMAP.CFG"
#define MAX_LAYOUTS 24
#define STEM_MAX  9

static char stems[MAX_LAYOUTS][STEM_MAX];
static unsigned int stem_count;
static char head[1024];

static void pad(const char *s, unsigned int width)
{
    unsigned int n = (unsigned int)p_strlen(s);

    p_puts(s);

    while (n++ < width)
        p_putc(' ');
}

static void usage(void)
{
    p_putln("Usage: keymap                 show the current and available layouts");
    p_putln("       keymap <name>          switch to layout <name> and keep it");
    p_putln("       keymap -t <name>       switch until reboot only");
    p_putln("       keymap <path>          load a layout file (not kept)");
    p_putln("       keymap --test          type to try the layout (Ctrl+C ends)");
    p_putln("Layouts live in " KBD_DIR ".");
}

static void print_error(int r)
{
    int kind = r & 0xFF;
    int line = r >> 8;

    switch (kind) {
    case 1:  p_puts("cannot read the layout file"); break;
    case 2:  p_puts("layout file is too large (8 KiB max)"); break;
    case 3:  p_puts("syntax error"); break;
    case 4:  p_puts("bad scancode (use 01-7F in hex)"); break;
    case 5:  p_puts("bad character (printable ASCII, none, space)"); break;
    default: p_puts("unknown error"); break;
    }

    if (line) {
        p_puts(" on line ");
        p_u64((uint64_t)line);
    }

    p_putc('\n');
}

/* ---- directory listing ---- */

static void on_entry(const char *name, uint8_t attributes, uint32_t size,
                     void *context)
{
    (void)size;
    (void)context;

    if ((attributes & PIEG_ATTR_DIRECTORY) || stem_count >= MAX_LAYOUTS)
        return;

    size_t n = p_strlen(name);

    if (n < 5 || n - 4 >= STEM_MAX || !p_ieq(name + n - 4, ".KBD"))
        return;

    for (size_t i = 0; i < n - 4; i++)
        stems[stem_count][i] = p_lower(name[i]);

    stems[stem_count][n - 4] = '\0';
    stem_count++;
}

static void sort_stems(void)
{
    for (unsigned int i = 1; i < stem_count; i++) {
        char tmp[STEM_MAX];
        unsigned int j = i;

        memcpy(tmp, stems[i], STEM_MAX);

        while (j > 0) {
            const char *a = stems[j - 1];
            const char *b = tmp;

            while (*a && *a == *b) {
                a++;
                b++;
            }

            if ((unsigned char)*a <= (unsigned char)*b)
                break;

            memcpy(stems[j], stems[j - 1], STEM_MAX);
            j--;
        }

        memcpy(stems[j], tmp, STEM_MAX);
    }
}

/* Copies the "name ..." line of a layout file into out; 0 when absent. */
static int layout_name(const char *stem, char *out, size_t cap)
{
    char path[64];
    uint32_t got = 0;
    size_t o = 0;

    for (const char *p = KBD_DIR "/"; *p; p++)
        path[o++] = *p;

    for (size_t i = 0; stem[i]; i++)
        path[o++] = stem[i];

    for (const char *p = ".KBD"; *p; p++)
        path[o++] = *p;

    path[o] = '\0';

    if (pieg->fs_read(path, 0, head, sizeof(head) - 1, &got))
        return 0;

    head[got] = '\0';

    for (uint32_t i = 0; i < got;) {
        uint32_t s = i;

        while (i < got && head[i] != '\n')
            i++;

        uint32_t e = i;

        if (i < got)
            i++;

        while (s < e && (head[s] == ' ' || head[s] == '\t'))
            s++;

        if (e - s > 5 && head[s] == 'n' && head[s + 1] == 'a' &&
            head[s + 2] == 'm' && head[s + 3] == 'e' &&
            (head[s + 4] == ' ' || head[s + 4] == '\t')) {
            s += 5;

            while (s < e && (head[s] == ' ' || head[s] == '\t'))
                s++;

            while (e > s && (head[e - 1] == '\r' || head[e - 1] == ' ' ||
                             head[e - 1] == '\t'))
                e--;

            size_t n = 0;

            while (s < e && n + 1 < cap)
                out[n++] = head[s++];

            out[n] = '\0';
            return n > 0;
        }
    }

    return 0;
}

static int show(void)
{
    char code[PIEG_KBD_CODE_MAX];
    char name[PIEG_KBD_NAME_MAX];
    char title[PIEG_KBD_NAME_MAX];

    pieg->kbd_info(code, name);

    p_puts("Current layout: ");
    p_puts(code);
    p_puts(" - ");
    p_putln(name);

    int r = pieg->fs_list(KBD_DIR, on_entry, 0);

    if (r) {
        p_puts("Cannot read " KBD_DIR ": ");
        p_putln(pieg->fs_error(r));
        return 1;
    }

    sort_stems();

    if (!stem_count) {
        p_putln("No layouts found in " KBD_DIR ".");
        return 0;
    }

    p_putln("\nAvailable layouts:");

    for (unsigned int i = 0; i < stem_count; i++) {
        p_puts(p_streq(stems[i], code) ? " * " : "   ");
        pad(stems[i], 10);
        p_putln(layout_name(stems[i], title, sizeof(title)) ? title : "(no name)");
    }

    p_putln("\nSwitch with: keymap <name>");
    return 0;
}

/* ---- switching ---- */

static int is_plain_name(const char *s)
{
    size_t n = p_strlen(s);

    if (n == 0 || n > 8)
        return 0;

    for (size_t i = 0; i < n; i++) {
        char c = s[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }

    return 1;
}

static int keep_default(const char *stem)
{
    char text[16];
    size_t n = 0;

    for (size_t i = 0; stem[i]; i++)
        text[n++] = p_lower(stem[i]);

    text[n++] = '\n';
    return pieg->fs_write(KBD_CFG, text, (uint32_t)n, 0);
}

static int switch_to(const char *arg, int keep)
{
    char path[256];
    char code[PIEG_KBD_CODE_MAX];
    char name[PIEG_KBD_NAME_MAX];
    int plain = is_plain_name(arg);

    if (plain) {
        size_t o = 0;

        for (const char *p = KBD_DIR "/"; *p; p++)
            path[o++] = *p;

        for (size_t i = 0; arg[i]; i++)
            path[o++] = arg[i];

        for (const char *p = ".KBD"; *p; p++)
            path[o++] = *p;

        path[o] = '\0';
    } else {
        pieg->resolve(arg, path, sizeof(path));
    }

    int r = pieg->kbd_load(path);

    if (r) {
        p_puts("keymap: ");
        p_puts(arg);
        p_puts(": ");
        print_error(r);

        if (plain && (r & 0xFF) == 1)
            p_putln("Run keymap with no arguments to see the available layouts.");

        return 1;
    }

    pieg->kbd_info(code, name);
    p_puts("Layout: ");
    p_puts(code);
    p_puts(" - ");
    p_putln(name);

    if (!plain) {
        p_putln("(loaded from a path, so it is not kept after reboot)");
        return 0;
    }

    if (!keep)
        return 0;

    r = keep_default(arg);

    if (r) {
        p_puts("Not kept after reboot: ");
        p_putln(pieg->fs_error(r));
    } else {
        p_putln("Kept for the next boot.");
    }

    return 0;
}

/* ---- --test ---- */

static void test_layout(void)
{
    p_putln("Type to try the layout. Enter starts a new line, Ctrl+C ends.");

    for (;;) {
        char c = p_getc();

        if (c == 3)
            break;

        if (c == '\n' || (c >= 32 && c <= 126))
            p_putc(c);
    }

    p_putln("");
}

int main(int argc, char **argv)
{
    int keep = 1;
    const char *target = 0;

    for (int i = 1; i < argc; i++) {
        if (p_streq(argv[i], "-h") || p_streq(argv[i], "--help")) {
            usage();
            return 0;
        }

        if (p_streq(argv[i], "--test")) {
            test_layout();
            return 0;
        }

        if (p_streq(argv[i], "-l") || p_streq(argv[i], "--list")) {
            target = 0;
            continue;
        }

        if (p_streq(argv[i], "-t") || p_streq(argv[i], "--temp")) {
            keep = 0;
            continue;
        }

        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            p_puts("keymap: unknown option ");
            p_putln(argv[i]);
            usage();
            return 1;
        }

        if (target) {
            usage();
            return 1;
        }

        target = argv[i];
    }

    if (!target)
        return show();

    return switch_to(target, keep);
}
