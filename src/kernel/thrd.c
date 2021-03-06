/**
 * @file thrd.c
 * @version 1.0
 *
 * @section License
 * Copyright (C) 2014-2015, Erik Moqvist
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * This file is part of the Simba project.
 */

#include "simba.h"

/* Thread states. */
#define THRD_STATE_CURRENT      0
#define THRD_STATE_READY        1
#define THRD_STATE_SUSPENDED    2
#define THRD_STATE_RESUMED      3
#define THRD_STATE_TERMINATED   4

/* Stack usage and debugging. */
#define THRD_STACK_LOW_MAGIC 0x1337
#define THRD_FILL_PATTERN 0x19

#if !defined(THRD_MONITOR_PRIO)
#    define THRD_MONITOR_PRIO -80
#endif

static char *state_fmt[] = {
    "current",
    "ready",
    "suspended",
    "resumed",
    "terminated"
};

FS_COMMAND_DEFINE("/kernel/thrd/list", thrd_cmd_list);
FS_COMMAND_DEFINE("/kernel/thrd/set_log_mask", thrd_cmd_set_log_mask);
FS_COMMAND_DEFINE("/kernel/thrd/monitor/set_period_ms", thrd_cmd_monitor_set_period_ms);
FS_COMMAND_DEFINE("/kernel/thrd/monitor/set_print", thrd_cmd_monitor_set_print);

struct thrd_scheduler_t {
    struct thrd_t *current_p;
    struct thrd_t *ready_p;
};

struct monitor_t {
    long period_us;
    int print;
};

static volatile struct thrd_scheduler_t scheduler = {
    .current_p = NULL,
    .ready_p = NULL
};

static struct monitor_t monitor = {
    .period_us = 2000000,
    .print = 0
};

/* Forward declarations for thrd_port. */
static void scheduler_ready_push(struct thrd_t *thrd_p);
static void thrd_reschedule(void);
static void terminate(void);

#include "thrd_port.i"

/* Stacks. */
static THRD_STACK(idle_thrd_stack, THRD_IDLE_STACK_MAX);
static THRD_STACK(monitor_thrd_stack, THRD_MONITOR_STACK_MAX);

static void terminate(void)
{
    /* The thread is terminated. */
    sys_lock();
    thrd_self()->state = THRD_STATE_TERMINATED;
    thrd_reschedule();
    sys_unlock();
}

/**
 * Push a thread on the list of threads that are ready to be
 * scheduled. The ready list is a linked list with the highest
 * priority thread in the first element. The pushed thread is added
 * _after_ any already pushed threads with the same priority.
 *
 * @param[in] thrd_p Thread to push to the the ready list.
 */
static void scheduler_ready_push(struct thrd_t *thrd_p)
{
    struct thrd_t *ready_p;

    /* Add in prio order, with highest prio first. */
    ready_p = scheduler.ready_p;

    while (ready_p != NULL) {
        if (thrd_p->prio < ready_p->prio) {
            /* Insert before the 'ready_p' thrd. */
            if (ready_p->prev_p != NULL) {
                ready_p->prev_p->next_p = thrd_p;
            } else {
                scheduler.ready_p = thrd_p;
            }

            thrd_p->prev_p = ready_p->prev_p;
            thrd_p->next_p = ready_p;
            ready_p->prev_p = thrd_p;
            return;
        }

        /* End of ready list. */
        if (ready_p->next_p == NULL) {
            ready_p->next_p = thrd_p;
            thrd_p->prev_p = ready_p;
            thrd_p->next_p = NULL;
            return;
        }

        ready_p = ready_p->next_p;
    }

    /* Empty list. */
    scheduler.ready_p = thrd_p;
    thrd_p->prev_p = NULL;
    thrd_p->next_p = NULL;
}

/**
 * Pop the most important thread from the ready list.
 */
static struct thrd_t *scheduler_ready_pop(void)
{
    struct thrd_t *thrd_p;

    thrd_p = scheduler.ready_p;
    scheduler.ready_p = thrd_p->next_p;

    if (scheduler.ready_p != NULL) {
        scheduler.ready_p->prev_p = NULL;
    }

    thrd_p->prev_p = NULL;
    thrd_p->next_p = NULL;

    return (thrd_p);
}

/**
 * Perform a rescheduling to let the currently most improtant thread
 * to run.
 *
 * This function must be called with the system lock taken or from an
 * isr (if the system is pre-emptive).
 */
