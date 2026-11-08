
#include "libpieg.h"

/*
 * PPaint - PIEG Paint
 * Graphics API 9
 *
 * Mouse:
 *   Left click / drag       Draw
 *   Palette                 Select color
 *   Slider                  Change brush size
 *
 * Keyboard:
 *   1-8                     Select color
 *   + / =                   Increase brush size
 *   -                       Decrease brush size
 *   E                       Toggle eraser
 *   C                       Clear canvas
 *   S                       Settings
 *   Arrow keys              Move cursor (when enabled)
 *   Enter                   Paint at cursor
 *   Esc                     Exit
 */

#define MAX_CELLS 65536
#define MAX_SIDE 256

#define BG          0x14181dU
#define PANEL       0x1d232aU
#define PANEL_DARK  0x171c22U
#define EDGE        0x3a4550U
#define DIM         0x8a96a3U
#define TEXT        0xd8dee6U
#define ACCENT      0x5aa9ffU
#define SELECTED    0x263e55U
#define HOVER       0x293541U
#define WARN        0xe0ad62U
#define CANVAS_BG   0x101419U

#define BRUSH_MIN 1
#define BRUSH_MAX 15

#define PALETTE_COUNT 8

static const unsigned int colors[PALETTE_COUNT] = {
    0x39d98a,
    0xffffff,
    0xff6262,
    0xffa34d,
    0xffdf5d,
    0x5aa9ff,
    0xc084fc,
    0x101419
};

static unsigned char canvas[MAX_CELLS];

static unsigned int W, H;
static unsigned int scale;

static unsigned int header_h;
static unsigned int toolbar_h;
static unsigned int footer_h;

static unsigned int canvas_x, canvas_y;
static unsigned int canvas_w, canvas_h;
static unsigned int cell_size;
static unsigned int cols, rows;

static unsigned int selected_color;
static unsigned int brush_size = 1;

static int eraser;
static int settings;
static int arrow_drawing;
static int exit_requested;

static int has_mouse;
static int mx, my, mbtn;
static int prev_btn;

static int drawing;
static int dragging_brush;

static int cursor_x;
static int cursor_y;
static int cursor_valid;

static int last_cell_x = -1;
static int last_cell_y = -1;

/* =========================================================
 * Helpers
 * ========================================================= */

static unsigned int min_u(unsigned int a, unsigned int b)
{
    return a < b ? a : b;
}

static unsigned int max_u(unsigned int a, unsigned int b)
{
    return a > b ? a : b;
}

static int inside(int x, int y,
                  unsigned int rx, unsigned int ry,
                  unsigned int rw, unsigned int rh)
{
    return x >= (int)rx &&
           y >= (int)ry &&
           x < (int)(rx + rw) &&
           y < (int)(ry + rh);
}

static void rect(unsigned int x, unsigned int y,
                 unsigned int w, unsigned int h,
                 unsigned int color)
{
    if (w && h)
        pieg->gfx_rect(x, y, w, h, color);
}

static void outline(unsigned int x, unsigned int y,
                    unsigned int w, unsigned int h,
                    unsigned int color)
{
    if (!w || !h)
        return;

    rect(x, y, w, 1, color);
    rect(x, y + h - 1, w, 1, color);
    rect(x, y, 1, h, color);
    rect(x + w - 1, y, 1, h, color);
}

static void text(unsigned int x, unsigned int y,
                 const char *str, unsigned int color)
{
    pieg->gfx_text(x, y, str, color, scale);
}

static void button(unsigned int x, unsigned int y,
                   unsigned int w, unsigned int h,
                   const char *label, int active)
{
    rect(x, y, w, h, active ? SELECTED : PANEL_DARK);
    outline(x, y, w, h, active ? ACCENT : EDGE);

    text(x + 7 * scale,
         y + (h - 7 * scale) / 2,
         label,
         active ? ACCENT : TEXT);
}

static void set_brush_size(int value)
{
    if (value < BRUSH_MIN)
        value = BRUSH_MIN;

    if (value > BRUSH_MAX)
        value = BRUSH_MAX;

    brush_size = (unsigned int)value;
}

