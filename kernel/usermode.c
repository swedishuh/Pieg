#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "api.h"
#include "fsinit.h"
#include "login.h"
#include "mem.h"
#include "panic.h"
#include "usermode.h"

extern void vga_write(const char *text);
extern uint64_t pd_table[];             /* entry.asm: 2 MiB page directory */
extern uint64_t isr_table[];            /* trap.asm: 32 exceptions + int 0x80 */
extern int64_t usermode_run(uint64_t entry, uint64_t user_sp,
                            uint64_t a0, uint64_t a1, uint64_t a2);
extern __attribute__((noreturn)) void usermode_abort(int64_t status);

uint8_t user_region[USER_REGION_SIZE]
    __attribute__((section(".bss.userregion"), aligned(2097152)));

#define THUNK_OFF 0x100000u
#define API_OFF   0x101000u
#define STR_OFF   0x102000u
#define VER_OFF   0x102400u
#define ARGV_OFF  0x103000u

/* ================================================================
 * GDT / TSS / IDT / paging
 * ================================================================ */

typedef struct __attribute__((packed)) { uint16_t limit; uint64_t base; } dtr_t;

typedef struct __attribute__((packed)) {
    uint32_t r0;
    uint64_t rsp[3];
    uint64_t r1;
    uint64_t ist[7];
    uint64_t r2;
    uint16_t r3;
    uint16_t iopb;                      /* == sizeof: no I/O bitmap, ring 3 has no ports */
} tss_t;

typedef struct __attribute__((packed)) {
    uint16_t lo;
    uint16_t sel;
    uint8_t ist;
    uint8_t type;
    uint16_t mid;
    uint32_t hi;
    uint32_t zero;
} idt_t;

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector, err;
    uint64_t rip, cs, rflags, rsp, ss;
} frame_t;

static uint64_t gdt[7] __attribute__((aligned(16)));
static tss_t tss;
static idt_t idt[256] __attribute__((aligned(16)));
static uint8_t trap_stack[65536] __attribute__((aligned(16)));

static void idt_set(unsigned int v, uint64_t addr, uint8_t type)
{
    idt[v].lo = (uint16_t)addr;
    idt[v].sel = 0x08;
    idt[v].ist = 0;
    idt[v].type = type;
    idt[v].mid = (uint16_t)(addr >> 16);
    idt[v].hi = (uint32_t)(addr >> 32);
    idt[v].zero = 0;
}

