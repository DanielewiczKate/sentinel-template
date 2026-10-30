/* track_table.c — GIVEN. The table of the track picture.
 *
 * The track picture holds one record for each contact identity. A hash
 * table finds the record: the identity is a short string, track_bucket()
 * gives its bucket, and a short chain starts there.
 *
 * This file holds track_update, the read, change and write that each worker
 * does, and the rest: the start and the end of the table, the search, the
 * totals, the independent count, and the eviction that keeps the table
 * inside TRACK_MAX_ENTRIES.
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

/* track_update: one report into the track picture.
 *
 * A report about a contact that the picture knows UPDATES its record. It
 * does not add a second record. That makes the table a picture of what is
 * out there, and not a log of reports.
 *
 * WHAT LAB 4 MUST PROTECT. track_update is a read, change and write on
 * shared state:
 *
 *     find the entry            (read)
 *     change its fields         (change)
 *     add to the totals         (write)
 *
 * No step is atomic, and the insert into a chain is worse. Two threads can
 * read the same head of a bucket, link their new entry to it, and store
 * their own entry as the head. One of the two entries is then lost, and
 * nothing reports it. This function has no lock: that is the state that
 * Lab 3 gives to Lab 4.
 */
int track_update(track_t *t, const contact_t *c)
{
    if (t == NULL || c == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t         b = track_bucket(c->id);
    track_entry_t *e = t->buckets[b];
    while (e != NULL && strcmp(e->id, c->id) != 0) {
        e = e->next;
    }

    int64_t now = sc_now_ns();
    int     urgent = (c->flag == PRIORITY_PRIORITY);

    if (e != NULL) {
        /* Update. Read, change, write: on state that another thread can
           change at the same time. */
        e->lat          = c->lat;
        e->lon          = c->lon;
        e->speed        = c->speed;
        e->reports++;
        e->last_seen_ns = now;
        e->last_flag    = urgent ? 'P' : 'R';
        memcpy(e->sensor, c->sensor, CONTACT_SENSOR_LEN + 1);

        if (urgent && !e->priority) {
            e->priority = 1;
            t->priority_tracks++;
        }
        t->total_reports++;
        return 0;
    }

    /* A new contact, so the picture grows. Make room first when it is
       full. */
    if (t->total_tracks >= TRACK_MAX_ENTRIES) {
        track_evict_one(t, b);
    }

    e = calloc(1, sizeof *e);
    if (e == NULL) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(e->id, c->id, CONTACT_ID_MAX + 1);
    memcpy(e->sensor, c->sensor, CONTACT_SENSOR_LEN + 1);
    e->lat           = c->lat;
    e->lon           = c->lon;
    e->speed         = c->speed;
    e->reports       = 1;
    e->priority      = urgent;
    e->last_flag     = urgent ? 'P' : 'R';
    e->first_seen_ns = now;
    e->last_seen_ns  = now;

    /* Link it at the head of the bucket. Two threads that do this at the
       same time on one bucket lose one of the two entries: both read the
       same old head, both point at it, and the second store replaces the
       first. */
    e->next       = t->buckets[b];
    t->buckets[b] = e;

    t->total_tracks++;
    t->total_reports++;
    if (urgent) {
        t->priority_tracks++;
    }
    return 1;
}