static void poll_mouse(void)
{
    has_mouse = 0;

    if (pieg->version >= 9 && pieg->mouse_state)
        has_mouse = pieg->mouse_state(&mx, &my, &mbtn) != 0;
}

/* =========================================================
 * Layout
 * ========================================================= */

static void setup_layout(void)
{
    header_h = 36 * scale;
    toolbar_h = 52 * scale;
    footer_h = 24 * scale;

    unsigned int top = header_h + toolbar_h + 5 * scale;
    unsigned int bottom = H > footer_h + 8 * scale
                        ? H - footer_h - 8 * scale
                        : H;

    unsigned int available_w = W > 20 * scale
                             ? W - 20 * scale
                             : W;

    unsigned int available_h = bottom > top
                             ? bottom - top
                             : 1;

    cell_size = 1;

    while (available_w / cell_size > MAX_SIDE ||
           available_h / cell_size > MAX_SIDE)
        cell_size++;

    cols = available_w / cell_size;
    rows = available_h / cell_size;

    cols = min_u(cols, MAX_SIDE);
    rows = min_u(rows, MAX_SIDE);

    if (!cols) cols = 1;
    if (!rows) rows = 1;

    if (cols * rows > MAX_CELLS)
        rows = MAX_CELLS / cols;

    canvas_w = cols * cell_size;
    canvas_h = rows * cell_size;

    canvas_x = (W - canvas_w) / 2;
    canvas_y = top + (available_h - canvas_h) / 2;

    for (unsigned int i = 0; i < MAX_CELLS; i++)
        canvas[i] = 0;

    cursor_x = (int)(cols / 2);
    cursor_y = (int)(rows / 2);
    cursor_valid = 1;
}

/* =========================================================
 * Painting
 * ========================================================= */

static void clear_canvas(void)
{
    for (unsigned int i = 0; i < MAX_CELLS; i++)
        canvas[i] = 0;
}

static void paint_cell(int x, int y)
{
    if (x < 0 || y < 0 ||
        x >= (int)cols || y >= (int)rows)
        return;

    unsigned int index = (unsigned int)y * cols +
                         (unsigned int)x;

    canvas[index] = eraser ? 0 : (unsigned char)(selected_color + 1);
}

static void paint_brush(int x, int y)
{
    int radius = (int)brush_size / 2;

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy <= radius * radius + 1)
                paint_cell(x + dx, y + dy);
        }
    }
}

static void paint_line(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0;
    int dy = y1 - y0;

    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;

    int steps = ax > ay ? ax : ay;

    if (steps == 0) {
        paint_brush(x1, y1);
        return;
    }

    for (int i = 0; i <= steps; i++) {
        int x = x0 + dx * i / steps;
        int y = y0 + dy * i / steps;

        paint_brush(x, y);
    }
}

static void mouse_draw(void)
{
    if (!has_mouse)
        return;

    if (!inside(mx, my, canvas_x, canvas_y,
                canvas_w, canvas_h)) {
        drawing = 0;
        return;
    }

    int cx = (mx - (int)canvas_x) / (int)cell_size;
    int cy = (my - (int)canvas_y) / (int)cell_size;

    cursor_x = cx;
    cursor_y = cy;
    cursor_valid = 1;

    if (mbtn & 1) {
        if (!drawing) {
            paint_brush(cx, cy);
            drawing = 1;
        } else {
            paint_line(last_cell_x, last_cell_y, cx, cy);
        }

        last_cell_x = cx;
        last_cell_y = cy;
    } else {
        drawing = 0;
    }
}

/* =========================================================
 * Palette
 * ========================================================= */

#define SWATCH_SIZE 18
#define SWATCH_STEP 23

static unsigned int palette_y(void)
{
    return header_h + 4 * scale;
}

static unsigned int palette_x(unsigned int i)
{
    return 10 * scale + i * SWATCH_STEP * scale;
}

static int palette_hit(int x, int y)
{
    unsigned int py = palette_y();

    for (unsigned int i = 0; i < PALETTE_COUNT; i++) {
        if (inside(x, y,
                   palette_x(i), py,
                   SWATCH_SIZE * scale,
                   SWATCH_SIZE * scale))
            return (int)i;
    }

    return -1;
}

