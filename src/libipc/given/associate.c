/* associate.c — GIVEN. Which track does a report belong to?
 *
 * ipc.h says why this file exists. With no association, the track picture
 * uses the id that the SENSOR gave, so one aircraft that three sensors see is
 * three tracks. The course documentation asks for "an accurate picture", and
 * three symbols for one aircraft are not accurate.
 *
 * WHAT THIS FILE IS NOT. It is not a tracker. It has no filter, it does not
 * predict the next position, and it chooses between two possible matches by
 * distance only. A tracker is a subject of mathematics. The subject of this
 * course is that association must read shared state before it writes to it,
 * and that is the difficulty of Lab 4.
 *
 * THE COST IS ON PURPOSE. track_associate walks each bucket, so its time
 * grows with the number of tracks; a match by id takes a fixed time. Under
 * the one mutex of Lab 4, the walk happens with the lock held, and the
 * workers wait for each other. Lab 4 measures that cost. A fast version here
 * would remove the lesson.
 */

#include "sentinel/ipc.h"

#include <math.h>
#include <string.h>
#include <errno.h>

/* 25 km, 30 s, 700 knots.
 *
 * 25 km lets two sensors of different kinds disagree about the position of
 * one contact, and keeps two separate aircraft apart. 30 s is longer than
 * the gap between two reports of each sensor in Table 1, so a live track
 * stays a candidate. 700 knots is above the fastest contact of the feed
 * (600), and it does not let a match jump across the map. */
const assoc_gate_t ASSOC_DEFAULT = {
    .max_km       = 25.0,
    .max_age_ns   = 30LL * 1000 * 1000 * 1000,
    .max_speed_kn = 700.0
};

/* max_km is how much two sensors can disagree about the position of ONE
   contact at the same time. It is not how far a contact can move.
   max_speed_kn adds to it as time passes. The two are one rule: see
   track_associate. */

#define EARTH_RADIUS_KM 6371.0088
#define KM_PER_NM       1.852

static double to_rad(double deg)
{
    return deg * (M_PI / 180.0);
}

double assoc_distance_km(double lat1, double lon1, double lat2, double lon2)
{
    /* The haversine formula. A difference in degrees is not correct here: a
       degree of longitude is 111 km at the equator and 38 km at 70 N, where
       three of the four sensors look. With degrees, the gate is three times
       too wide in the Arctic. */
    double dlat = to_rad(lat2 - lat1);
    double dlon = to_rad(lon2 - lon1);
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(to_rad(lat1)) * cos(to_rad(lat2)) *
               sin(dlon / 2) * sin(dlon / 2);
    if (a > 1.0) {
        a = 1.0;                        /* an error of rounding */
    }
    return 2.0 * EARTH_RADIUS_KM * asin(sqrt(a));
}

const track_entry_t *track_associate(const track_t *t, const contact_t *c,
                                     const assoc_gate_t *gate)
{
    if (t == NULL || c == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (gate == NULL) {
        gate = &ASSOC_DEFAULT;
    }

    int64_t now = sc_now_ns();

    const track_entry_t *best    = NULL;
    double               best_km = 0.0;

    /* THE WALK. Each bucket, each chain. Its time grows with the number of
       tracks, and so the lock of Lab 4 is held much longer. */
    for (size_t i = 0; i < TRACK_BUCKETS; i++) {
        for (const track_entry_t *e = t->buckets[i]; e != NULL; e = e->next) {

            /* Gate 1: the age. A track that no sensor reported for a long
               time is not a candidate, also when the report is near it. */
            int64_t age_ns = now - e->last_seen_ns;
            if (age_ns < 0) {
                age_ns = 0;
            }
            if (age_ns > gate->max_age_ns) {
                continue;
            }

            /* Gate 2: can this report BE that track?
             *
             * Do not divide the distance by the time and compare the result
             * with a maximum speed. Two sensors report one aircraft some
             * microseconds apart, and they disagree about its position by
             * some kilometres, because they are different instruments. The
             * division then gives billions of knots, and the gate refuses
             * each match between two sensors. That is the case that this
             * file must solve.
             *
             * The distance between two reports of one contact has two causes,
             * and only one of them is motion:
             *
             *     the permitted distance  =  the error of the POSITION
             *                             +  how far it can MOVE in the time
             *
             * So the permitted distance starts at max_km, and grows with the
             * time. In 30 s at 700 knots it grows by about 11 km. There is no
             * division, so no special case is necessary when no time passed. */
            double km     = assoc_distance_km(e->lat, e->lon, c->lat, c->lon);
            double age_h  = (double)age_ns / 1e9 / 3600.0;
            double could_move_km = gate->max_speed_kn * KM_PER_NM * age_h;

            if (km > gate->max_km + could_move_km) {
                continue;
            }

            /* The nearest track wins. When two tracks pass the gates, this is
               the whole decision. A real system uses the predicted position
               of each track here. */
            if (best == NULL || km < best_km) {
                best    = e;
                best_km = km;
            }
        }
    }

    return best;
}

int track_update_associated(track_t *t, const contact_t *c,
                            const assoc_gate_t *gate,
                            char assoc_id[CONTACT_ID_MAX + 1])
{
    if (t == NULL || c == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* A report whose own id is in the picture belongs to that track, for
       each position. When one sensor reports the same contact two times, and
       the contact moved more than the gate permits, it is a fast contact and
       not a new one. */
    const track_entry_t *match = track_find(t, c->id);
    if (match == NULL) {
        match = track_associate(t, c, gate);
    }

    contact_t filed = *c;
    if (match != NULL) {
        /* Use the identity of the track: the id from the sensor that saw the
           contact first. The track keeps one name for its whole life. */
        memcpy(filed.id, match->id, CONTACT_ID_MAX + 1);
    }

    if (assoc_id != NULL) {
        memcpy(assoc_id, filed.id, CONTACT_ID_MAX + 1);
    }

    return track_update(t, &filed);
}
