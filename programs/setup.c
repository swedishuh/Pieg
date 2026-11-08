#include "libpieg.h"

/*
 * setup - keyboard layout picker, then a short guide.
 *
 * Started by the terminal when no layout has been saved yet (and runnable
 * any time). Up/Down moves and loads the layout at once so it can be tried
 * in the test line; Enter keeps it for the next boot; Esc leaves the
 * layout as it was. A mouse or touchpad works too: click a layout to try it,
 * then Keep or Skip. Afterwards a one-page guide lists the main commands.
 */

#define KBD_DIR     "/INPUTDEV/KEYBOARD"
#define KBD_CFG     KBD_DIR "/KEYMAP.CFG"
#define MAX_LAYOUTS 16
#define STEM_MAX    9
#define NAME_MAX_C  32

#define BG     0x14181dU
#define PANEL  0x1d232aU
#define EDGE   0x3a4550U
#define DIM    0x8a96a3U
#define TEXT   0xd8dee6U
#define ACCENT 0x5aa9ffU
#define SELBG  0x27405fU
#define WARN   0xe0b040U

#define KEY_UP   0x82
#define KEY_DOWN 0x83
#define KEY_ESC  0x1B

static char stems[MAX_LAYOUTS][STEM_MAX];
static char names[MAX_LAYOUTS][NAME_MAX_C];
static unsigned int count;
static char head[1024];

static void build_path(char *out, const char *stem)
{
    size_t o = 0;

    for (const char *p = KBD_DIR "/"; *p; p++)
        out[o++] = *p;
    for (size_t i = 0; stem[i]; i++)
        out[o++] = stem[i];
    for (const char *p = ".KBD"; *p; p++)
        out[o++] = *p;
    out[o] = '\0';
}

/* "name ..." line of a layout file, else the stem. */
static void read_name(unsigned int idx)
{
    char path[64];
    uint32_t got = 0;

    build_path(path, stems[idx]);

    for (size_t i = 0; i < STEM_MAX; i++)
        names[idx][i] = stems[idx][i];

    if (pieg->fs_read(path, 0, head, sizeof(head) - 1, &got))
        return;

    for (uint32_t i = 0; i < got;) {
        uint32_t s = i;

        while (i < got && head[i] != '\n')
            i++;

        uint32_t e = i++;

        while (s < e && (head[s] == ' ' || head[s] == '\t'))
            s++;

        if (e - s > 5 && !memcmp(head + s, "name", 4) &&
            (head[s + 4] == ' ' || head[s + 4] == '\t')) {
            s += 5;

            while (s < e && (head[s] == ' ' || head[s] == '\t'))
                s++;
            while (e > s && (head[e - 1] == '\r' || head[e - 1] == ' '))
                e--;

            size_t n = 0;

            while (s < e && n + 1 < NAME_MAX_C)
                names[idx][n++] = head[s++];

            names[idx][n] = '\0';
            return;
        }
    }
}

static void on_entry(const char *name, uint8_t attr, uint32_t size, void *ctx)
{
    (void)size;
    (void)ctx;

    size_t n = p_strlen(name);

    if ((attr & PIEG_ATTR_DIRECTORY) || count >= MAX_LAYOUTS ||
        n < 5 || n - 4 >= STEM_MAX || !p_ieq(name + n - 4, ".KBD"))
        return;

    unsigned int j = count++;

    for (size_t i = 0; i < n - 4; i++)
        stems[j][i] = p_lower(name[i]);
    stems[j][n - 4] = '\0';
}

static void sort_layouts(void)
{
    for (unsigned int i = 1; i < count; i++)
        for (unsigned int j = i; j > 0; j--) {
            const char *a = stems[j - 1], *b = stems[j];
            char t[STEM_MAX];

            while (*a && *a == *b) {
                a++;
                b++;
            }

            if ((unsigned char)*a <= (unsigned char)*b)
                break;

            memcpy(t, stems[j], STEM_MAX);
            memcpy(stems[j], stems[j - 1], STEM_MAX);
            memcpy(stems[j - 1], t, STEM_MAX);
        }
}

static void load(unsigned int idx)
{
    char path[64];

    build_path(path, stems[idx]);
    pieg->kbd_load(path);
}

