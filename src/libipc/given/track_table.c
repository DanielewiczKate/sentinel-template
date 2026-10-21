/* track_table.c — GIVEN. The table of the track picture.
 *
 * The track picture holds one record for each contact identity. A hash
 * table finds the record: the identity is a short string, track_bucket()
 * gives its bucket, and a short chain starts there.
 *
 * Your src/track.c holds track_update, the read, change and write that each
 * worker does. This file holds the rest: the start and the end of the table,
 * the search, the totals, the independent count, and the eviction that
 * keeps the table inside TRACK_MAX_ENTRIES.
 *
 * Nothing here locks. track_find and track_count_entries walk the chains,
 * and a worker that adds an entry at the same time can leave a pointer
 * half-written. Call them when no worker runs, or in Lab 4 with the lock.
 */

#include "sentinel/ipc.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* FNV-1a: small and fast, and it spreads short strings well. */
static unsigned long hash_id(const char *s)
{
    unsigned long h = 2166136261UL;
    for (; *s != '\0'; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619UL;
    }
    return h;
}

size_t track_bucket(const char *id)
{
    return (size_t)(hash_id(id) % TRACK_BUCKETS);
}

int track_init(track_t *t)
{
    if (t == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(t, 0, sizeof *t);
    return 0;
}

void track_destroy(track_t *t)
{
    if (t == NULL) {
        return;
    }
    for (size_t i = 0; i < TRACK_BUCKETS; i++) {
        track_entry_t *e = t->buckets[i];
        while (e != NULL) {
            track_entry_t *next = e->next;
            free(e);
            e = next;
        }
        t->buckets[i] = NULL;
    }
    t->total_reports   = 0;
    t->total_tracks    = 0;
    t->priority_tracks = 0;
    t->total_evicted   = 0;
}

/* Remove one track, so that a new one can be created.
 *
 * WHICH TRACK TO FORGET IS A DECISION OF ENGINEERING. The rule here is the
 * track seen least recently: a contact that no sensor reported for the
 * longest time has most probably left the area. The rule has a cost. A real
 * aircraft that is silent for a minute, the case of R1, can be removed,
 * while a flood of forged contacts arrives all the time and so seems new.
 * **Under an attack, eviction removes real tracks and keeps forged ones.**
 * That is the cost of the limit, and total_evicted shows it to an operator.
 *
 * IT IS A SAMPLE, NOT AN EXACT ORDER. A walk of the whole table at each new
 * track makes a flood a cheaper attack than the memory that the limit saves:
 * the attacker pays for one malloc, and the centre for 1024 buckets. An
 * exact order needs a second list through each entry, changed at each
 * update, and that is more shared state for Lab 3 to race on and for Lab 4
 * to protect.
 *
 * So it looks at a few buckets, and takes the oldest entry among them. One
 * bucket is not enough: an entry alone in its bucket is always the oldest in
 * that bucket, so a contact that reports all the time could be removed while
 * old ones stay. Eight samples make that improbable at a low cost. It is
 * still a probability, so the tests check the limit, and only report how
 * many tracks stay.
 */
#define TRACK_EVICT_SAMPLES 8

void track_evict_one(track_t *t, size_t start_bucket)
{
    track_entry_t **oldest_link = NULL;
    int64_t         oldest_ns   = 0;
    int             sampled     = 0;

    for (size_t step = 0;
         step < TRACK_BUCKETS && sampled < TRACK_EVICT_SAMPLES;
         step++) {
        size_t          b    = (start_bucket + step) % TRACK_BUCKETS;
        track_entry_t **link = &t->buckets[b];

        if (*link == NULL) {
            continue;                   /* an empty bucket is not a sample */
        }
        sampled++;

        for (track_entry_t *e = *link; e != NULL; e = e->next) {
            if (oldest_link == NULL || e->last_seen_ns < oldest_ns) {
                oldest_link = link;
                oldest_ns   = e->last_seen_ns;
            }
            link = &e->next;
        }
    }

    if (oldest_link != NULL) {
        track_entry_t *victim = *oldest_link;
        *oldest_link = victim->next;

        /* total_tracks counts the tracks held NOW, not each track ever
           created, so it goes down with the entry. R4 compares it with an
           independent walk of the buckets, and the two must agree. */
        t->total_tracks--;
        if (victim->priority) {
            t->priority_tracks--;
        }
        t->total_evicted++;

        free(victim);
    }
}

const track_entry_t *track_find(const track_t *t, const char *id)
{
    if (t == NULL || id == NULL) {
        return NULL;
    }
    size_t b = track_bucket(id);
    for (const track_entry_t *e = t->buckets[b]; e != NULL; e = e->next) {
        if (strcmp(e->id, id) == 0) {
            return e;
        }
    }
    return NULL;
}

long track_total_reports(const track_t *t)
{
    return (t == NULL) ? 0 : t->total_reports;
}

long track_total_tracks(const track_t *t)
{
    return (t == NULL) ? 0 : t->total_tracks;
}

long track_count_entries(const track_t *t)
{
    if (t == NULL) {
        return 0;
    }

    /* Count by a walk, not with the counter. The two are kept apart, so a
       difference between them proves that an update was lost. The demo of
       Lab 3 uses that. */
    long n = 0;
    for (size_t i = 0; i < TRACK_BUCKETS; i++) {
        for (const track_entry_t *e = t->buckets[i]; e != NULL; e = e->next) {
            n++;
        }
    }
    return n;
}

long track_total_evicted(const track_t *t)
{
    return t == NULL ? 0 : t->total_evicted;
}
