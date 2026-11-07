#include "sched.h"

/*
 * Cooperative round-robin scheduler for the PIEG kernel.
 * Tasks are plain functions that do a slice of work and return; each pass
 * runs every due task once, in slot order.
 */

extern void vga_putchar(char c);
extern void vga_write(const char *text);

typedef enum {
    TASK_FREE = 0,
    TASK_READY,
    TASK_PAUSED
} task_state_t;

typedef struct {
    task_state_t state;
    sched_fn_t   fn;
    void        *arg;
    uint32_t     period;
    uint64_t     next_due;
    uint64_t     runs;
    char         name[SCHED_NAME_MAX];
} task_t;

static task_t   tasks[SCHED_MAX_TASKS];
static uint64_t round_counter;
static int      running = -1;

static int valid(int id)
{
    return id >= 0 && id < SCHED_MAX_TASKS && tasks[id].state != TASK_FREE;
}

void sched_init(void)
{
    for (int i = 0; i < SCHED_MAX_TASKS; i++)
        tasks[i].state = TASK_FREE;

    round_counter = 0;
    running = -1;

    vga_write("[SCHED] Round-robin scheduler ready.\n");
}

int sched_add(const char *name, sched_fn_t fn, void *arg, uint32_t period)
{
    if (!fn)
        return -1;

    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        task_t *t = &tasks[i];

        if (t->state != TASK_FREE)
            continue;

        t->state = TASK_READY;
        t->fn = fn;
        t->arg = arg;
        t->period = period ? period : 1;
        t->next_due = round_counter;
        t->runs = 0;

        const char *src = name ? name : "task";
        int n = 0;

        while (src[n] && n < SCHED_NAME_MAX - 1) {
            t->name[n] = src[n];
            n++;
        }

        t->name[n] = '\0';
        return i;
    }

    return -1;
}

int sched_remove(int id)
{
    if (!valid(id))
        return -1;

    tasks[id].state = TASK_FREE;
    return 0;
}

int sched_pause(int id)
{
    if (!valid(id))
        return -1;

    tasks[id].state = TASK_PAUSED;
    return 0;
}

int sched_resume(int id)
{
    if (!valid(id))
        return -1;

    if (tasks[id].state == TASK_PAUSED) {
        tasks[id].state = TASK_READY;
        tasks[id].next_due = round_counter;
    }

    return 0;
}

int sched_current(void)
{
    return running;
}

uint32_t sched_active(void)
{
    uint32_t n = 0;

    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state == TASK_READY)
            n++;
    }

    return n;
}

uint32_t sched_step(void)
{
    uint32_t ran = 0;

    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        task_t *t = &tasks[i];

        if (t->state != TASK_READY || t->next_due > round_counter)
            continue;

        running = i;
        int result = t->fn(t->arg);
        running = -1;

        ran++;

        /* The task may have removed or paused itself. */
        if (t->state == TASK_FREE)
            continue;

        t->runs++;

        if (result == SCHED_DONE)
            t->state = TASK_FREE;
        else
            t->next_due = round_counter + t->period;
    }

    round_counter++;
    return ran;
}

uint64_t sched_run(uint64_t max_rounds)
{
    uint64_t rounds = 0;

    while (sched_active() > 0 && (max_rounds == 0 || rounds < max_rounds)) {
        sched_step();
        rounds++;
    }

    return rounds;
}

__attribute__((noreturn))
void sched_start(void)
{
    sched_run(0);

    vga_write("[SCHED] No ready tasks, halting.\n");

    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* Prints value and returns the number of characters written. */
static int print_u64(uint64_t value)
{
    char digits[20];
    int n = 0;

    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);

    int count = n;

    while (n)
        vga_putchar(digits[--n]);

    return count;
}

static int print_str(const char *s)
{
    int n = 0;

    for (; s[n]; n++)
        vga_putchar(s[n]);

    return n;
}

static void pad(int used, int width)
{
    for (; used < width; used++)
        vga_putchar(' ');
}

void sched_list(void)
{
    vga_write("ID  STATE   RUNS      PERIOD  NAME\n");

    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        task_t *t = &tasks[i];

        if (t->state == TASK_FREE)
            continue;

        pad(print_u64((uint64_t)i), 4);
        pad(print_str(t->state == TASK_READY ? "ready" : "paused"), 8);
        pad(print_u64(t->runs), 10);
        pad(print_u64(t->period), 8);
        vga_write(t->name);
        vga_putchar('\n');
    }

    vga_write("Round: ");
    print_u64(round_counter);
    vga_write("  Active: ");
    print_u64(sched_active());
    vga_putchar('\n');
}
