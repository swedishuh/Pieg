#ifndef PIEG_API_H
#define PIEG_API_H

/*
 * Kernel services handed to every program in /PROGRAMS.
 *
 * Programs are position-independent ELF64 executables. The shell loads one
 * and calls its entry point as:
 *
 *     int pieg_start(const pieg_api_t *api, int argc, char **argv);
 *
 * Programs run in kernel context (no privilege separation yet). They must
 * not use anything outside this table. All fs_* functions return 0 on
 * success or a nonzero error code; fs_error() turns a code into text.
 * Paths are absolute; use resolve() to turn user input into one.
 */

#include <stddef.h>
#include <stdint.h>

#define PIEG_API_VERSION    9
#define PIEG_ATTR_DIRECTORY 0x10

/* Buffer sizes for kbd_info(). */
#define PIEG_KBD_CODE_MAX   12
#define PIEG_KBD_NAME_MAX   32

typedef void (*pieg_list_cb)(const char *name, uint8_t attributes,
                             uint32_t size, void *context);

typedef struct {
    int mounted;
    int writable;
    int pieg_volume;
    char type[8];
    char device[16];
    char label[16];
    uint64_t sectors;
} pieg_fsinfo_t;

typedef struct {
    char name[16];
    uint64_t bytes;
    int writable;
    int mounted;
} pieg_bdinfo_t;

typedef struct {
    uint64_t total;
    uint64_t used;
    uint64_t free;
    int usage_available;
} pieg_raminfo_t;

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t header_type;
    uint8_t irq_line;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar[6];            /* raw BAR registers, 0 when absent */
} pieg_pciinfo_t;

typedef struct {
    char name[16];
    uint64_t bytes;
    int state;                  /* 0 = unsupported, 1 = blank, 2 = valid GPT */
    uint64_t free_bytes;        /* space a new PIEG volume could use */
    int bus;                    /* 0 = ATA, 1 = SATA, 2 = USB */
    int boot;                   /* 1 = this disk holds the live boot image */
} pieg_diskinfo_t;

typedef struct {
    char name[16];              /* e.g. "usb0p1" */
    uint64_t start;             /* first sector */
    uint64_t bytes;
    char fstype[8];             /* "FAT32", "-" when not FAT */
    char label[16];
} pieg_partinfo_t;

typedef struct pieg_api {
    uint32_t version;
    const char *shell_version;

    /* Console. getchar() is non-blocking and returns 0 when no key is ready. */
    void (*putchar)(char c);
    void (*write)(const char *text);
    char (*getchar)(void);
    void (*clear)(void);

    /* Working directory and path handling. */
    const char *(*cwd)(void);
    void (*resolve)(const char *arg, char *out, size_t capacity);

    /* Filesystem. */
    int (*fs_list)(const char *path, pieg_list_cb callback, void *context);
    int (*fs_stat)(const char *path, int *is_directory, uint32_t *size);
    int (*fs_read)(const char *path, uint32_t offset, void *buffer,
                   uint32_t length, uint32_t *done);
    int (*fs_write)(const char *path, const void *data, uint32_t length,
                    int append);
    int (*fs_create)(const char *path);
    int (*fs_mkdir)(const char *path);
    int (*fs_remove)(const char *path);
    int (*fs_df)(uint64_t *total_bytes, uint64_t *free_bytes);
    const char *(*fs_error)(int error);

    /* System information. fs_info/ram_info return 0 when unavailable. */
    int (*fs_info)(pieg_fsinfo_t *info);
    unsigned int (*blockdev_count)(void);
    int (*blockdev_info)(unsigned int index, pieg_bdinfo_t *info);
    int (*ram_info)(pieg_raminfo_t *info);

    /* Machine control. */
    __attribute__((noreturn)) void (*panic)(const char *reason,
                                            const char *details);
    __attribute__((noreturn)) void (*reboot)(void);
    __attribute__((noreturn)) void (*halt)(void);

    /* Added in API version 2 (programs built for version 1 never touch these). */
    unsigned int (*pci_count)(void);
    int (*pci_info)(unsigned int index, pieg_pciinfo_t *info);

    /*
     * Added in API version 3: persistent storage. Disks are the physical
     * ATA/SATA disks seen at boot. disk_install() creates a PIEG volume in
     * free space (size_mib 0 = all of it, at most 4 GiB), copies the
     * running system onto it and returns NULL, or an error message.
     */
    unsigned int (*disk_count)(void);
    int (*disk_info)(unsigned int index, pieg_diskinfo_t *info);
    const char *(*disk_install)(unsigned int index, uint32_t size_mib);

    /*
     * Added in API version 4. user_mode is 1 when the program runs in ring 3
     * behind int 0x80 (then fs_list is built by libpieg from fs_list_raw,
     * because callbacks cannot cross rings). Records: u32 size, u8 attr,
     * u8 name length, name, NUL.
     */
    int user_mode;
    int (*fs_list_raw)(const char *path, void *buf, uint32_t cap, uint32_t *used);

    /* Added in API version 5: PC speaker tone control. */
    int (*sound_tone)(uint32_t frequency);
    void (*sound_stop)(void);

    /* Added in API version 6: partitions of disk `disk` (0 when n is past the end). */
    int (*disk_part_info)(unsigned int disk, unsigned int n,
                          pieg_partinfo_t *info);

    /*
     * Added in API version 7: keyboard layouts.
     *
     * kbd_load() reads a layout file (absolute path) and makes it the active
     * layout. It returns 0, or (line << 8) | kind: kind 1 file unreadable,
     * 2 too large, 3 syntax error, 4 bad scancode, 5 bad character. A failed
     * load leaves the current layout untouched.
     *
     * kbd_info() copies the active layout's short name ("us", "de", ...)
     * into code (PIEG_KBD_CODE_MAX bytes) and its display name into name
     * (PIEG_KBD_NAME_MAX bytes). Either may be NULL.
     */
    int (*kbd_load)(const char *path);
    int (*kbd_info)(char *code, char *name);

    /*
     * Added in API version 8: graphics. gfx_begin() takes the screen from the
     * console (returns 0 and the size, or -1 when there is no framebuffer);
     * gfx_end() returns it and repaints the console. Colors are 0xRRGGBB.
     * Nothing shows until gfx_present(). gfx_text() uses the 5x7 font at an
     * integer scale (6*scale pixels per character). gfx_ms() is a millisecond
     * clock that wraps at 2^32; call it at least every 50 ms.
     *
     * getchar() also returns 0x82 (Up), 0x83 (Down) and 0x1B (Esc) now.
     */
    int (*gfx_begin)(uint32_t *width, uint32_t *height);
    void (*gfx_end)(void);
    void (*gfx_rect)(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     uint32_t rgb);
    void (*gfx_text)(uint32_t x, uint32_t y, const char *text, uint32_t rgb,
                     uint32_t scale);
    void (*gfx_present)(void);
    uint32_t (*gfx_ms)(void);

    /*
     * Added in API version 9: pointer. mouse_state() returns 0 when there is
     * no mouse or touchpad, otherwise a mask of the sources found (1 = PS/2,
     * 2 = USB). x, y are screen pixels (the same as gfx_begin() reports),
     * buttons has bit 0 = left, 1 = right, 2 = middle. Any pointer may be
     * NULL. The kernel draws the cursor; programs only read the state, so
     * call it (or getchar()) regularly.
     */
    int (*mouse_state)(int *x, int *y, int *buttons);
} pieg_api_t;

#endif