/* ---- drawing ---- */

#define BTN    0x2f6fbfU
#define BTN_HI 0x4a8ae0U
#define BTN_BG 0x2b343dU
#define HOVER  0x222c36U

#define HIT_NONE -1
#define HIT_KEEP 100
#define HIT_SKIP 101

static uint32_t W, H, S;                /* S = text scale */

/* Pointer state; has_mouse is 0 on an older kernel or without a mouse. */
static int has_mouse, mx, my, mbtn;

static void put_text(uint32_t x, uint32_t y, const char *s, uint32_t c)
{
    pieg->gfx_text(x, y, s, c, S);
}

static void poll_mouse(void)
{
    if (pieg->version >= 9 && pieg->mouse_state)
        has_mouse = pieg->mouse_state(&mx, &my, &mbtn) != 0;
}

typedef struct {
    uint32_t px, py, pw, ph;            /* panel */
    uint32_t x, y;                      /* text origin */
    uint32_t cw, lh;                    /* character width, line height */
} geom_t;

static geom_t layout(unsigned int lines, unsigned int cols)
{
    geom_t g;

    g.cw = 6 * S;
    g.lh = 10 * S;
    g.pw = g.cw * cols;
    g.ph = g.lh * lines;

    if (g.pw > W)
        g.pw = W;
    if (g.ph > H)
        g.ph = H;

    g.px = (W - g.pw) / 2;
    g.py = (H - g.ph) / 2;
    g.x = g.px + 2 * g.cw;
    g.y = g.py + g.lh;
    return g;
}

static void panel(const geom_t *g)
{
    pieg->gfx_rect(0, 0, W, H, BG);
    pieg->gfx_rect(g->px, g->py, g->pw, g->ph, PANEL);
    pieg->gfx_rect(g->px, g->py, g->pw, 1, EDGE);
    pieg->gfx_rect(g->px, g->py + g->ph - 1, g->pw, 1, EDGE);
    pieg->gfx_rect(g->px, g->py, 1, g->ph, EDGE);
    pieg->gfx_rect(g->px + g->pw - 1, g->py, 1, g->ph, EDGE);
}

static int inside(int x, int y, uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh)
{
    return x >= (int)rx && y >= (int)ry &&
           x < (int)(rx + rw) && y < (int)(ry + rh);
}

/* Page 1 geometry: title, hint, one row per layout, test line, buttons. */
static geom_t page1(void)
{
    return layout(count + 10, 46);
}

static void button_rect(const geom_t *g, int which, uint32_t *x, uint32_t *y,
                        uint32_t *w, uint32_t *h)
{
    *w = g->cw * 14;
    *h = 12 * S;
    *x = g->x + (which == HIT_SKIP ? g->cw * 16 : 0);
    *y = g->y + g->lh * (count + 6) - 2 * S;
}

/* What the pointer is over on page 1: a layout row, a button, or nothing. */
static int hit_test(int x, int y)
{
    geom_t g = page1();
    uint32_t bx, by, bw, bh;

    if (!has_mouse)
        return HIT_NONE;

    for (unsigned int i = 0; i < count; i++) {
        uint32_t ry = g.y + g.lh * (i + 3);

        if (inside(x, y, g.px + g.cw, ry - S, g.pw - 2 * g.cw, 9 * S))
            return (int)i;
    }

    button_rect(&g, HIT_KEEP, &bx, &by, &bw, &bh);
    if (inside(x, y, bx, by, bw, bh))
        return HIT_KEEP;

    button_rect(&g, HIT_SKIP, &bx, &by, &bw, &bh);
    if (inside(x, y, bx, by, bw, bh))
        return HIT_SKIP;

    return HIT_NONE;
}

static void draw_button(const geom_t *g, int which, const char *label,
                        int hot, int primary)
{
    uint32_t bx, by, bw, bh;

    button_rect(g, which, &bx, &by, &bw, &bh);
    pieg->gfx_rect(bx, by, bw, bh,
                   primary ? (hot ? BTN_HI : BTN) : (hot ? EDGE : BTN_BG));
    put_text(bx + g->cw, by + 2 * S, label, TEXT);
}