static void draw_palette(void)
{
    unsigned int py = palette_y();

    for (unsigned int i = 0; i < PALETTE_COUNT; i++) {
        unsigned int x = palette_x(i);

        rect(x, py,
             SWATCH_SIZE * scale,
             SWATCH_SIZE * scale,
             colors[i]);

        outline(x, py,
                SWATCH_SIZE * scale,
                SWATCH_SIZE * scale,
                EDGE);

        if (selected_color == i && !eraser) {
            outline(x - scale, py - scale,
                    (SWATCH_SIZE + 2) * scale,
                    (SWATCH_SIZE + 2) * scale,
                    ACCENT);
        }
    }
}

/* =========================================================
 * Brush slider
 * ========================================================= */

static unsigned int slider_y(void)
{
    return header_h + 31 * scale;
}

static unsigned int slider_x(void)
{
    return 48 * scale;
}

static unsigned int slider_w(void)
{
    unsigned int reserved = 52 * scale;

    if (W <= slider_x() + reserved)
        return 20 * scale;

    return W - slider_x() - reserved;
}

static void slider_update(int x)
{
    int sx = (int)slider_x();
    int sw = (int)slider_w();

    if (sw <= 0)
        return;

    if (x < sx) x = sx;
    if (x > sx + sw) x = sx + sw;

    int value = BRUSH_MIN +
        (x - sx) * (BRUSH_MAX - BRUSH_MIN) / sw;

    set_brush_size(value);
}

static int slider_hit(int x, int y)
{
    return inside(x, y,
                  slider_x() - 4 * scale,
                  slider_y() - 7 * scale,
                  slider_w() + 8 * scale,
                  18 * scale);
}

static void draw_slider(void)
{
    unsigned int sx = slider_x();
    unsigned int sy = slider_y();
    unsigned int sw = slider_w();

    text(10 * scale, sy - 2 * scale, "SIZE", DIM);

    rect(sx, sy + 2 * scale,
         sw, 4 * scale, EDGE);

    unsigned int filled =
        sw * (brush_size - BRUSH_MIN) /
        (BRUSH_MAX - BRUSH_MIN);

    rect(sx, sy + 2 * scale,
         filled, 4 * scale, ACCENT);

    unsigned int knob_x = sx + filled;

    rect(knob_x - 3 * scale,
         sy - 2 * scale,
         7 * scale,
         12 * scale,
         TEXT);

    outline(knob_x - 3 * scale,
            sy - 2 * scale,
            7 * scale,
            12 * scale,
            ACCENT);

    char value[3];

    if (brush_size >= 10) {
        value[0] = '0' + brush_size / 10;
        value[1] = '0' + brush_size % 10;
        value[2] = '\0';
    } else {
        value[0] = '0' + brush_size;
        value[1] = '\0';
    }

    text(sx + sw + 9 * scale,
         sy - 2 * scale,
         value,
         ACCENT);
}

/* =========================================================
 * Header and footer
 * ========================================================= */

static unsigned int exit_x(void)
{
    return W - 58 * scale;
}

static void draw_header(void)
{
    rect(0, 0, W, header_h, PANEL);
    rect(0, header_h - scale, W, scale, EDGE);

    text(12 * scale, 12 * scale, "PPaint", ACCENT);

    text(78 * scale, 12 * scale, "PIEG IMAGE EDITOR", DIM);

    button(exit_x(), 5 * scale,
           48 * scale, 26 * scale,
           "Exit", 0);
}

static void draw_toolbar(void)
{
    unsigned int y = header_h;

    rect(0, y, W, toolbar_h, PANEL_DARK);
    rect(0, y + toolbar_h - scale, W, scale, EDGE);

    draw_palette();
    draw_slider();
}

static void draw_footer(void)
{
    unsigned int y = H - footer_h;

    rect(0, y, W, footer_h, PANEL);
    rect(0, y, W, scale, EDGE);

    text(10 * scale, y + 8 * scale,
         "S: SETTINGS", DIM);

    text(105 * scale, y + 8 * scale,
         "E: ERASER", DIM);

    text(190 * scale, y + 8 * scale,
         "+/-: SIZE", DIM);
}

