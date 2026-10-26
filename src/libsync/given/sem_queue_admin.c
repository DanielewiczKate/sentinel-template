/* sem_queue_admin.c — GIVEN. The shutdown and the readers of the queue.
 *
 * Your src/sem_queue.c makes the queue, and pushes and pops under the mutex
 * and the two semaphores. This file stops the queue at the end of a run, and
 * reads its count and the waits of the centre.
 */

#include "sentinel/sync.h"

void sem_queue_shutdown(sem_queue_t *sq)
{
    if (sq == NULL) {
        return;
    }
    atomic_store(&sq->shutdown, 1);

    /* Wake each thread that sleeps in sem_wait, or it sleeps for ever and
       the program cannot end. A post too many does no harm; a post too few
       stops the shutdown. */
    for (int i = 0; i < 256; i++) {
        sem_post(&sq->full);
        sem_post(&sq->empty);
    }
}

size_t sem_queue_count(sem_queue_t *sq)
{
    if (sq == NULL) {
        return 0;
    }
    pthread_mutex_lock(&sq->lock);
    size_t n = queue_count(&sq->q);
    pthread_mutex_unlock(&sq->lock);
    return n;
}

long sem_queue_waits(const sem_queue_t *sq)
{
    return sq ? atomic_load(&sq->waits) : 0;
}

int64_t sem_queue_wait_ns(const sem_queue_t *sq)
{
    return sq ? atomic_load(&sq->wait_ns) : 0;
}
