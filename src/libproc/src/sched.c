/* sched.c — which report the centre handles next (requirement R2).
 *
 * THE COURSE WROTE A PART OF THIS FILE: struct sched, take() and
 * sched_next(), which holds the three policies. Do not change that part.
 * You write the four functions under "YOURS" at the end of the file.
 *
 * Three policies. The differences between them are the lesson:
 *
 *   FCFS      the order of arrival. Simple and fair. An urgent contact
 *             behind 2000 satellite reports waits for all 2000.
 *
 *   PRIORITY  the highest class first: HIGH, then the P of the sensor,
 *             then routine (urgency_t). An urgent contact overtakes, as R2
 *             requires. The cost is starvation: while a higher class
 *             continues to arrive, the centre never serves a lower one.
 *
 *   RR        round robin over the sensors, one report from each in turn.
 *             No sensor can push out another, so SAT at 2000 reports each
 *             second cannot stop AUR at 5 each second. It does not help an
 *             urgent contact, which waits for the turn of its sensor.
 *
 * The three policies use one array of waiting reports, slots[0] to
 * slots[count - 1], in the order of arrival. Only the choice of the report
 * to remove is different.
 *
 * MAN PAGES. Each call of the C library and of Linux in this file, the
 * command that shows its manual page in a terminal, and the same page on
 * the web. The functions of the course have no man page: read them in
 * proc.h, syscall.h and common/cpu.h.
 *
 *   malloc(), calloc(),      man 3 malloc
 *   free()
 *       https://man7.org/linux/man-pages/man3/malloc.3.html
 *   errno, error names       man 3 errno
 *       https://man7.org/linux/man-pages/man3/errno.3.html
 *   memmove()                man 3 memmove
 *       https://man7.org/linux/man-pages/man3/memmove.3.html
 *   strcmp()                 man 3 strcmp
 *       https://man7.org/linux/man-pages/man3/strcmp.3.html
 */

#include "sentinel/proc.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ------------------------------------------------ GIVEN: do not change */

struct sched {
    sched_policy_t policy;     /* the policy of sched_create               */
    pending_t     *slots;      /* an array of capacity reports, from malloc */
    size_t         capacity;   /* the most reports that can wait            */
    size_t         count;      /* the reports that wait: slots[0] to
                                  slots[count - 1], in the order of arrival */

    /* Round robin must remember which sensor has the next turn. It starts
       at 0, which is NWS. */
    int            rr_next;
};

/* Remove slot i. The reports that stay keep their order of arrival.
 *
 * This uses memmove, not a swap with the last report. A swap is faster,
 * but it breaks the order of arrival, and FCFS is that order. */
static void take(sched_t *s, size_t i, pending_t *out)
{
    *out = s->slots[i];
    memmove(&s->slots[i], &s->slots[i + 1],
            (s->count - i - 1) * sizeof *s->slots);
    s->count--;
}

int sched_next(sched_t *s, pending_t *out)
{
    if (s == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (s->count == 0) {
        return 0;
    }

    switch (s->policy) {

    case POLICY_FCFS:
        /* The oldest first. The array is in the order of arrival. */
        take(s, 0, out);
        return 1;

    case POLICY_PRIORITY: {
        /* The oldest report of the highest class. The scan keeps the FIRST
           report of the best class that it finds, so each class keeps its
           order of arrival: FCFS among the HIGH reports, among the P
           reports, and among the routine reports. */
        size_t    best   = 0;
        urgency_t best_u = pending_urgency(&s->slots[0]);
        for (size_t i = 1; i < s->count && best_u != URGENCY_HIGH; i++) {
            urgency_t u = pending_urgency(&s->slots[i]);
            if (u > best_u) {
                best   = i;
                best_u = u;
            }
        }
        take(s, best, out);
        return 1;
    }

    case POLICY_RR: {
        /* One report from each sensor in turn. Start at the sensor that
           has the turn. Serve the first sensor that has a report waiting,
           and give the turn to the sensor after it.

           The loop runs SENSOR_COUNT times. If no report has the name of a
           sensor, take the oldest report. */
        for (int step = 0; step < SENSOR_COUNT; step++) {
            int want = (s->rr_next + step) % SENSOR_COUNT;
            const char *want_name = sensor_name((sensor_id_t)want);

            for (size_t i = 0; i < s->count; i++) {
                if (strcmp(s->slots[i].c.sensor, want_name) == 0) {
                    take(s, i, out);
                    s->rr_next = (want + 1) % SENSOR_COUNT;
                    return 1;
                }
            }
        }
        take(s, 0, out);
        return 1;
    }
    }

    errno = EINVAL;
    return -1;
}

/* ------------------------------------------------ YOURS: write these four
 *
 * proc.h states what each function does, and each of its errors. Use the
 * fields of struct sched above: sched_next reads them. MAN PAGES, at the
 * top of this file, names the manual page of each call.
 *
 * Each function below is a stub: it sets errno to ENOSYS and fails, so
 * that the library builds before you write it. Replace each body. The
 * comment above each function gives its steps.
 */

/* sched_create: make an empty scheduler.
 *
 *   1. If policy is not POLICY_FCFS, POLICY_PRIORITY or POLICY_RR, set
 *      errno to EINVAL and return NULL.
 *   2. If capacity is 0, use 4096.
 *   3. s = malloc(sizeof *s). If it gives NULL, return NULL: malloc has
 *      set errno to ENOMEM.
 *   4. s->slots = calloc(capacity, sizeof *s->slots). calloc gives NULL
 *      with ENOMEM when capacity times the size does not fit in size_t, so
 *      it makes the ENOMEM check of proc.h for you (man 3 calloc). If it
 *      gives NULL: save errno, free(s), put errno back, and return NULL.
 *   5. Set s->policy, s->capacity, s->count = 0 and s->rr_next = 0.
 *   6. Return s.
 */
sched_t *sched_create(sched_policy_t policy, size_t capacity)
{
    (void)policy;
    (void)capacity;
    errno = ENOSYS;
    return NULL;
}

/* sched_destroy: free the scheduler.
 *
 *   1. If s is NULL, return. That is not an error.
 *   2. free(s->slots), then free(s). Free the array first: after
 *      free(s), s->slots cannot be read.
 */
void sched_destroy(sched_t *s)
{
    (void)s;
}

/* sched_push: add a copy of *p at the end.
 *
 *   1. If s or p is NULL, set errno to EINVAL and return -1.
 *   2. If s->count equals s->capacity, set errno to ENOSPC and return -1.
 *      Do not change s.
 *   3. Copy the report into the first free slot: s->slots[s->count] = *p.
 *      The = of a struct copies each field.
 *   4. Add 1 to s->count, and return 0.
 */
int sched_push(sched_t *s, const pending_t *p)
{
    (void)s;
    (void)p;
    errno = ENOSYS;
    return -1;
}

/* sched_pending: how many reports wait.
 *
 *   1. If s is NULL, return 0.
 *   2. Return s->count.
 */
size_t sched_pending(const sched_t *s)
{
    (void)s;
    return 0;
}