/* =========================================================
 * Canvas rendering
 * ========================================================= */

static void draw_canvas(void)
{
    rect(canvas_x - 2 * scale,
         canvas_y - 2 * scale,
         canvas_w + 4 * scale,
         canvas_h + 4 * scale,
         PANEL_DARK);

    outline(canvas_x - 2 * scale,
            canvas_y - 2 * scale,
            canvas_w + 4 * scale,
            canvas_h + 4 * scale,
            EDGE);

    rect(canvas_x, canvas_y,
         canvas_w, canvas_h,
         CANVAS_BG);

    for (unsigned int y = 0; y < rows; y++) {
        for (unsigned int x = 0; x < cols; x++) {
            unsigned char c = canvas[y * cols + x];

            if (!c)
                continue;

            rect(canvas_x + x * cell_size,
                 canvas_y + y * cell_size,
                 cell_size,
                 cell_size,
                 colors[c - 1]);
        }
    }

    if (cursor_valid &&
        cursor_x >= 0 && cursor_y >= 0 &&
        cursor_x < (int)cols &&
        cursor_y < (int)rows) {

        unsigned int x = canvas_x +
                         (unsigned int)cursor_x * cell_size;

        unsigned int y = canvas_y +
                         (unsigned int)cursor_y * cell_size;

        outline(x, y, cell_size, cell_size, ACCENT);
    }
}

/* =========================================================
 * Settings dialog
 * ========================================================= */

static unsigned int settings_w(void)
{
    unsigned int available = W > 16 * scale
                           ? W - 16 * scale
                           : W;

    return min_u(available, 340 * scale);
}

static unsigned int settings_h(void)
{
    unsigned int available = H > 16 * scale
                           ? H - 16 * scale
                           : H;

    return min_u(available, 190 * scale);
}

static unsigned int settings_x(void)
{
    return (W - settings_w()) / 2;
}

static unsigned int settings_y(void)
{
    return (H - settings_h()) / 2;
}

static void draw_settings(void)
{
    unsigned int x = settings_x();
    unsigned int y = settings_y();
    unsigned int w = settings_w();
    unsigned int h = settings_h();

    rect(0, 0, W, H, 0x0b1016);

    rect(x, y, w, h, PANEL);
    outline(x, y, w, h, EDGE);

    rect(x, y, w, 3 * scale, ACCENT);

    text(x + 14 * scale, y + 14 * scale,
         "Settings", ACCENT);

    text(x + 14 * scale, y + 42 * scale,
         "INPUT", DIM);

    unsigned int row_y = y + 58 * scale;
    unsigned int row_h = 38 * scale;

    rect(x + 10 * scale, row_y,
         w - 20 * scale, row_h,
         PANEL_DARK);

    outline(x + 10 * scale, row_y,
            w - 20 * scale, row_h,
            EDGE);

    text(x + 18 * scale,
         row_y + 12 * scale,
         "Arrow-key drawing",
         TEXT);

    button(x + w - 72 * scale,
           row_y + 5 * scale,
           60 * scale,
           28 * scale,
           arrow_drawing ? "ON" : "OFF",
           arrow_drawing);

    text(x + 14 * scale,
         y + 112 * scale,
         "Arrows move the cursor.", DIM);

    text(x + 14 * scale,
         y + 128 * scale,
         "Enter paints at its position.", DIM);

    button(x + w - 78 * scale,
           y + h - 38 * scale,
           66 * scale,
           28 * scale,
           "Back", 0);

    pieg->gfx_present();
}

static void handle_settings_click(int x, int y)
{
    unsigned int sx = settings_x();
    unsigned int sy = settings_y();
    unsigned int w = settings_w();
    unsigned int h = settings_h();

    unsigned int row_y = sy + 58 * scale;

    if (inside(x, y,
               sx + 10 * scale,
               row_y,
               w - 20 * scale,
               38 * scale)) {
        arrow_drawing = !arrow_drawing;
        return;
    }

    if (inside(x, y,
               sx + w - 78 * scale,
               sy + h - 38 * scale,
               66 * scale,
               28 * scale)) {
        settings = 0;
    }
}

