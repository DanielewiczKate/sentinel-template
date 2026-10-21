/* queue_stats.c — GIVEN. The state and the statistics of the queue.
 *
 * Your src/queue.c keeps the fields up to date; these functions only read
 * them. A NULL queue gives 0 and does not stop the program: a display calls
 * them, and a display must never stop the centre.
 */

#include "sentinel/ipc.h"

int queue_is_empty(const queue_t *q)
{
    return (q == NULL) || q->count == 0;
}

int queue_is_full(const queue_t *q)
{
    return (q != NULL) && q->count == q->capacity;
}

long queue_pushed(const queue_t *q)  { return q ? q->pushed : 0; }
long queue_popped(const queue_t *q)  { return q ? q->popped : 0; }
long queue_dropped(const queue_t *q) { return q ? q->rejected : 0; }
size_t queue_peak(const queue_t *q)  { return q ? q->peak : 0; }
