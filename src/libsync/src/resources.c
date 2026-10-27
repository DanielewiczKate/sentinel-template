/* resources.c — two shared resources, and the order that stops a freeze (R5).
 *
 * Besides the track picture, workers compete for two resources: the
 * tracking sensor and the data link to headquarters. A worker that needs
 * BOTH is where a deadlock comes from.
 *
 * THE FOUR CONDITIONS OF A DEADLOCK (Lecture 4.8). A deadlock needs all
 * four at the same time:
 *
 *   1. Mutual exclusion   one thread at a time holds a resource
 *   2. Hold and wait      a thread holds one and waits for another
 *   3. No preemption      no thread can take a resource from its holder
 *   4. Circular wait      a cycle of threads, each waiting for the next
 *
 * Remove ONE of the four, and no deadlock can occur. This file removes the
 * fourth. Each thread takes the two locks in the SAME fixed order: the lower
 * rank first, whatever order the caller names. A cycle needs one thread that
 * takes A, then B, and another that takes B, then A. With one order, no
 * thread takes B before A, so no cycle forms. The rank is the value of
 * resource_id_t.
 *
 * Conditions 1 to 3 stay: a mutex IS mutual exclusion; a worker needs both
 * resources at the same time; and only the holder can unlock a mutex.
 *
 * YOU WRITE THE TWO FUNCTIONS OF THIS FILE. The comment above each one
 * gives its steps, and sync.h gives the same steps with the errors. Each one
 * is a stub now: replace its whole body, its (void) lines too. Lecture 4.8 (Deadlocks) shows the four conditions and the
 * order of the resources.
 *
 * The names, the start, the end and the naive order are given, in
 * given/resources_setup.c.
 *
 * MAN PAGES. Each call of the C library and of Linux in this file, the
 * command that shows its manual page in a terminal, and the same page on
 * the web. The functions of the course have no man page: read them in
 * sync.h, ipc.h and syscall.h.
 *
 *   pthread_mutex_lock(),    man 3 pthread_mutex_lock
 *   pthread_mutex_unlock()
 *       https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3p.html
 *   errno, error names       man 3 errno
 *       https://man7.org/linux/man-pages/man3/errno.3.html
 */

#include "sentinel/sync.h"

#include <errno.h>

/* resources_acquire_both: take both locks, the lower rank first.
 *
 *   1. If r is NULL, a == b, or a or b is below 0 or not below
 *      RESOURCE_COUNT, set errno to EINVAL and return -1.
 *   2. resource_id_t first  = (a < b) ? a : b;
 *      resource_id_t second = (a < b) ? b : a;
 *      The caller can name them in either order: first is the lower rank.
 *   3. pthread_mutex_lock(&r->locks[first]), then
 *      pthread_mutex_lock(&r->locks[second]). No check: write a comment that
 *      says why (sync.h, CALLS THAT NEED NO CHECK).
 *   4. Add 1 to r->acquisitions[first] and to r->acquisitions[second].
 *   5. Return 0.
 */
int resources_acquire_both(resources_t *r, resource_id_t a, resource_id_t b)
{
    (void)r;
    (void)a;
    (void)b;
    errno = ENOSYS;
    return -1;
}

/* resources_release_both: give both locks back.
 *
 *   1. If r is NULL or a == b, return.
 *   2. first and second, as in step 2 above.
 *   3. pthread_mutex_unlock(&r->locks[second]), then
 *      pthread_mutex_unlock(&r->locks[first]).
 */
void resources_release_both(resources_t *r, resource_id_t a, resource_id_t b)
{
    (void)r;
    (void)a;
    (void)b;
}
