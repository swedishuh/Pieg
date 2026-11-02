#ifndef PIEG_USERMODE_H
#define PIEG_USERMODE_H

#include <stdint.h>
#include "api.h"

/*
 * One private 2 MiB page, 2 MiB aligned, and the ONLY memory ring 3 may
 * touch. Layout:
 *   0x000000  program image (max 1 MiB)
 *   0x100000  syscall thunks     0x101000  api table
 *   0x102000  returned strings   0x103000  argv
 *   0x110000..end  user stack
 */
#define USER_REGION_SIZE (2u * 1024u * 1024u)
extern uint8_t user_region[USER_REGION_SIZE];

/* GDT/TSS/IDT, int 0x80 syscalls, and the user-accessible page mapping. */
void arch_init(void);

/* Runs a loaded program: ring 0 in kernel mode, ring 3 in default mode. */
int usermode_call(uint64_t entry, const pieg_api_t *kapi, int argc, char **argv);

/* Directory listing without a callback (callbacks can't cross rings). */
int usermode_fs_list_raw(const char *path, void *buf, uint32_t cap, uint32_t *used);

#endif
