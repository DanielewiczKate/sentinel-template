/* validate.c — GIVEN. Is the report physically possible?
 *
 * contact_parse decides if a line is WELL FORMED. It does not decide if a
 * report is TRUE. Each forged contact of the attack tool was well formed:
 *
 *     9999 knots                     accepted: the parser checks only that
 *                                    the speed is not below 0
 *     a coastal radar at the Pole    accepted: nothing compares a contact
 *                                    with the area of its sensor
 *     a time from 2001               accepted: no part of the system read
 *                                    the time field
 *     one contact in two places,     accepted, both
 *     1 ms apart
 *
 * All five went into the track picture, and R1 to R6 still printed PASS,
 * because the six requirements do not state a rule of security.
 *
 * WHY LAB 4 CHECKS THIS, AND NOT THE PARSER. Two of the three checks need
 * the HISTORY of the track: where this contact was, and when. The history is
 * shared state. So the check must read the picture in the same critical
 * section as the write, or the answer is old before it is used. A parser
 * sees one line, and cannot do this.
 *
 * The functions here only read. libsync calls them with the lock held.
 */

#include "sentinel/ipc.h"

#include <math.h>
#include <string.h>
#include <errno.h>

/* 700 knots is above the fastest contact of Table 1 (600), and above each
 * object in this scenario. 60 s is much longer than the gap between two
 * reports of each sensor, and much shorter than a useful replay. Both values
 * are wide on purpose: a check that refuses real traffic is worse than a
 * check that misses a forgery, because the watch officer sees the first and
 * not the second. */
const valid_rules_t VALID_DEFAULT = {
    .max_speed_kn  = 700.0,
    .max_age_ns    = 60LL * 1000 * 1000 * 1000,
    .max_future_ns = 5LL * 1000 * 1000 * 1000
};

const char *valid_strerror(valid_err_t e)
{
    switch (e) {
    case VALID_OK:            return "ok";
    case VALID_ERR_SPEED:     return "speed impossible";
    case VALID_ERR_TELEPORT:  return "moved further than possible";
    case VALID_ERR_STALE:     return "report too old (replay)";
    case VALID_ERR_FUTURE:    return "report from the future";
    }
    return "unknown";
}

valid_err_t track_validate(const track_t *t, const contact_t *c,
                           const valid_rules_t *r)
{
    if (c == NULL) {
        errno = EINVAL;
        return VALID_ERR_SPEED;     /* a refusal: the caller gave no report */
    }
    if (r == NULL) {
        r = &VALID_DEFAULT;
    }

    /* 1. The speed that the report states. The check that costs least, and
          the only one that needs no state. */
    if (c->speed > r->max_speed_kn) {
        return VALID_ERR_SPEED;
    }

    /* 2. The age. THIS IS THE ONLY CODE THAT READS THE TIME OF A REPORT.
     *
     * The scheduler and the track picture use sc_now_ns() for the arrival,
     * so no other code reads t_sec or t_usec. With no check here, a report
     * from 2001 is current, and a wrong clock fails no test. This check
     * also shows why Lab 1 uses CLOCK_REALTIME for a time that must be a
     * date, and CLOCK_MONOTONIC for a duration: with a monotonic time, the
     * report holds the seconds since the start of the machine, and each
     * report seems to be a replay from 1970. */
    int64_t now_sec; int32_t now_usec;
    sc_now_wall(&now_sec, &now_usec);

    int64_t report_ns = c->t_sec * 1000000000LL + (int64_t)c->t_usec * 1000LL;
    int64_t now_ns    = now_sec  * 1000000000LL + (int64_t)now_usec * 1000LL;
    int64_t age_ns    = now_ns - report_ns;

    if (age_ns > r->max_age_ns) {
        return VALID_ERR_STALE;
    }
    if (-age_ns > r->max_future_ns) {
        /* After the clock of the centre by more than two machines can
           disagree. A sensor cannot report an event that did not occur. */
        return VALID_ERR_FUTURE;
    }

    /* 3. A jump. It needs the HISTORY, so Lab 4 does it.
     *
     * The report states an identity that the picture can know. When it
     * does, ask if the contact can move from its last position to this one
     * in the time since. Association asks which identity a report has; here
     * the report states it, and the question is if the statement agrees
     * with the last report. */
    if (t != NULL) {
        const track_entry_t *e = track_find(t, c->id);
        if (e != NULL) {
            int64_t since_ns = sc_now_ns() - e->last_seen_ns;
            if (since_ns < 0) {
                since_ns = 0;
            }
            double km    = assoc_distance_km(e->lat, e->lon, c->lat, c->lon);
            double hours = (double)since_ns / 1e9 / 3600.0;

            /* The same rule as the association gate, for the same reason:
               two reports of one contact differ by the error of the sensors
               AND by the motion. A division of the distance by the time
               makes each normal update a jump. ASSOC_DEFAULT.max_km is the
               error of the position that the system uses. */
            double could_move_km = r->max_speed_kn * 1.852 * hours;
            if (km > ASSOC_DEFAULT.max_km + could_move_km) {
                return VALID_ERR_TELEPORT;
            }
        }
    }

    return VALID_OK;
}
