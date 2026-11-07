
#include "libpieg.h"

#define MAX_FILE_SIZE 8192
#define MAX_PATH_SIZE 256

static char file_buf[MAX_FILE_SIZE];
static char path_buf[MAX_PATH_SIZE];

static uint32_t buf_len = 0;
static int modified = 0;

/* Print the editor screen. */
static void draw_interface(const char *filename)
{
    pieg->clear();

    p_puts(" Microcephaly (mc)");
    if (modified)
        p_puts(" [+]");

    p_puts(" | File: ");
    p_putln(filename);

    p_putln("------------------------------------------------------------");

    if (buf_len > 0) {
        file_buf[buf_len] = '\0';
        p_puts(file_buf);
    }

    p_putln("");
    p_putln("------------------------------------------------------------");
    p_putln("^X Exit | ^S Save | Type text to append");
    p_putln("Backspace: Delete last character");
    p_puts("Bytes: ");
    p_u64(buf_len);
    p_putln("");
}

/* Save the current file. */
static int save_file(void)
{
    int result = pieg->fs_write(path_buf, file_buf, buf_len, 0);

    if (result == 0) {
        modified = 0;
        return 0;
    }

    return result;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        p_putln("Usage: mc <filename>");
        return 1;
    }

    pieg->resolve(argv[1], path_buf, sizeof(path_buf));

    uint32_t bytes_read = 0;

    int result = pieg->fs_read(
        path_buf,
        0,
        file_buf,
        MAX_FILE_SIZE - 1,
        &bytes_read
    );

    if (result == 0) {
        buf_len = bytes_read;

        if (buf_len >= MAX_FILE_SIZE)
            buf_len = MAX_FILE_SIZE - 1;
    } else {
        buf_len = 0;
    }

    file_buf[buf_len] = '\0';

    while (1) {
        draw_interface(argv[1]);

        char c = p_getc();

        /* Ctrl+X: Exit */
        if ((unsigned char)c == 24) {
            if (modified) {
                p_putln("");
                p_putln("Unsaved changes. Press S to save, X to discard,");
                p_putln("or any other key to continue editing.");

                char answer = p_getc();

                if (answer == 's' || answer == 'S') {
                    result = save_file();

                    if (result != 0) {
                        p_puts("Save failed: ");
                        p_putln(pieg->fs_error(result));
                        p_getc();
                        continue;
                    }
                } else if (answer != 'x' && answer != 'X') {
                    continue;
                }
            }

            pieg->clear();
            p_putln("Microcephaly closed.");
            return 0;
        }

        /* Ctrl+S: Save */
        if ((unsigned char)c == 19) {
            result = save_file();

            if (result != 0) {
                p_puts("Save failed: ");
                p_putln(pieg->fs_error(result));
                p_getc();
            }

            continue;
        }


        /* Backspace */
        if ((unsigned char)c == 8 || (unsigned char)c == 127) {
            if (buf_len > 0) {
                buf_len--;
                file_buf[buf_len] = '\0';
                modified = 1;
            }

            continue;
        }

        /* Normal text */
        if (c == '\r')
            c = '\n';

        if ((unsigned char)c >= 32 || c == '\n') {
            if (buf_len < MAX_FILE_SIZE - 1) {
                file_buf[buf_len++] = c;
                file_buf[buf_len] = '\0';
                modified = 1;
            }
        }
    }
}