static void thrd_reschedule(void)
{
    struct thrd_t *in_p, *out_p;

    out_p = thrd_self();

    ASSERTN(out_p->stack_low_magic == THRD_STACK_LOW_MAGIC, ESTACK);

    in_p = scheduler_ready_pop();

    /* Swap threads. */
    in_p->state = THRD_STATE_CURRENT;

    if (in_p != out_p) {
        scheduler.current_p = in_p;
        thrd_port_cpu_usage_stop(out_p);
        thrd_port_swap(in_p, out_p);
        thrd_port_cpu_usage_start(out_p);
    }
}

#if !defined(NPROFILESTACK)

extern char __main_stack_end;

static void thrd_fill_pattern(char *from_p, size_t size)
{
  size_t i;

  for (i = 0; i < size; i++) {
    from_p[i] = THRD_FILL_PATTERN;
  }
}

static int thrd_get_used_stack(struct thrd_t *thrd_p)
{
  char *stack_p;
  size_t i;

  stack_p = (char *)&thrd_p[1];
  i = 0;

  /* Stack grows towards lower memory addresses, so start from the
     bottom.*/
  while ((i < thrd_p->stack_size) &&
         (stack_p[i] == THRD_FILL_PATTERN)) {
      i++;
  }

  return (thrd_p->stack_size - i);
}

#endif

static void thrd_list_thrd(struct thrd_t *thrd_p, chan_t *chout_p)
{
    struct thrd_parent_t *child_p;
    struct list_sl_iterator_t iter;

    std_fprintf(chout_p,
                FSTR("%16s %16s %12s %5d %4u%%"
#if !defined(NPROFILESTACK)
                     "    %6d/%6d"
#endif
                     "     0x%02x\r\n"),
                thrd_p->name_p,
                thrd_p->parent.thrd_p != NULL ?
                thrd_p->parent.thrd_p->name_p : "",
                state_fmt[thrd_p->state], thrd_p->prio,
                (unsigned int)thrd_p->cpu.usage,
#if !defined(NPROFILESTACK)
                thrd_get_used_stack(thrd_p),
                (int)thrd_p->stack_size,
#endif
                thrd_p->log_mask);

    /* Children. */
    LIST_SL_ITERATOR_INIT(&iter, &thrd_p->children);

    while (1) {
        LIST_SL_ITERATOR_NEXT(&iter, &child_p);

        if (child_p == NULL) {
            break;
        }

        thrd_list_thrd(container_of(child_p, struct thrd_t, parent), chout_p);
    }
}

int thrd_cmd_list(int argc,
                  const char *argv[],
                  chan_t *chout,
                  chan_t *chin,
                  char *name)
{
    std_fprintf(chout,
                FSTR("            NAME           PARENT        STATE  PRIO   CPU"
#if !defined(NPROFILESTACK)
                     "  MAX-STACK-USAGE"
#endif
                     "  LOGMASK\r\n"));
    thrd_list_thrd(&main_thrd, chout);

    return (0);
}

static struct thrd_t *get_by_name(struct thrd_t *thrd_p,
                                  const char *name_p)
{
    struct thrd_parent_t *child_p;
    struct list_sl_iterator_t iter;

    /* Thread found? */
    if (strcmp(thrd_p->name_p, name_p) == 0) {
        return (thrd_p);
    }

    /* Iterate over children. */
    LIST_SL_ITERATOR_INIT(&iter, &thrd_p->children);

    thrd_p = NULL;

    while (thrd_p == NULL) {
        LIST_SL_ITERATOR_NEXT(&iter, &child_p);

        if (child_p == NULL) {
            break;
        }

        thrd_p = get_by_name(container_of(child_p, struct thrd_t, parent), name_p);
    }

    return (thrd_p);
}

static struct thrd_t *thrd_get_by_name(const char *name)
{
    return (get_by_name(&main_thrd, name));
}

int thrd_cmd_set_log_mask(int argc,
                          const char *argv[],
                          chan_t *chout_p,
                          chan_t *chin_p,
                          char *name_p)
{
    struct thrd_t *thrd_p;
    long mask;

    if (argc != 3) {
        std_fprintf(chout_p, FSTR("Usage: set_log_mask <thread name> <log mask>\r\n"));
        return (-EINVAL);
    }

    thrd_p = thrd_get_by_name(argv[1]);

    if (thrd_p == NULL) {
        return (-ESRCH);
    }

    if (std_strtol(argv[2], &mask) != 0) {
        return (-EINVAL);
    }

    thrd_p->log_mask = mask;

    return (0);
}

