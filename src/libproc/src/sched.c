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
 * fields of struct sched above: sched_next reads them. Read man 3 malloc
 * and man 3 free.
 *
 * Each function below is a stub: it sets errno to ENOSYS and fails, so
 * that the library builds before you write it. Replace each body.
 */

sched_t *sched_create(sched_policy_t policy, size_t capacity)
{
    (void)policy;
    (void)capacity;
    errno = ENOSYS;
    return NULL;
}

void sched_destroy(sched_t *s)
{
    (void)s;
}

int sched_push(sched_t *s, const pending_t *p)
{
    (void)s;
    (void)p;
    errno = ENOSYS;
    return -1;
}

size_t sched_pending(const sched_t *s)
{
    (void)s;
    return 0;
}
