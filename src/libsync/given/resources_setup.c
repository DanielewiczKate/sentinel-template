/* resources_setup.c — GIVEN. The start, the end and the names of the two
 * resources, and the naive order.
 *
 * Your src/resources.c takes and gives back the two locks in the fixed
 * order. This file makes and destroys the two mutexes. It also holds
 * resources_acquire_naive, which takes the locks in the order that the caller
 * names: two threads that name them in opposite orders can deadlock. The
 * course tests use it to show the freeze that your order removes.
 */

#include "sentinel/sync.h"

#include <string.h>
#include <errno.h>

static const char *const kResourceNames[RESOURCE_COUNT] = {
    "tracking sensor",
    "HQ data link"
};

const char *resource_name(resource_id_t id)
{
    if (id < 0 || id >= RESOURCE_COUNT) {
        return NULL;
    }
    return kResourceNames[id];
}

int resources_init(resources_t *r)
{
    if (r == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(r, 0, sizeof *r);
    for (int i = 0; i < RESOURCE_COUNT; i++) {
        int rc = pthread_mutex_init(&r->locks[i], NULL);
        if (rc != 0) {
            errno = rc;
            return -1;
        }
    }
    return 0;
}

void resources_destroy(resources_t *r)
{
    if (r == NULL) {
        return;
    }
    for (int i = 0; i < RESOURCE_COUNT; i++) {
        pthread_mutex_destroy(&r->locks[i]);
    }
}

int resources_acquire_naive(resources_t *r, resource_id_t a, resource_id_t b)
{
    if (r == NULL || a == b || a < 0 || b < 0 ||
        a >= RESOURCE_COUNT || b >= RESOURCE_COUNT) {
        errno = EINVAL;
        return -1;
    }

    /* NO REORDER. Each call looks correct alone; two of them with opposite
       orders, at the same time, can each hold one lock and wait for ever for
       the other. */
    pthread_mutex_lock(&r->locks[a]);
    pthread_mutex_lock(&r->locks[b]);
    r->acquisitions[a]++;
    r->acquisitions[b]++;
    return 0;
}
