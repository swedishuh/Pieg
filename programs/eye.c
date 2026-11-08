
#include "libpieg.h"

#include <stdint.h>
#include <stddef.h>

#define EYE_VERSION "Lost track of version"

#define RPF_HEADER_SIZE 16u
#define RPF_FORMAT_RGB24 1u
#define EYE_MAX_DIMENSION 4096u

#define COLOR_BACKGROUND 0x101820u
#define COLOR_TEXT       0xd8dee6u
#define COLOR_ACCENT     0x5aa9ffu

static uint32_t screen_w;
static uint32_t screen_h;

/* One source image row, RGB24. */
static uint8_t row_buffer[EYE_MAX_DIMENSION * 3u];

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint32_t make_color(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16)
         | ((uint32_t)p[1] << 8)
         | (uint32_t)p[2];
}

static void report(const char *message)
{
    p_puts("eye: ");
    p_putln(message);
}

static int read_exact(const char *path,
                      uint32_t offset,
                      void *buffer,
                      uint32_t length)
{
    uint32_t done = 0;

    int error = pieg->fs_read(path, offset, buffer, length, &done);

    if (error != 0)
        return error;

    if (done != length)
        return -1;

    return 0;
}

static int load_image(const char *path)
{
    uint8_t header[RPF_HEADER_SIZE];

    int error = read_exact(path, 0, header, sizeof(header));

    if (error) {
        report("could not read image header");
        return 1;
    }

    if (header[0] != 'R' ||
        header[1] != 'P' ||
        header[2] != 'F' ||
        header[3] != '1') {
        report("not an RPF1 image");
        return 1;
    }

    uint32_t image_w = read_u32_le(header + 4);
    uint32_t image_h = read_u32_le(header + 8);
    uint32_t format  = read_u32_le(header + 12);

    if (format != RPF_FORMAT_RGB24) {
        report("unsupported pixel format");
        return 1;
    }

    if (!image_w || !image_h ||
        image_w > EYE_MAX_DIMENSION ||
        image_h > EYE_MAX_DIMENSION) {
        report("invalid image dimensions (maximum 4096x4096)");
        return 1;
    }

    uint32_t row_bytes = image_w * 3u;
    uint32_t expected_size =
        RPF_HEADER_SIZE + row_bytes * image_h;

    int is_directory = 0;
    uint32_t file_size = 0;

    error = pieg->fs_stat(path, &is_directory, &file_size);

    if (error || is_directory) {
        report("file does not exist or is a directory");
        return 1;
    }

    if (file_size != expected_size) {
        report("invalid RPF file size");
        return 1;
    }

    /*
     * Fit the image inside the display while preserving
     * its aspect ratio.
     */
    uint32_t draw_w;
    uint32_t draw_h;

    if (screen_w * image_h <= screen_h * image_w) {
        draw_w = screen_w;
        draw_h = (image_h * screen_w) / image_w;
    } else {
        draw_h = screen_h;
        draw_w = (image_w * screen_h) / image_h;
    }

    if (!draw_w) draw_w = 1;
    if (!draw_h) draw_h = 1;

    uint32_t origin_x = (screen_w - draw_w) / 2;
    uint32_t origin_y = (screen_h - draw_h) / 2;

    pieg->gfx_rect(0, 0, screen_w, screen_h, COLOR_BACKGROUND);

    uint32_t previous_source_y = 0xFFFFFFFFu;

    for (uint32_t y = 0; y < draw_h; y++) {
        uint32_t source_y = (y * image_h) / draw_h;

        /*
         * Read a source row only when the scaled output
         * moves to a different source row.
         */
        if (source_y != previous_source_y) {
            uint32_t offset =
                RPF_HEADER_SIZE + source_y * row_bytes;

            error = read_exact(path, offset, row_buffer, row_bytes);

            if (error) {
                report("failed while reading image pixels");
                return 1;
            }

            previous_source_y = source_y;
        }

        uint32_t screen_y = origin_y + y;

        for (uint32_t x = 0; x < draw_w; x++) {
            uint32_t source_x = (x * image_w) / draw_w;
            const uint8_t *pixel = &row_buffer[source_x * 3u];

            uint32_t color = make_color(pixel);

            pieg->gfx_rect(
                origin_x + x,
                screen_y,
                1,
                1,
                color
            );
        }
    }

    pieg->gfx_present();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        p_putln("EYE " EYE_VERSION " - PIEG image viewer");
        p_putln("");
        p_putln("Usage: eye IMAGE.RPF");
        return 1;
    }

    if (!pieg || pieg->version < 8 || !pieg->gfx_begin) {
        report("requires graphics API version 8 or newer");
        return 1;
    }

    char path[256];

    pieg->resolve(argv[1], path, sizeof(path));

    int is_directory = 0;
    uint32_t file_size = 0;

    int error = pieg->fs_stat(path, &is_directory, &file_size);

    if (error || is_directory) {
        report("cannot open image file");
        return 1;
    }

    if (pieg->gfx_begin(&screen_w, &screen_h)) {
        report("could not initialize graphics");
        return 1;
    }

    int result = load_image(path);

    if (!result) {
        pieg->gfx_text(
            8,
            8,
            "EYE - press any key to close",
            COLOR_TEXT,
            1
        );

        pieg->gfx_present();

        while (!pieg->getchar()) {
            /* Wait for a key. */
        }
    }

    pieg->gfx_end();

    return result;
}
