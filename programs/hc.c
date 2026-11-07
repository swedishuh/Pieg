
#include "libpieg.h"

/*
 * HIPPOCAMPUS
 * PIEG Disk Management Utility
 *
 * Commands:
 *
 * hippocampus
 * hippocampus list
 * hippocampus info DISK
 * hippocampus install DISK [MiB]
 * hippocampus help
 *
 * Current backend capabilities:
 * - Disk discovery
 * - Disk information
 * - GPT state detection
 * - Free-space reporting
 * - PIEG persistent volume installation
 *
 * Existing partitions are never intentionally modified
 * by the PIEG volume installation operation.
 */

#define HIPPOCAMPUS_VERSION "1.0"
#define MAX_VOLUME_MIB 4096u

static void print_header(void)
{
    p_putln("");
    p_putln("  HIPPOCAMPUS");
    p_putln("  PIEG Disk Management Utility");
    p_putln("  --------------------------------");
}

static void print_mib(uint64_t bytes)
{
    p_u64(bytes >> 20);
    p_puts(" MiB");
}

static void print_gib(uint64_t bytes)
{
    p_u64(bytes >> 30);
    p_puts(".");
    p_u64(((bytes >> 20) & 1023) * 100 / 1024);
    p_puts(" GiB");
}

static const char *bus_name(int bus)
{
    return bus == 2 ? "USB" : bus == 1 ? "SATA" : "ATA";
}

static void print_parts(unsigned int disk_index)
{
    pieg_partinfo_t p;

    for (unsigned int n = 0; pieg->disk_part_info(disk_index, n, &p); n++) {
        p_puts("   ");
        p_puts(p.name);
        p_puts("   ");
        print_mib(p.bytes);
        p_puts("   ");
        p_puts(p.fstype);

        if (p.label[0]) {
            p_puts("  ");
            p_puts(p.label);
        }

        p_putln("");
    }
}

static const char *disk_state(unsigned int state)
{
    switch (state) {
    case 1:
        return "Blank disk";
    case 2:
        return "GPT partition table";
    default:
        return "Unsupported partition layout";
    }
}

static int parse_mib(const char *s, uint32_t *out)
{
    uint32_t value = 0;

    if (!s || !*s)
        return 0;

    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return 0;

        if (value > MAX_VOLUME_MIB / 10)
            return 0;

        value = value * 10 + (uint32_t)(*s - '0');

        if (value > MAX_VOLUME_MIB)
            return 0;
    }

    if (!value)
        return 0;

    *out = value;
    return 1;
}

static void read_line(char *dst, size_t capacity)
{
    size_t n = 0;

    if (!capacity)
        return;

    for (;;) {
        char c = p_getc();

        if (c == '\r' || c == '\n') {
            p_putc('\n');
            break;
        }

        if ((c == '\b' || (unsigned char)c == 127) && n) {
            n--;
            p_putc('\b');
            p_putc(' ');
            p_putc('\b');
            continue;
        }

        if (c >= 32 && c < 127 && n + 1 < capacity) {
            dst[n++] = c;
            p_putc(c);
        }
    }

    dst[n] = '\0';
}

static int find_disk(const char *name, unsigned int *index,
                     pieg_diskinfo_t *info)
{
    unsigned int count = pieg->disk_count();

    for (unsigned int i = 0; i < count; i++) {
        pieg_diskinfo_t current;

        if (!pieg->disk_info(i, &current))
            continue;

        if (p_ieq(current.name, name)) {
            *index = i;
            *info = current;
            return 1;
        }
    }

    return 0;
}

/* ---------------------------------------------------------
 * DISK OVERVIEW
 * --------------------------------------------------------- */