static void frame1(uint32_t ms, unsigned int sel, int hover, const char *typed,
                   int saved_fail)
{
    geom_t g = page1();

    panel(&g);

    put_text(g.x, g.y, "Keyboard layout", ACCENT);
    put_text(g.x, g.y + g.lh, "Pick the layout of your keyboard.", DIM);

    for (unsigned int i = 0; i < count; i++) {
        uint32_t ry = g.y + g.lh * (i + 3);
        int on = i == sel;

        if (on)
            pieg->gfx_rect(g.px + g.cw, ry - S, g.pw - 2 * g.cw, 9 * S, SELBG);
        else if (hover == (int)i)
            pieg->gfx_rect(g.px + g.cw, ry - S, g.pw - 2 * g.cw, 9 * S, HOVER);

        put_text(g.x, ry, stems[i], on ? ACCENT : TEXT);
        put_text(g.x + 10 * g.cw, ry, names[i], on ? TEXT : DIM);
    }

    uint32_t ty = g.y + g.lh * (count + 4);
    char line[48] = "Try it: ";
    size_t o = 8, t = p_strlen(typed);

    memcpy(line + o, typed, t);
    o += t;
    line[o] = '\0';
    put_text(g.x, ty, line, TEXT);

    if ((ms / 500) & 1)
        pieg->gfx_rect(g.x + (uint32_t)o * g.cw, ty, g.cw - S, 7 * S, ACCENT);

    draw_button(&g, HIT_KEEP, "Keep  Enter", hover == HIT_KEEP, 1);
    draw_button(&g, HIT_SKIP, "Skip  Esc", hover == HIT_SKIP, 0);

    put_text(g.x, g.y + g.lh * (count + 8),
             saved_fail ? "Cannot save the layout (read-only?)"
                        : (has_mouse ? "Up/Down or click to choose a layout"
                                     : "Up/Down to choose a layout"),
             saved_fail ? WARN : DIM);

    pieg->gfx_present();
}

static int keep(const char *stem)
{
    char text[16];
    size_t n = 0;

    for (size_t i = 0; stem[i]; i++)
        text[n++] = stem[i];

    text[n++] = '\n';
    return pieg->fs_write(KBD_CFG, text, (uint32_t)n, 0);
}

/* ---- page 2: the guide ---- */

typedef struct {
    const char *cmd;
    const char *what;
} guide_entry_t;

static const guide_entry_t commands[] = {
    { "help",         "built-ins and programs" },
    { "ls  cd",       "look around the filesystem" },
    { "cat",          "show a file" },
    { "keymap",       "change the keyboard layout" },
    { "setup",        "open this setup again" },
    { "mouse",        "test a mouse or touchpad" },
    { "lspci",        "list PCI devices" },
    { "iys",          "run .ys scripts" },
    { "ver  uname",   "version information" },
    { "reboot  halt", "restart or power off" },
};

#define N_COMMANDS ((unsigned int)(sizeof(commands) / sizeof(commands[0])))

static void frame2(uint32_t ms)
{
    geom_t g = layout(N_COMMANDS + 12, 50);
    uint32_t y = g.y;

    panel(&g);

    put_text(g.x, y, "Welcome to PIEG", ACCENT);
    put_text(g.x, y + g.lh, "Pieg Isn't Even GNU. You start in the shell.", DIM);

    y += g.lh * 3;
    put_text(g.x, y, "Commands", ACCENT);

    for (unsigned int i = 0; i < N_COMMANDS; i++) {
        y += g.lh;
        put_text(g.x + 2 * g.cw, y, commands[i].cmd, TEXT);
        put_text(g.x + 16 * g.cw, y, commands[i].what, DIM);
    }

    y += g.lh * 2;
    put_text(g.x, y, "Ctrl+Up/Down scroll    Ctrl+C cancel", DIM);
    y += g.lh;
    put_text(g.x, y, "Ctrl+P / Ctrl+N history", DIM);
    y += g.lh;
    put_text(g.x, y, has_mouse ? "Your mouse or touchpad was found."
                               : "Type  help  any time for the full list.", DIM);

    y += g.lh * 2;

    if ((ms / 600) & 1)
        put_text(g.x, y, has_mouse ? "Press any key or click to start"
                                   : "Press any key to start", ACCENT);

    pieg->gfx_present();
}