/* =========================================================
 * Frame rendering
 * ========================================================= */

static void render(void)
{
    rect(0, 0, W, H, BG);

    draw_canvas();
    draw_header();
    draw_toolbar();
    draw_footer();

    if (settings)
        draw_settings();
    else
        pieg->gfx_present();
}

/* =========================================================
 * Keyboard
 * ========================================================= */

static void handle_keyboard(unsigned char c)
{
    if (settings) {
        if (c == 27 || c == 's' || c == 'S')
            settings = 0;

        return;
    }

    if (c == 27) {
        exit_requested = 1;
        return;
    }

    if (c == 's' || c == 'S') {
        settings = 1;
        drawing = 0;
        return;
    }

    if (c == 'e' || c == 'E') {
        eraser = !eraser;
        return;
    }

    if (c == 'c' || c == 'C') {
        clear_canvas();
        return;
    }

    if (c == '+' || c == '=') {
        set_brush_size((int)brush_size + 1);
        return;
    }

    if (c == '-') {
        set_brush_size((int)brush_size - 1);
        return;
    }

    if (c >= '1' && c <= '8') {
        selected_color = c - '1';
        eraser = 0;
        return;
    }

    if (!arrow_drawing)
        return;

    if (c == 0x82 && cursor_y > 0)
        cursor_y--;

    if (c == 0x83 && cursor_y + 1 < (int)rows)
        cursor_y++;

    if (c == '\n')
        paint_brush(cursor_x, cursor_y);
}

/* =========================================================
 * Main
 * ========================================================= */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (pieg->version < 9 ||
        !pieg->gfx_begin ||
        !pieg->mouse_state) {
        p_putln("ppaint: requires graphics API 9");
        return 1;
    }

    if (pieg->gfx_begin(&W, &H)) {
        p_putln("ppaint: unable to start graphics");
        return 1;
    }

    if (W < 240 || H < 180) {
        pieg->gfx_end();
        p_putln("ppaint: resolution too small");
        return 1;
    }

    scale = min_u(W / 480, H / 360);

    if (scale < 1)
        scale = 1;

    if (scale > 3)
        scale = 3;

    setup_layout();

    poll_mouse();
    prev_btn = mbtn;

    render();

    unsigned int last_frame = pieg->gfx_ms();

    for (;;) {
        unsigned int now = pieg->gfx_ms();

        unsigned char key =
            (unsigned char)pieg->getchar();

        int changed = 0;

        poll_mouse();

        if (key) {
            handle_keyboard(key);
            changed = 1;
        }

        if (exit_requested) {
            pieg->gfx_end();
            return 0;
        }

        int click = (mbtn & 1) && !(prev_btn & 1);

        if (settings) {
            if (click)
                handle_settings_click(mx, my);

            changed = 1;
        } else {
            if (click) {
                int color = palette_hit(mx, my);

                if (color >= 0) {
                    selected_color = (unsigned int)color;
                    eraser = 0;
                    drawing = 0;
                    changed = 1;
                } else if (slider_hit(mx, my)) {
                    dragging_brush = 1;
                    slider_update(mx);
                    drawing = 0;
                    changed = 1;
                } else if (inside(mx, my,
                                  exit_x(), 5 * scale,
                                  48 * scale, 26 * scale)) {
                    exit_requested = 1;
                }
            }

            if (dragging_brush) {
                if (mbtn & 1) {
                    slider_update(mx);
                    changed = 1;
                } else {
                    dragging_brush = 0;
                }
            } else {
                mouse_draw();
            }

            if (has_mouse)
                changed = 1;
        }

        prev_btn = mbtn;

        if (exit_requested) {
            pieg->gfx_end();
            return 0;
        }

        if (now - last_frame >= 33) {
            last_frame = now;
            changed = 1;
        }

        if (changed)
            render();
    }

    pieg->gfx_end();
    return 0;
}