static void disk_list(void)
{
    unsigned int count = pieg->disk_count();

    print_header();

    p_puts("Detected storage devices: ");
    p_u64(count);
    p_putln("");
    p_putln("");

    if (!count) {
        p_putln("No ATA/SATA disks detected.");
        return;
    }

    p_putln("DEVICE   BUS    CAPACITY       PARTITION TABLE");
    p_putln("------------------------------------------------");

    for (unsigned int i = 0; i < count; i++) {
        pieg_diskinfo_t d;

        if (!pieg->disk_info(i, &d))
            continue;

        p_puts(d.name);
        p_puts("     ");
        p_puts(bus_name(d.bus));
        p_puts("    ");

        print_mib(d.bytes);

        p_puts("       ");
        p_puts(disk_state(d.state));
        p_putln(d.boot ? "  [BOOT DEVICE]" : "");
        print_parts(i);

        if (d.state) {
            p_puts("             Available: ");
            print_mib(d.free_bytes);
            p_putln("");
        }

        p_putln("");
    }
}

/* ---------------------------------------------------------
 * DISK INSPECTION
 * --------------------------------------------------------- */

static int disk_info(const char *name)
{
    unsigned int index;
    pieg_diskinfo_t d;

    if (!find_disk(name, &index, &d)) {
        p_puts("Hippocampus: disk not found: ");
        p_putln(name);
        return 1;
    }

    print_header();

    p_puts("Device:          ");
    p_putln(d.name);

    p_puts("Device index:    ");
    p_u64(index);
    p_putln("");

    p_puts("Capacity:        ");
    print_mib(d.bytes);
    p_putln("");

    p_puts("Capacity (GiB):  ");
    print_gib(d.bytes);
    p_putln("");

    p_puts("Bus:             ");
    p_putln(bus_name(d.bus));

    if (d.boot)
        p_putln("Boot device:     yes (booted from this disk)");

    p_puts("Partition table: ");
    p_putln(disk_state(d.state));

    p_putln("Partitions:");
    print_parts(index);

    if (d.state) {
        p_puts("Free space:      ");
        print_mib(d.free_bytes);
        p_putln("");

        p_puts("Used space:      ");
        print_mib(d.bytes - d.free_bytes);
        p_putln("");
    }

    p_putln("");
    p_putln("End of disk information.");

    return 0;
}

/* ---------------------------------------------------------
 * PIEG VOLUME INSTALLATION
 * --------------------------------------------------------- */

static int disk_install(const char *name, uint32_t requested_mib)
{
    unsigned int index;
    pieg_diskinfo_t d;

    if (!find_disk(name, &index, &d)) {
        p_puts("Hippocampus: disk not found: ");
        p_putln(name);
        return 1;
    }

    if (d.boot) {
        p_putln("Refusing: this is the disk PIEG was booted from.");
        return 1;
    }

    if (!d.state) {
        p_putln("Unsupported partition layout.");
        p_putln("This disk cannot currently be used.");
        return 1;
    }

    uint64_t available_mib = d.free_bytes >> 20;

    uint32_t actual_mib = requested_mib;

    if (!actual_mib) {
        actual_mib = available_mib < MAX_VOLUME_MIB
            ? (uint32_t)available_mib
            : MAX_VOLUME_MIB;
    }

    if (!actual_mib || actual_mib > available_mib) {
        p_putln("Error: requested volume exceeds available space.");
        return 1;
    }

    print_header();

    p_putln("VOLUME INSTALLATION");
    p_putln("-------------------");

    p_puts("Target:          ");
    p_putln(d.name);

    p_puts("Disk capacity:   ");
    print_mib(d.bytes);
    p_putln("");

    p_puts("Available space: ");
    print_mib(d.free_bytes);
    p_putln("");

    p_puts("Volume size:     ");
    p_u64(actual_mib);
    p_putln(" MiB");

    p_putln("");
    p_putln("Operation:");
    p_putln("Create a FAT32 PIEG persistent volume.");
    p_putln("");
    p_putln("WARNING:");
    p_putln("This operation modifies the disk partition table.");
    p_putln("Back up important data before continuing.");
    p_putln("");

    p_puts("Type YES to confirm: ");

    char answer[8];
    read_line(answer, sizeof(answer));

    if (!p_streq(answer, "YES")) {
        p_putln("Operation cancelled.");
        return 1;
    }

    p_putln("");
    p_putln("Hippocampus: installing volume...");

    const char *err = pieg->disk_install(index, actual_mib);

    if (err) {
        p_puts("Installation failed: ");
        p_putln(err);
        return 1;
    }

    p_putln("");
    p_putln("Installation completed successfully.");
    p_putln("Your PIEG persistent volume is ready.");
    p_putln("Reboot to start using persistent storage.");

    return 0;
}