int thrd_cmd_monitor_set_period_ms(int argc,
                                   const char *argv[],
                                   chan_t *out_p,
                                   chan_t *in_p)
{
    long ms;

    if (argc != 2) {
        std_fprintf(out_p, FSTR("Usage: set_period_ms <milliseconds>\r\n"));

        return (-EINVAL);
    }

    if (std_strtol(argv[1], &ms) != 0) {
        return (-EINVAL);
    }

    monitor.period_us = (1000 * ms);

    return (0);
}

int thrd_cmd_monitor_set_print(int argc,
                               const char *argv[],
                               chan_t *out_p,
                               chan_t *in_p)
{
    long print;

    if (argc != 2) {
        goto err_inval;
    }

    if (std_strtol(argv[1], &print) != 0) {
        goto err_inval;
    }

    if ((print != 0) && (print != 1)) {
        goto err_inval;
    }

    monitor.print = print;

    return (0);

err_inval:
    std_fprintf(out_p, FSTR("Usage: set_print <1/0>\r\n"));

    return (-EINVAL);
}

static void *idle_thrd(void *arg_p)
{
    struct thrd_t *thrd_p;

    thrd_set_name("idle");

    thrd_p = thrd_self();

    while (1) {
        thrd_port_idle_wait(thrd_p);
    }

    return (NULL);
}

static void update_cpu_usage(struct thrd_t *thrd_p,
                             int print)
{
    struct thrd_parent_t *child_p;
    struct list_sl_iterator_t iter;

    thrd_p->cpu.usage = thrd_port_cpu_usage_get(thrd_p);
    thrd_port_cpu_usage_reset(thrd_p);

    if (print == 1) {
        std_printf(FSTR("%20s %10f%%\r\n"), thrd_p->name_p, thrd_p->cpu.usage);
    }

    /* Children. */
    LIST_SL_ITERATOR_INIT(&iter, &thrd_p->children);

    while (1) {
        LIST_SL_ITERATOR_NEXT(&iter, &child_p);

        if (child_p == NULL) {
            break;
        }

        update_cpu_usage(container_of(child_p, struct thrd_t, parent),
                         print);
    }
}

/**
 * The monitor thread monitors the cpu usage of all threads.
 */
static void *monitor_thrd(void *arg_p)
{
    int print;
    float irq_usage;

    thrd_set_name("monitor");

    while (1) {
        thrd_usleep(monitor.period_us);
        print = monitor.print;

        if (print == 1) {
            irq_usage = sys_interrupt_cpu_usage_get();
            sys_interrupt_cpu_usage_reset();
            std_printf(FSTR("\r\n                NAME         CPU\r\n"
                            "                 irq %10f%%\r\n"),
                       irq_usage);
        }

        update_cpu_usage(&main_thrd, print);
    }

    return (NULL);
}

int thrd_module_init(void)
{
#if !defined(NPROFILESTACK)
    char dummy = 0;
#endif

    /* Main function becomes a thrd. */
    main_thrd.prev_p = NULL;
    main_thrd.next_p = NULL;
    main_thrd.prio = 0;
    main_thrd.state = THRD_STATE_CURRENT;
    main_thrd.err = 0;
    main_thrd.log_mask = LOG_UPTO(NOTICE);
    main_thrd.name_p = "main";
    main_thrd.parent.thrd_p = NULL;
    LIST_SL_INIT(&main_thrd.children);
    main_thrd.cpu.usage = 0;
#if !defined(NASSERT)
    main_thrd.stack_low_magic = THRD_STACK_LOW_MAGIC;
#endif
#if !defined(NPROFILESTACK)
    main_thrd.stack_size = (&__main_stack_end - (char *)(&main_thrd + 1));
    thrd_fill_pattern((char *)(&main_thrd + 1),
                      &dummy - (char *)(&main_thrd + 2));
#endif
    thrd_port_init_main(&main_thrd.port);
    scheduler.current_p = &main_thrd;

    thrd_spawn(idle_thrd, NULL, 127, idle_thrd_stack, sizeof(idle_thrd_stack));
    thrd_spawn(monitor_thrd,
               NULL,
               THRD_MONITOR_PRIO,
               monitor_thrd_stack,
               sizeof(monitor_thrd_stack));

    return (0);
}

