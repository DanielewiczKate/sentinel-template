/* safetrack.c — the track picture with a mutex (R4).
 *
 * In Lab 3, track_update does three things that must happen together:
 *
 *     find the entry              (read)
 *     change its fields           (change)
 *     add to the shared totals    (write)
 *
 * Two workers in these steps at the same time lose updates, and the picture
 * counts tracks that do not exist. One mutex around the whole call removes
 * that: the lock covers the read, the change and the write, and no more.
 *
 * WHY ONE LOCK, AND NOT ONE FOR EACH BUCKET. The shared totals cover each
 * bucket, so a lock for each bucket needs one more lock for the totals: a
 * second lock, taken while the first is held. Two locks need a fixed order,
 * or two workers can deadlock (resources.c). One lock is correct and simple.
 *
 * YOU WRITE THIS FILE. The comment above each function gives its steps, and
 * sync.h gives the same steps with the errors. Each function is a stub now:
 * it fails, so that the library builds before you write it. Replace the
 * whole body of each stub, its (void) lines too. Lecture 4.6 (Mutex) shows pthread_mutex_init, pthread_mutex_lock,
 * pthread_mutex_unlock and pthread_mutex_destroy.
 *
 * The rest of the picture (the checked path, the totals, the JSON file) is
 * given, in given/safetrack_check.c.
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
 *   errno, error names       man 3 errno
 *       https://man7.org/linux/man-pages/man3/errno.3.html
 */

#include "sentinel/sync.h"

#include <string.h>
#include <errno.h>

/* safe_track_init: prepare an empty, protected picture.
 *
 *   1. If st is NULL, set errno to EINVAL and return -1.
 *   2. memset(st, 0, sizeof *st).
 *   3. If track_init(&st->track) does not give 0, return -1: it has set
 *      errno.
 *   4. int rc = pthread_mutex_init(&st->lock, NULL). If rc is not 0:
 *      track_destroy(&st->track), set errno to rc, and return -1. A pthread
 *      function RETURNS its error.
 *   5. Return 0.
 */
int safe_track_init(safe_track_t *st)
{
    (void)st;
    errno = ENOSYS;
    return -1;
}

/* safe_track_destroy: free the picture and the mutex.
 *
 *   1. If st is NULL, return.
 *   2. track_destroy(&st->track).
 *   3. pthread_mutex_destroy(&st->lock). No check: sync.h, CALLS THAT NEED
 *      NO CHECK, gives the reason.
 */
void safe_track_destroy(safe_track_t *st)
{
    (void)st;
}

/* safe_track_update: put one report into the picture, under the lock.
 *
 *   1. If st or c is NULL, set errno to EINVAL and return -1.
 *   2. pthread_mutex_lock(&st->lock).
 *   3. int rc = track_update(&st->track, c);
 *   4. pthread_mutex_unlock(&st->lock).
 *   5. Return rc.
 *   The lock covers the whole call of track_update, and nothing more.
 *   No check of the lock and the unlock: sync.h, CALLS THAT NEED NO CHECK,
 *   gives the reason.
 */
int safe_track_update(safe_track_t *st, const contact_t *c)
{
    (void)st;
    (void)c;
    errno = ENOSYS;
    return -1;
}