/* ---------------------------------------------------------
 * HELP
 * --------------------------------------------------------- */

static void print_help(void)
{
    print_header();

    p_putln("USAGE");
    p_putln("");
    p_putln("  hippocampus");
    p_putln("      Open disk overview");
    p_putln("");
    p_putln("  hippocampus list");
    p_putln("      List detected disks");
    p_putln("");
    p_putln("  hippocampus info DISK");
    p_putln("      Inspect a disk");
    p_putln("");
    p_putln("  hippocampus install DISK [MiB]");
    p_putln("      Create a PIEG persistent volume");
    p_putln("");
    p_putln("  hippocampus help");
    p_putln("      Show this help");
    p_putln("");
    p_puts("Version: ");
    p_putln(HIPPOCAMPUS_VERSION);
}

/* ---------------------------------------------------------
 * INTERACTIVE INTERFACE
 * --------------------------------------------------------- */

static void interactive(void)
{
    for (;;) {
        print_header();

        p_putln("1. Disk overview");
        p_putln("2. Inspect a disk");
        p_putln("3. Install PIEG persistent volume");
        p_putln("4. Help");
        p_putln("5. Exit");
        p_putln("");

        p_puts("hippocampus> ");

        char choice[16];
        read_line(choice, sizeof(choice));

        if (p_streq(choice, "1")) {
            disk_list();
        }
        else if (p_streq(choice, "2")) {
            p_puts("Disk name: ");

            char name[32];
            read_line(name, sizeof(name));

            disk_info(name);
        }
        else if (p_streq(choice, "3")) {
            p_puts("Disk name: ");

            char name[32];
            read_line(name, sizeof(name));

            p_puts("Volume size in MiB (0 = automatic): ");

            char size[16];
            read_line(size, sizeof(size));

            uint32_t mib = 0;

            if (!p_streq(size, "0") &&
                !parse_mib(size, &mib)) {
                p_putln("Invalid volume size.");
            }
            else {
                disk_install(name, mib);
            }
        }
        else if (p_streq(choice, "4")) {
            print_help();
        }
        else if (p_streq(choice, "5")) {
            p_putln("Leaving Hippocampus.");
            return;
        }
        else {
            p_putln("Unknown option.");
        }

        p_putln("");
        p_putln("Press ENTER to continue.");

        char unused[2];
        read_line(unused, sizeof(unused));
    }
}

/* ---------------------------------------------------------
 * ENTRY POINT
 * --------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        interactive();
        return 0;
    }

    if (p_ieq(argv[1], "list")) {
        disk_list();
        return 0;
    }

    if (p_ieq(argv[1], "info")) {
        if (argc < 3) {
            p_putln("Usage: hippocampus info DISK");
            return 1;
        }

        return disk_info(argv[2]);
    }

    if (p_ieq(argv[1], "install")) {
        if (argc < 3 || argc > 4) {
            p_putln("Usage: hippocampus install DISK [MiB]");
            return 1;
        }

        uint32_t mib = 0;

        if (argc == 4 && !parse_mib(argv[3], &mib)) {
            p_putln("Invalid size. Use 1-4096 MiB.");
            return 1;
        }

        return disk_install(argv[2], mib);
    }

    if (p_ieq(argv[1], "help") ||
        p_ieq(argv[1], "--help")) {
        print_help();
        return 0;
    }

    p_puts("Unknown command: ");
    p_putln(argv[1]);
    p_putln("Run 'hippocampus help' for usage.");

    return 1;
}
