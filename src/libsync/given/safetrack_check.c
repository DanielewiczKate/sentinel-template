/* safetrack_check.c — GIVEN. The rest of the protected track picture.
 *
 * Your src/safetrack.c makes the picture and puts one report into it under
 * the mutex. This file holds what the final program needs too: the checked
 * path (validate, associate, update), the totals, and the JSON file for the
 * page. Each function takes the same mutex, st->lock, for each read and each
 * write of the picture.
 */

#include "sentinel/sync.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

/* A read of a shared long while another thread writes it is also a race. So
   each total takes the lock, and gives a value that existed. */
long safe_track_total_reports(safe_track_t *st)
{
    if (st == NULL) {
        return 0;
    }
    pthread_mutex_lock(&st->lock);
    long v = track_total_reports(&st->track);
    pthread_mutex_unlock(&st->lock);
    return v;
}

long safe_track_total_tracks(safe_track_t *st)
{
    if (st == NULL) {
        return 0;
    }
    pthread_mutex_lock(&st->lock);
    long v = track_total_tracks(&st->track);
    pthread_mutex_unlock(&st->lock);
    return v;
}

long safe_track_count_entries(safe_track_t *st)
{
    if (st == NULL) {
        return 0;
    }
    pthread_mutex_lock(&st->lock);
    long v = track_count_entries(&st->track);
    pthread_mutex_unlock(&st->lock);
    return v;
}

int safe_track_submit(safe_track_t *st, const contact_t *c,
                      const assoc_gate_t *gate, const valid_rules_t *rules,
                      char assoc_id[CONTACT_ID_MAX + 1])
{
    if (st == NULL || c == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&st->lock);

    /* ONE CRITICAL SECTION, not three. Validation and association READ the
       picture to decide what the update writes. With three short critical
       sections, a worker decides on a picture that another worker changed
       after the check. The cost: this holds the lock for a scan of each
       track, and safe_track_update holds it for one hash lookup. */
    valid_err_t v = track_validate(&st->track, c, rules);
    if (v != VALID_OK) {
        st->refused++;
        pthread_mutex_unlock(&st->lock);
        return -(2 + (int)v);
    }

    int rc = track_update_associated(&st->track, c, gate, assoc_id);

    pthread_mutex_unlock(&st->lock);
    return rc;
}

long safe_track_refused(safe_track_t *st)
{
    if (st == NULL) {
        return 0;
    }
    pthread_mutex_lock(&st->lock);
    long n = st->refused;
    pthread_mutex_unlock(&st->lock);
    return n;
}

int safe_track_write_json(safe_track_t *st, const char *path)
{
    if (st == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* Write a temporary file, then rename it. The page reads this file while
       the centre writes it, and rename() is atomic: a reader gets the old
       file or the new one, never a part of one. */
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        return -1;
    }

    pthread_mutex_lock(&st->lock);

    /* One time for the whole picture, so that a reader ages each track
       against the same instant. sc_now_ns is the monotonic clock of
       last_seen_ns, so the two subtract correctly. */
    int64_t now = sc_now_ns();

    fprintf(f, "{\n  \"tracks\": [\n");
    long n = 0;
    for (size_t b = 0; b < TRACK_BUCKETS; b++) {
        for (const track_entry_t *e = st->track.buckets[b]; e; e = e->next) {
            char flag[2] = { e->last_flag ? e->last_flag : 'R', '\0' };
            fprintf(f,
                "%s    {\"id\":\"%s\",\"sensor\":\"%s\",\"lat\":%.4f,"
                "\"lon\":%.4f,\"speed\":%.1f,\"reports\":%ld,\"priority\":%d,"
                "\"flag\":\"%s\",\"last_ns\":%lld,\"first_ns\":%lld}",
                n ? ",\n" : "", e->id, e->sensor, e->lat, e->lon, e->speed,
                e->reports, e->priority, flag,
                (long long)e->last_seen_ns, (long long)e->first_seen_ns);
            n++;
        }
    }
    fprintf(f, "\n  ],\n");
    fprintf(f, "  \"now\": %lld,\n", (long long)now);
    fprintf(f, "  \"count\": %ld,\n", n);
    fprintf(f, "  \"reports\": %ld,\n", st->track.total_reports);
    fprintf(f, "  \"evicted\": %ld,\n", st->track.total_evicted);
    fprintf(f, "  \"refused\": %ld\n", st->refused);
    fprintf(f, "}\n");

    pthread_mutex_unlock(&st->lock);

    if (fclose(f) != 0) {
        return -1;
    }
    return rename(tmp, path);
}