void arch_init(void)
{
    uint64_t base = (uint64_t)(uintptr_t)&tss;
    uint64_t lim = sizeof(tss) - 1;

    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFull;     /* 0x08 kernel code */
    gdt[2] = 0x00CF92000000FFFFull;     /* 0x10 kernel data */
    gdt[3] = 0x00CFF2000000FFFFull;     /* 0x18 user data   (0x1B) */
    gdt[4] = 0x00AFFA000000FFFFull;     /* 0x20 user code   (0x23) */
    gdt[5] = (lim & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) |
             (((lim >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;                /* 0x28 TSS */

    tss.rsp[0] = (uint64_t)(uintptr_t)trap_stack + sizeof(trap_stack);
    tss.iopb = sizeof(tss);

    dtr_t g = { sizeof(gdt) - 1, (uint64_t)(uintptr_t)gdt };

    __asm__ volatile ("lgdt %0" : : "m"(g));
    __asm__ volatile (
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "xorl %%eax, %%eax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        : : : "rax", "memory");
    __asm__ volatile ("ltr %%ax" : : "a"((uint16_t)0x28));

    for (unsigned int i = 0; i < 32; i++)
        idt_set(i, isr_table[i], 0x8E);

    idt_set(0x80, isr_table[32], 0xEE);     /* DPL 3: callable from ring 3 */

    dtr_t d = { sizeof(idt) - 1, (uint64_t)(uintptr_t)idt };

    __asm__ volatile ("lidt %0" : : "m"(d));

    /*
     * Paging: entry.asm identity-maps 4 GiB as supervisor-only 2 MiB pages.
     * Ring 3 needs the U/S bit at every level, but only user_region's own
     * page-directory entry gets it, so everything else stays kernel-only.
     */
    uint64_t cr3;

    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));

    uint64_t *pml4 = (uint64_t *)(uintptr_t)(cr3 & ~0xFFFull);
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[0] & 0x000FFFFFFFFFF000ull);

    pml4[0] |= 4;

    for (unsigned int i = 0; i < 4; i++)
        pdpt[i] |= 4;

    pd_table[(uintptr_t)user_region >> 21] |= 4;

    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* ================================================================
 * Syscalls (int 0x80). rax = number, args in rdi rsi rdx rcx r8 r9.
 * mask bit i = argument i is a pointer that must lie inside user_region.
 * ================================================================ */

#define SYSCALLS(X) \
    X(putchar, 0) X(write, 1) X(getchar, 0) X(clear, 0) X(cwd, 0) \
    X(resolve, 3) X(fs_list_raw, 11) X(fs_stat, 7) X(fs_read, 21) \
    X(fs_write, 3) X(fs_create, 1) X(fs_mkdir, 1) X(fs_remove, 1) \
    X(fs_df, 3) X(fs_error, 0) X(fs_info, 1) X(blockdev_count, 0) \
    X(blockdev_info, 2) X(ram_info, 1) X(panic, 3) X(reboot, 0) \
    X(halt, 0) X(pci_count, 0) X(pci_info, 2) X(disk_count, 0) \
    X(disk_info, 2) X(disk_install, 0) \
    X(sound_tone, 0) X(sound_stop, 0) \
    X(disk_part_info, 4) \
    X(kbd_load, 1) X(kbd_info, 3) \
    X(gfx_begin, 3) X(gfx_end, 0) X(gfx_rect, 0) X(gfx_text, 4) \
    X(gfx_present, 0) X(gfx_ms, 0) \
    X(mouse_state, 7)

enum {
    SYS_exit = 0,
#define X(n, m) SYS_##n,
    SYSCALLS(X)
#undef X
    SYS_COUNT
};

static const uint16_t sys_off[SYS_COUNT] = {
    0,
#define X(n, m) offsetof(pieg_api_t, n),
    SYSCALLS(X)
#undef X
};

static const uint8_t sys_mask[SYS_COUNT] = {
    0,
#define X(n, m) m,
    SYSCALLS(X)
#undef X
};

typedef uint64_t (*sysfn_t)(uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t);

static const pieg_api_t *kernel_api;

static bool in_user(uint64_t p)
{
    uint64_t b = (uint64_t)(uintptr_t)user_region;

    return p == 0 || (p >= b && p < b + USER_REGION_SIZE);
}

/* Kernel strings handed back to ring 3 are copied into user memory. */
static char *user_string(const char *s)
{
    static unsigned int slot;

    if (!s)
        return 0;

    char *d = (char *)user_region + STR_OFF + (slot++ & 3u) * 256u;
    size_t i = 0;

    while (s[i] && i < 255) {
        d[i] = s[i];
        i++;
    }

    d[i] = '\0';
    return d;
}

static void do_syscall(frame_t *f)
{
    uint64_t n = f->rax;

    if (n == SYS_exit)
        usermode_abort((int)f->rdi);

    if (n >= SYS_COUNT) {
        f->rax = (uint64_t)-1;
        return;
    }

    uint64_t a[6] = { f->rdi, f->rsi, f->rdx, f->rcx, f->r8, f->r9 };

    for (unsigned int i = 0; i < 6; i++) {
        if (((sys_mask[n] >> i) & 1u) && !in_user(a[i])) {
            f->rax = (uint64_t)-1;
            return;
        }
    }

    sysfn_t fn = *(const sysfn_t *)((const char *)kernel_api + sys_off[n]);
    uint64_t r = fn(a[0], a[1], a[2], a[3], a[4], a[5]);

    if (n == SYS_cwd || n == SYS_fs_error || n == SYS_disk_install)
        r = (uint64_t)(uintptr_t)user_string((const char *)(uintptr_t)r);

    f->rax = r;
}

/* ================================================================
 * Faults
 * ================================================================ */

static char msg[96];

static char *cat(char *o, const char *s)
{
    while (*s)
        *o++ = *s++;

    return o;
}

static char *dec(char *o, uint64_t v)
{
    char t[21];
    int n = 0;

    if (!v)
        t[n++] = '0';

    while (v) {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    }

    while (n)
        *o++ = t[--n];

    return o;
}

static char *hex(char *o, uint64_t v)
{
    static const char d[] = "0123456789abcdef";
    int s = 60;

    o = cat(o, "0x");

    while (s > 0 && !((v >> s) & 15))
        s -= 4;

    for (; s >= 0; s -= 4)
        *o++ = d[(v >> s) & 15];

    return o;
}

static const char *fault_name(uint64_t v)
{
    switch (v) {
    case 0:  return "divide error";
    case 6:  return "invalid opcode";
    case 13: return "protection fault";
    case 14: return "page fault";
    }

    return "CPU exception";
}

void trap_dispatch(frame_t *f)
{
    bool from_user = (f->cs & 3) == 3;

    if (f->vector == 0x80 && from_user) {
        do_syscall(f);
        return;
    }

    char *o = msg;

    if (from_user) {
        o = cat(o, "\n[user] program killed: ");
        o = cat(o, fault_name(f->vector));
        o = cat(o, " (vector ");
        o = dec(o, f->vector);
        o = cat(o, ") at rip ");
        o = hex(o, f->rip);

        if (f->vector == 14) {
            uint64_t cr2;

            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            o = cat(o, ", address ");
            o = hex(o, cr2);
        }

        o = cat(o, "\n");
        *o = '\0';
        vga_write(msg);
        usermode_abort((int64_t)(128 + f->vector));
    }

    o = cat(o, "vector ");
    o = dec(o, f->vector);
    o = cat(o, " error ");
    o = hex(o, f->err);
    o = cat(o, " rip ");
    o = hex(o, f->rip);
    *o = '\0';
    kernel_panic("CPU exception in kernel mode", msg);
}

/* ================================================================
 * Running a program
 * ================================================================ */

typedef int (*program_entry_t)(const pieg_api_t *, int, char **);

int usermode_call(uint64_t entry, const pieg_api_t *kapi, int argc, char **argv)
{
    if (login_mode() == LOGIN_KERNEL)
        return ((program_entry_t)(uintptr_t)entry)(kapi, argc, argv);

    kernel_api = kapi;

    uint8_t *base = user_region;
    uint8_t *t = base + THUNK_OFF;

    /* thunk 0: exit(status = rax of main). thunk n: mov eax,n; int 0x80; ret */
    static const uint8_t exit_code[] = { 0x48, 0x89, 0xC7, 0xB8, 0, 0, 0, 0, 0xCD, 0x80 };

    memcpy(t, exit_code, sizeof(exit_code));

    for (unsigned int n = 1; n < SYS_COUNT; n++) {
        uint8_t *p = t + 16u * n;

        p[0] = 0xB8; p[1] = (uint8_t)n; p[2] = 0; p[3] = 0; p[4] = 0;
        p[5] = 0xCD; p[6] = 0x80; p[7] = 0xC3;
    }

    pieg_api_t *u = (pieg_api_t *)(base + API_OFF);

    memcpy(u, kapi, sizeof(*u));

#define X(n, m) *(uint64_t *)((char *)u + offsetof(pieg_api_t, n)) = \
                    (uint64_t)(uintptr_t)(t + 16u * SYS_##n);
    SYSCALLS(X)
#undef X

    u->fs_list = 0;                     /* libpieg builds it on fs_list_raw */
    u->user_mode = 1;

    char *ver = (char *)base + VER_OFF;
    size_t k = 0;

    while (kapi->shell_version[k] && k < 63) {
        ver[k] = kapi->shell_version[k];
        k++;
    }

    ver[k] = '\0';
    u->shell_version = ver;

    /* argv strings and vector, copied into user memory. */
    char *s = (char *)base + ARGV_OFF;
    char **vec = (char **)(base + ARGV_OFF + 2048);
    size_t used = 0;
    int n = 0;

    for (; n < argc && n < 16; n++) {
        size_t len = 0;

        while (argv[n][len])
            len++;

        if (used + len + 1 > 2048)
            break;

        memcpy(s + used, argv[n], len + 1);
        vec[n] = s + used;
        used += len + 1;
    }

    vec[n] = 0;

    uint64_t sp = (uint64_t)(uintptr_t)base + USER_REGION_SIZE - 16 - 8;

    *(uint64_t *)(uintptr_t)sp = (uint64_t)(uintptr_t)t;     /* return -> exit thunk */

    return (int)usermode_run(entry, sp, (uint64_t)(uintptr_t)u,
                             (uint64_t)n, (uint64_t)(uintptr_t)vec);
}

/* ================================================================
 * Callback-free directory listing:  u32 size, u8 attr, u8 len, name\0
 * ================================================================ */

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t used;
} raw_ctx_t;

static void raw_cb(const char *name, uint8_t attr, uint32_t size, void *context)
{
    raw_ctx_t *r = (raw_ctx_t *)context;
    uint32_t nl = 0;

    while (name[nl] && nl < 255)
        nl++;

    if (r->used + 6 + nl + 1 > r->cap)
        return;

    uint8_t *p = r->buf + r->used;

    memcpy(p, &size, 4);
    p[4] = attr;
    p[5] = (uint8_t)nl;
    memcpy(p + 6, name, nl);
    p[6 + nl] = 0;
    r->used += 6 + nl + 1;
}

int usermode_fs_list_raw(const char *path, void *buf, uint32_t cap, uint32_t *used)
{
    raw_ctx_t r = { (uint8_t *)buf, cap, 0 };
    int e = (int)fs_list(path, raw_cb, &r);

    if (used)
        *used = r.used;

    return e;
}