static void show_guide(void)
{
    uint32_t start = pieg->gfx_ms(), last = 0;
    int prev = mbtn;
    int drawn = 0;

    for (;;) {
        uint32_t now = pieg->gfx_ms();
        unsigned char c = (unsigned char)pieg->getchar();

        poll_mouse();

        /* Ignore the first moments: the Enter or click that got us here. */
        if (now - start > 400) {
            if (c)
                return;
            if ((mbtn & 1) && !(prev & 1))
                return;
        }

        prev = mbtn;

        if (!drawn || now - last >= 300) {
            last = now;
            drawn = 1;
            frame2(now - start);
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (pieg->version < 8 || !pieg->gfx_begin) {
        p_putln("setup: needs a newer kernel (API 8)");
        return 1;
    }

    pieg->fs_list(KBD_DIR, on_entry, 0);

    if (!count) {
        p_putln("setup: no layouts in " KBD_DIR);
        return 1;
    }

    sort_layouts();

    for (unsigned int i = 0; i < count; i++)
        read_name(i);

    char original[PIEG_KBD_CODE_MAX];
    unsigned int sel = 0;

    pieg->kbd_info(original, 0);

    for (unsigned int i = 0; i < count; i++)
        if (p_streq(stems[i], original))
            sel = i;

    if (pieg->gfx_begin(&W, &H)) {
        p_putln("setup: no graphical display; use 'keymap'");
        return 1;
    }

    S = H / 300;
    S = S < 1 ? 1 : S > 4 ? 4 : S;

    char typed[24] = "";
    size_t typed_n = 0;
    int fail = 0, done = 0, accepted = 0;
    int dirty = 1, hover = HIT_NONE, prev_btn = 0;
    uint32_t blink = 0;

    poll_mouse();
    prev_btn = mbtn;

    while (!done) {
        uint32_t now = pieg->gfx_ms();
        unsigned char c = (unsigned char)pieg->getchar();
        int act = 0;                    /* what a click chose: layout, Keep, Skip */

        poll_mouse();

        int h = hit_test(mx, my);

        if (h != hover) {
            hover = h;
            dirty = 1;
        }

        if ((mbtn & 1) && !(prev_btn & 1) && h != HIT_NONE)
            act = 1;

        prev_btn = mbtn;

        if (act && h >= 0 && h < HIT_KEEP) {
            sel = (unsigned int)h;
            load(sel);
            typed[0] = '\0';
            typed_n = 0;
            dirty = 1;
        } else if (act && h == HIT_KEEP) {
            c = '\n';
        } else if (act && h == HIT_SKIP) {
            c = KEY_ESC;
        }

        if (c == KEY_UP || c == KEY_DOWN) {
            sel = (unsigned int)(c == KEY_UP ? (sel + count - 1) % count
                                             : (sel + 1) % count);
            load(sel);
            typed[0] = '\0';
            typed_n = 0;
            dirty = 1;
        } else if (c == KEY_ESC) {
            done = 1;
        } else if (c == '\n' || c == '\r') {
            fail = keep(stems[sel]) != 0;
            accepted = !fail;
            done = accepted;
            dirty = 1;
        } else if (c == '\b' || c == 127) {
            if (typed_n)
                typed[--typed_n] = '\0';
            dirty = 1;
        } else if (c >= 32 && c < 127) {
            if (typed_n + 1 >= sizeof(typed))
                typed_n = 0;
            typed[typed_n++] = c;
            typed[typed_n] = '\0';
            dirty = 1;
        }

        /* Redraw on input, and when the cursor changes phase. */
        if (!done && (dirty || (now / 500) != blink)) {
            blink = now / 500;
            dirty = 0;
            frame1(now, sel, hover, typed, fail);
        }
    }

    if (!accepted) {
        /* Esc: back to the layout that was active, and remember the skip. */
        for (unsigned int i = 0; i < count; i++)
            if (p_streq(stems[i], original))
                load(i);

        keep(original);
    }

    show_guide();

    pieg->gfx_end();
    return 0;
}
