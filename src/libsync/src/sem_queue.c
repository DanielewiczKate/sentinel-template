/* sem_queue.c — the shared queue, with a mutex and two semaphores.
 *
 * THE COURSE WROTE A PART OF THIS FILE: wait_for_slot(), is_shut_down() and
 * pop_after_shutdown(), under "GIVEN". Do not change them. YOU write the
 * four functions under "YOURS". The comment above each one gives its steps,
 * and sync.h gives the same steps with the errors. Each one is a stub now:
 * it fails, so that the library builds before you write it. Replace the
 * whole body of each stub, its (void) lines too. Lecture 3.3 (Semaphores) shows sem_init, sem_wait, sem_post and
 * sem_destroy in prodCons.c; here the buffer is your FIFO ring of Lab 3,
 * not the stack of the lecture.
 *
 * In Lab 3 a worker called usleep(200) when the queue was empty: a busy
 * wait. And two workers in queue_pop at the same time took one report two
 * times, and took 1 from count two times. This file removes both faults. It
 * is the bounded buffer of Lecture 3.3 (prodCons.c), around the FIFO ring of
 * Lab 3:
 *
 *     empty   the free slots. A producer waits on it; a consumer posts it.
 *     full    the reports that wait. A consumer waits on it; a producer
 *             posts it.
 *     lock    a mutex around the call of queue_push or queue_pop.
 *
 * A semaphore is not a mutex. It holds a NUMBER, and sem_wait waits while
 * that number is 0, with no use of the processor. The mutex is still
 * necessary: the semaphores decide how many threads continue, and the mutex
 * keeps two of them out of the ring at the same time.
 *
 * THE ORDER IS FIXED: sem_wait first, then the lock. A thread that takes the
 * lock and then waits on a semaphore sleeps with the lock, and no other
 * thread can post the semaphore: a deadlock.
 *
 * The shutdown, the count and the totals of the waits are given, in
 * given/sem_queue_admin.c.
 *
 * MAN PAGES. Each call of the C library and of Linux in this file, the
 * command that shows its manual page in a terminal, and the same page on
 * the web. The functions of the course have no man page: read them in
 * sync.h, ipc.h and syscall.h.
 *
 *   memset()                 man 3 memset
 *       https://man7.org/linux/man-pages/man3/memset.3.html
 *   pthread_mutex_init(),    man 3 pthread_mutex_init
 *   pthread_mutex_destroy()
 *       https://man7.org/linux/man-pages/man3/pthread_mutex_init.3p.html
 *   pthread_mutex_lock(),    man 3 pthread_mutex_lock
 *   pthread_mutex_unlock()
 *       https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3p.html
 *   sem_init()               man 3 sem_init
 *       https://man7.org/linux/man-pages/man3/sem_init.3.html
 *   sem_destroy()            man 3 sem_destroy
 *       https://man7.org/linux/man-pages/man3/sem_destroy.3.html
 *   sem_wait(),              man 3 sem_wait
 *   sem_trywait()
 *       https://man7.org/linux/man-pages/man3/sem_wait.3.html
 *   sem_post()               man 3 sem_post
 *       https://man7.org/linux/man-pages/man3/sem_post.3.html
 *   semaphores               man 7 sem_overview
 *       https://man7.org/linux/man-pages/man7/sem_overview.7.html
 *   errno, error names       man 3 errno
 *       https://man7.org/linux/man-pages/man3/errno.3.html
 *   atomic_load()            no man page
 *       https://en.cppreference.com/w/c/atomic/atomic_load
 *   atomic_fetch_add()       no man page
 *       https://en.cppreference.com/w/c/atomic/atomic_fetch_add
 */

#include "sentinel/sync.h"

#include <string.h>
#include <errno.h>

/* ------------------------------------------------ GIVEN: do not change */

/* Wait for a free slot: sem_wait(&sq->empty), again after EINTR. It also
   counts each push that had to wait, and for how long: in SENTINEL the
   producer is the centre, and each wait is time that it does not read its
   sensors. Returns 0, or -1 after a shutdown (the slot is given back). */
static int wait_for_slot(sem_queue_t *sq)
{
    /* sem_trywait takes a free slot at once, or fails with no wait. Only a
       push that must WAIT is timed. */
    if (sem_trywait(&sq->empty) != 0) {
        int64_t began = sc_now_ns();
        while (sem_wait(&sq->empty) != 0) {
            if (errno != EINTR) {
                return -1;
            }
        }
        atomic_fetch_add(&sq->waits, 1);
        atomic_fetch_add(&sq->wait_ns, sc_now_ns() - began);
    }
    if (atomic_load(&sq->shutdown)) {
        sem_post(&sq->empty);
        return -1;
    }
    return 0;
}

