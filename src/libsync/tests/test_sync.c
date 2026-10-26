/* test_sync.c — your test of libsync.
 *
 * The course wrote the includes, make(), hammer(), the first test and
 * main(). Write the body of each other test function: the comment above it
 * gives what to check. Each check is an assert(). make test builds and runs
 * this file.
 */

#include "sentinel/sync.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define UPDATES 100000

/* A report about the contact `id`. */
static contact_t make(const char *id)
{
    contact_t c;
    memset(&c, 0, sizeof c);
    snprintf(c.id, sizeof c.id, "%s", id);
    snprintf(c.sensor, sizeof c.sensor, "SAT");
    return c;
}

/* A thread that puts UPDATES reports about ONE contact into the picture
   that arg points to. It is not static, so that the compiler accepts it
   before your test_two_threads calls it. */
void *hammer(void *arg)
{
    safe_track_t *st = arg;
    contact_t c = make("ONE");
    for (int i = 0; i < UPDATES; i++) {
        safe_track_update(st, &c);
    }
    return NULL;
}

/* EXAMPLE, written by the course. One thread: a new id creates a track
   (1), the same id again updates it (0), and the total is 2 reports. */
static void test_one_thread(void)
{
    safe_track_t st;
    assert(safe_track_init(&st) == 0);

    contact_t c = make("A-1");
    assert(safe_track_update(&st, &c) == 1);
    assert(safe_track_update(&st, &c) == 0);
    assert(safe_track_total_reports(&st) == 2);
    assert(safe_track_total_tracks(&st) == 1);

    safe_track_destroy(&st);
}

/* Two threads. Make a picture with safe_track_init. Start two threads with
   pthread_create(&t[i], NULL, hammer, &st), and join both with
   pthread_join. Then safe_track_total_reports(&st) is EXACTLY 2 * UPDATES,
   and safe_track_total_tracks(&st) is 1. With no lock, this test fails on
   two processors or more: that was Lab 3. */
static void test_two_threads(void)
{
}

/* The queue. Make a sem_queue_t of 4 slots with sem_queue_init. Push three
   reports, with the ids A, B and C (a pending_t holds a contact in its field
   c). Then three pops each give 1, with A, then B, then C. Destroy it. */
static void test_queue_order(void)
{
}

/* NULL. safe_track_init(NULL), safe_track_update(NULL, &c),
   sem_queue_init(NULL, 4) and resources_acquire_both(NULL,
   RESOURCE_TRACKING_SENSOR, RESOURCE_HQ_DATALINK) each give -1 with errno
   EINVAL. */
static void test_null(void)
{
}

int main(void)
{
    test_one_thread();
    test_two_threads();
    test_queue_order();
    test_null();
    printf("test_sync: OK\n");
    return 0;
}
