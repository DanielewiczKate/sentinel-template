/* queue.c — a ring buffer of contact reports.
 *
 * YOU WRITE THIS FILE. The comment above each function gives its steps, and
 * ipc.h gives the same steps with the errors. Each function is a stub now:
 * it fails, so that the library builds before you write it. Replace each
 * body. Read man 3 calloc, man 3 free and man 3 memset.
 *
 * head and tail move around a fixed array. After the last slot, the
 * operator % takes them back to slot 0: (tail + 1) % capacity. In a ring,
 * head == tail means "empty" and also "full", so the queue keeps a count.
 *
 * Do not add a lock. This file is correct for one thread and wrong for many,
 * and Lab 3 measures that.
 */

#include "sentinel/ipc.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* queue_init: prepare an empty queue.
 *
 *   1. If q is NULL or capacity is 0, set errno to EINVAL and return -1.
 *   2. Set each field of *q to 0: memset(q, 0, sizeof *q).
 *   3. q->slots = calloc(capacity, sizeof *q->slots). If it gives NULL, set
 *      errno to ENOMEM and return -1.
 *   4. Set q->capacity to capacity, and return 0.
 */
int queue_init(queue_t *q, size_t capacity)
{
    (void)q;
    (void)capacity;
    errno = ENOSYS;
    return -1;
}

/* queue_destroy: free the slots.
 *
 *   1. If q is NULL, return.
 *   2. free(q->slots). Then set q->slots to NULL, and q->capacity and
 *      q->count to 0.
 */
void queue_destroy(queue_t *q)
{
    (void)q;
}

/* queue_push: put a copy of *p at the tail.
 *
 *   1. If q or p is NULL, set errno to EINVAL and return -1.
 *   2. If q->count equals q->capacity, the queue is full: add 1 to
 *      q->rejected, set errno to ENOSPC, and return -1.
 *   3. q->slots[q->tail] = *p.
 *   4. q->tail = (q->tail + 1) % q->capacity.
 *   5. Add 1 to q->count and to q->pushed.
 *   6. If q->count is more than q->peak, set q->peak to q->count.
 *   7. Return 0.
 */
int queue_push(queue_t *q, const pending_t *p)
{
    (void)q;
    (void)p;
    errno = ENOSYS;
    return -1;
}

/* queue_pop: take the report at the head.
 *
 *   1. If q or out is NULL, set errno to EINVAL and return -1.
 *   2. If q->count is 0, the queue is empty: return 0, and do not change
 *      *out.
 *   3. *out = q->slots[q->head].
 *   4. q->head = (q->head + 1) % q->capacity.
 *   5. Take 1 from q->count, and add 1 to q->popped.
 *   6. Return 1.
 */
int queue_pop(queue_t *q, pending_t *out)
{
    (void)q;
    (void)out;
    errno = ENOSYS;
    return -1;
}

/* queue_count: the number of reports that wait.
 *
 *   1. If q is NULL, return 0.
 *   2. Return q->count.
 */
size_t queue_count(const queue_t *q)
{
    (void)q;
    return 0;
}