/* 1 after sem_queue_shutdown, and 0 before it. */
static int is_shut_down(sem_queue_t *sq)
{
    return atomic_load(&sq->shutdown);
}

/* A pop after a shutdown: take a report that still waits, or give 0 when
   the queue is empty. It posts full again before it gives 0, so that the
   next sleeping worker wakes up too. Without that, one worker wakes and the
   others sleep for ever. */
static int pop_after_shutdown(sem_queue_t *sq, pending_t *out)
{
    pthread_mutex_lock(&sq->lock);
    int got = queue_pop(&sq->q, out);
    pthread_mutex_unlock(&sq->lock);
    if (got == 1) {
        sem_post(&sq->empty);
        return 1;
    }
    sem_post(&sq->full);
    return 0;
}

/* ------------------------------------------------ YOURS */

/* sem_queue_init: prepare an empty queue of capacity slots.
 *
 *   1. If sq is NULL or capacity is 0, set errno to EINVAL and return -1.
 *   2. memset(sq, 0, sizeof *sq).
 *   3. If queue_init(&sq->q, capacity) does not give 0, return -1. It is
 *      your function of Lab 3, and it has set errno.
 *   4. int rc = pthread_mutex_init(&sq->lock, NULL). If rc is not 0:
 *      queue_destroy(&sq->q), set errno to rc, and return -1.
 *   5. sem_init(&sq->empty, 0, (unsigned)capacity): each slot is free at the
 *      start. If it does not give 0:
 *          int saved = errno;
 *          pthread_mutex_destroy(&sq->lock);
 *          queue_destroy(&sq->q);
 *          errno = saved;
 *          return -1;
 *   6. sem_init(&sq->full, 0, 0): no report waits at the start. If it does
 *      not give 0, do as in step 5, with sem_destroy(&sq->empty) first.
 *   7. Return 0.
 */
int sem_queue_init(sem_queue_t *sq, size_t capacity)
{
    (void)sq;
    (void)capacity;
    errno = ENOSYS;
    return -1;
}

/* sem_queue_destroy: free the queue.
 *
 *   1. If sq is NULL, return.
 *   2. queue_destroy(&sq->q), pthread_mutex_destroy(&sq->lock),
 *      sem_destroy(&sq->empty) and sem_destroy(&sq->full). No check: sync.h,
 *      CALLS THAT NEED NO CHECK, gives the reason.
 */
void sem_queue_destroy(sem_queue_t *sq)
{
    (void)sq;
}

/* sem_queue_push: put a copy of *p in; wait for a free slot when it is full.
 *
 *   1. If sq or p is NULL, set errno to EINVAL and return -1.
 *   2. If wait_for_slot(sq) does not give 0, return -1. It is GIVEN, above:
 *      it calls sem_wait(&sq->empty) for you, and it counts the wait.
 *   3. pthread_mutex_lock(&sq->lock).
 *   4. int rc = queue_push(&sq->q, p);
 *   5. pthread_mutex_unlock(&sq->lock).
 *   6. sem_post(&sq->full): one more report waits.
 *   7. Return rc.
 *   queue_push cannot fail in step 4: step 2 gave this thread a free slot.
 *   No check of the lock, the unlock and sem_post: sync.h, CALLS THAT NEED
 *   NO CHECK, gives the reason.
 */
int sem_queue_push(sem_queue_t *sq, const pending_t *p)
{
    (void)sq;
    (void)p;
    (void)wait_for_slot;     /* step 2 calls it */
    errno = ENOSYS;
    return -1;
}

/* sem_queue_pop: take the report at the head; wait while no report waits.
 *
 *   1. If sq or out is NULL, set errno to EINVAL and return -1.
 *   2. Wait for a report:
 *        while (sem_wait(&sq->full) != 0) {
 *            if (errno != EINTR) {
 *                return -1;
 *            }
 *        }
 *      A signal gives EINTR: that is not an error, so wait again.
 *   3. If is_shut_down(sq), return pop_after_shutdown(sq, out). Both are
 *      GIVEN, above.
 *   4. pthread_mutex_lock(&sq->lock).
 *   5. int got = queue_pop(&sq->q, out);
 *   6. pthread_mutex_unlock(&sq->lock).
 *   7. sem_post(&sq->empty): one more slot is free.
 *   8. Return got.
 *   Keep the order: sem_wait first, then the lock. No check of the lock,
 *   the unlock and sem_post, as in the push.
 */
int sem_queue_pop(sem_queue_t *sq, pending_t *out)
{
    (void)sq;
    (void)out;
    (void)is_shut_down;         /* step 3 calls it */
    (void)pop_after_shutdown;   /* step 3 calls it */
    errno = ENOSYS;
    return -1;
}