struct thrd_t *thrd_spawn(void *(*entry)(void *),
                          void *arg_p,
                          int prio,
                          void *stack_p,
                          size_t stack_size)
{
    struct thrd_t *thrd_p;
    int err = 0;

    /* Initialize thrd structure in the beginning of the stack. */
    thrd_p = stack_p;
    thrd_p->prev_p = NULL;
    thrd_p->next_p = NULL;
    thrd_p->prio = prio;
    thrd_p->state = THRD_STATE_READY;
    thrd_p->err = 0;
    thrd_p->log_mask = LOG_UPTO(NOTICE);
    thrd_p->name_p = "";
    thrd_p->parent.thrd_p = thrd_self();
    LIST_SL_INIT(&thrd_p->children);
    thrd_p->cpu.usage = 0.0f;
#if !defined(NASSERT)
    thrd_p->stack_low_magic = THRD_STACK_LOW_MAGIC;
#endif
#if !defined(NPROFILESTACK)
    thrd_p->stack_size = (stack_size - sizeof(*thrd_p));
    thrd_fill_pattern((char *)(thrd_p + 1), thrd_p->stack_size);
#endif
    LIST_SL_ADD_TAIL(&thrd_p->parent.thrd_p->children, &thrd_p->parent);

    err = thrd_port_spawn(thrd_p, entry, arg_p, stack_p, stack_size);

    sys_lock();
    scheduler_ready_push(thrd_p);
    sys_unlock();

    return (err == 0 ? stack_p : NULL);
}

int thrd_suspend(struct time_t *timeout_p)
{
    int err;

    sys_lock();
    err = thrd_suspend_isr(timeout_p);
    sys_unlock();

    return (err);
}

int thrd_resume(struct thrd_t *thrd_p, int err)
{
    sys_lock();
    thrd_resume_isr(thrd_p, err);
    sys_unlock();

    return (0);
}

int thrd_resume_isr(struct thrd_t *thrd_p, int err)
{
    thrd_p->err = err;

    if (thrd_p->state == THRD_STATE_SUSPENDED) {
        thrd_p->state = THRD_STATE_READY;
        scheduler_ready_push(thrd_p);
    } else if (thrd_p->state != THRD_STATE_TERMINATED) {
        thrd_p->state = THRD_STATE_RESUMED;
    }

    return (0);
}

int thrd_wait(struct thrd_t *thrd_p)
{
    while (1) {
        sys_lock();

        if (thrd_p->state == THRD_STATE_TERMINATED) {
            sys_unlock();
            break;
        }

        sys_unlock();

        thrd_usleep(50000);
    }

    return (0);
}

int thrd_usleep(long useconds)
{
    struct time_t timeout;
    int err;

    timeout.seconds = (useconds / 1000000);
    timeout.nanoseconds = 1000 * (useconds % 1000000);
    err = thrd_suspend(&timeout);

    return (err == -ETIMEDOUT ? 0 : -1);
}

struct thrd_t *thrd_self(void)
{
    return (scheduler.current_p);
}

int thrd_set_name(const char *name_p)
{
    thrd_self()->name_p = name_p;

    return (0);
}

int thrd_set_log_mask(struct thrd_t *thrd_p, int mask)
{
    int old;

    old = thrd_p->log_mask;
    thrd_p->log_mask = mask;

    return (old);
}

int thrd_get_log_mask(void)
{
    return (scheduler.current_p->log_mask);
}

void thrd_tick(void)
{
    thrd_port_tick();
}

int thrd_suspend_isr(struct time_t *timeout_p)
{
    struct thrd_t *thrd_p;
    struct timer_t timer;

    thrd_p = thrd_self();

    /* Immediatly return if the thread is already resumed. */
    if (thrd_p->state == THRD_STATE_RESUMED) {
        thrd_p->state = THRD_STATE_READY;
        scheduler_ready_push(thrd_p);
    } else {
        thrd_p->state = THRD_STATE_SUSPENDED;

        if (timeout_p != NULL) {
            if ((timeout_p->seconds == 0) && (timeout_p->nanoseconds == 0)) {
                return (-ETIMEDOUT);
            } else {
                timer_set_isr(&timer,
                              timeout_p,
                              thrd_port_suspend_timer_callback,
                              thrd_p,
                              0);
            }
        }
    }

    thrd_reschedule();

    return (thrd_p->err);
}
