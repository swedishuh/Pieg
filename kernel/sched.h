#ifndef SCHED_H
#define SCHED_H

#include <stddef.h>
#include <stdint.h>

#define SCHED_MAX_TASKS 16
#define SCHED_NAME_MAX  16

/* Task return values. */
#define SCHED_CONTINUE 0        /* run again on the next due round */
#define SCHED_DONE     1        /* one-shot: remove the task */

typedef int (*sched_fn_t)(void *arg);

void     sched_init(void);

/* period = rounds between runs (0 and 1 both mean every round).
 * Returns the task id, or -1 if the table is full or fn is null. */
int      sched_add(const char *name, sched_fn_t fn, void *arg, uint32_t period);
int      sched_remove(int id);
int      sched_pause(int id);
int      sched_resume(int id);

int      sched_current(void);   /* id of the running task, -1 outside one */
uint32_t sched_active(void);    /* tasks that are ready to run */

/* One pass over the task table. Returns the number of tasks that ran. */
uint32_t sched_step(void);

/* Runs until no ready task is left, or max_rounds passes (0 = unlimited).
 * Returns the rounds executed. */
uint64_t sched_run(uint64_t max_rounds);

/* Runs until no ready task is left, then halts the CPU. Never returns. */
__attribute__((noreturn))
void     sched_start(void);

void     sched_list(void);

#endif
