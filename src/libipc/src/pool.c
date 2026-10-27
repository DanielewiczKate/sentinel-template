/* pool.c — start a pool of worker threads, and stop it.
 *
 * THE COURSE WROTE A PART OF THIS FILE: free_pool() and start_failed(),
 * under "GIVEN". Do not change them. YOU write the three functions under
 * "YOURS". The comment above each one gives its steps, and ipc.h gives the
 * same steps with the errors. Each one is a stub now: it fails, so that the
 * library builds before you write it. Replace each body. Lecture 4.5
 * (Threads) shows pthread_create and pthread_join. MAN PAGES, at the end
 * of this comment, names the manual page of each call.
 *
 * N threads take reports from the same queue and apply them to the same
 * track picture. Each thread runs pool_worker_main, which is given in
 * given/pool_watch.c. This file starts the threads, and stops them.
 *
 * THIS LIBRARY HAS NO LOCK, AND THAT IS THE LESSON. Each worker calls
 * queue_pop and track_update on structures that each other worker uses at
 * the same time. Lab 3 measures what that costs, and Lab 4 corrects it.
 *
 * One thing is correct here, because it is not the lesson: each worker gets
 * its OWN argument block, &workers[i]. With the address of one shared
 * variable, each thread reads the value that the variable has when that
 * thread runs.
 *
 * MAN PAGES. Each call of the C library and of Linux in this file, the
 * command that shows its manual page in a terminal, and the same page on
 * the web. The functions of the course have no man page: read them in
 * ipc.h, proc.h and syscall.h.
 *
 *   malloc(), calloc(),      man 3 malloc
 *   free()
 *       https://man7.org/linux/man-pages/man3/malloc.3.html
 *   pthread_create()         man 3 pthread_create
 *       https://man7.org/linux/man-pages/man3/pthread_create.3.html
 *   pthread_join()           man 3 pthread_join
 *       https://man7.org/linux/man-pages/man3/pthread_join.3.html
 *   threads                  man 7 pthreads
 *       https://man7.org/linux/man-pages/man7/pthreads.7.html
 *   errno, error names       man 3 errno
 *       https://man7.org/linux/man-pages/man3/errno.3.html
 */

#include "sentinel/ipc.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

/* ------------------------------------------------ GIVEN: do not change */

/* Free the thread ids, the argument blocks and the pool. Each pointer can
   be NULL: free(NULL) does nothing. */
static void free_pool(pool_t *p)
{
    free(p->tids);
    free(p->workers);
    free(p);
}

/* pthread_create failed for thread i, and gave rc. Threads 0 to i - 1 run
   already, and they use the pool. So stop them, wait for each one, and only
   then free the pool. Each thread drains the queue before it returns: the
   reports that wait are applied, not lost. Returns NULL with errno rc. */
static pool_t *start_failed(pool_t *p, int i, int rc)
{
    p->stop = 1;
    for (int j = 0; j < i; j++) {
        /* No check: a thread of this pool that no other code joins. */
        pthread_join(p->tids[j], NULL);
    }
    free_pool(p);
    errno = rc;
    return NULL;
}

/* ------------------------------------------------ YOURS */

/* pool_create_deadline: start a pool of n_workers threads.
 *
 *   1. If n_workers <= 0, or q or t is NULL, set errno to EINVAL and
 *      return NULL.
 *   2. pool_t *p = calloc(1, sizeof *p). If it gives NULL, return NULL:
 *      calloc has set errno to ENOMEM.
 *   3. p->tids = calloc(n_workers, sizeof *p->tids), and
 *      p->workers = calloc(n_workers, sizeof *p->workers). If one of them
 *      gives NULL: free_pool(p), set errno to ENOMEM, and return NULL.
 *   4. Set p->n_workers, p->queue (q), p->track (t), p->service_us,
 *      p->late_ms and p->log.
 *   5. If pool_calibrate(p) does not give 0: free_pool(p), set errno to
 *      EAGAIN, and return NULL.
 *   6. For each i from 0 to n_workers - 1:
 *        - p->workers[i].pool = p, and p->workers[i].index = i;
 *        - int rc = pthread_create(&p->tids[i], NULL, pool_worker_main,
 *                                  &p->workers[i]);
 *        - if rc is not 0, return start_failed(p, i, rc).
 *      Give each thread the address of ITS OWN block, &p->workers[i], as
 *      threadNumber.c of Lecture 4.5 gives &thread_numbers[i]. Not &i.
 *   7. If log is not NULL, write the line of ipc.h:
 *        log_writef(log, LOG_SYSTEM,
 *                   "worker pool started: %d workers, %d us, deadline %d ms",
 *                   n_workers, service_us, late_ms);
 *      Do not check its result. Write a comment that says why: a lost log
 *      line is not an error of the pool (the rule of Lab 1).
 *   8. Return p.
 */
pool_t *pool_create_deadline(int n_workers, queue_t *q, track_t *t,
                             int service_us, int late_ms, log_t *log)
{
    (void)n_workers;
    (void)q;
    (void)t;
    (void)service_us;
    (void)late_ms;
    (void)log;
    (void)start_failed;     /* step 6 calls it */
    errno = ENOSYS;
    return NULL;
}

/* pool_stop: tell each worker to stop, and wait for each one.
 *
 *   1. If p is NULL, return.
 *   2. p->stop = 1. Each worker sees it, drains the queue, and returns.
 *   3. For each i from 0 to p->n_workers - 1:
 *        pthread_join(p->tids[i], NULL);
 *      Do not check its result. Write a comment that says why: each one is
 *      a thread of this pool that no other code joins.
 *   4. If p->log is not NULL, write the line of ipc.h:
 *        log_writef(p->log, LOG_SYSTEM, "worker pool stopped: %ld processed",
 *                   pool_total_processed(p));
 *      No check, with the comment of step 7 of pool_create_deadline.
 */
void pool_stop(pool_t *p)
{
    (void)p;
}

/* pool_destroy: free the pool. Call it after pool_stop.
 *
 *   1. If p is not NULL, call free_pool(p).
 */
void pool_destroy(pool_t *p)
{
    (void)p;
}
