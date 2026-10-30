/* assess.c — the class that the centre gives to a report.
 *
 * GIVEN CODE. You read this file, and you do not change it. Your
 * sched_next uses pending_urgency to find the most urgent report.
 *
 * A sensor marks a report P because of something that it observed. It
 * cannot know if the contact is filed, and it does not know which regions
 * the centre monitors. The centre knows both, so the centre decides HIGH: a
 * contact that is not filed AND is in a monitored region.
 *
 * The flag of the sensor never changes. The record keeps what the sensor
 * sent, and the class is a second fact beside it.
 */

#include "sentinel/proc.h"

#include <stddef.h>

urgency_t centre_assess(const traffic_t *expected, const contact_t *c,
                        const char **region)
{
    if (region != NULL) {
        *region = NULL;
    }
    if (c == NULL) {
        return URGENCY_ROUTINE;
    }
    if (expected != NULL && !traffic_is_filed(expected, c)) {
        const traffic_region_t *r = traffic_in_region(expected, c);
        if (r != NULL) {
            if (region != NULL) {
                *region = r->name;
            }
            return URGENCY_HIGH;
        }
    }
    return (c->flag == PRIORITY_PRIORITY) ? URGENCY_SENSOR : URGENCY_ROUTINE;
}

urgency_t pending_urgency(const pending_t *p)
{
    if (p == NULL) {
        return URGENCY_ROUTINE;
    }
    if (p->urgency == URGENCY_ROUTINE && p->c.flag == PRIORITY_PRIORITY) {
        return URGENCY_SENSOR;
    }
    return p->urgency;
}

char urgency_letter(urgency_t u)
{
    return (u == URGENCY_HIGH) ? 'H' : (u == URGENCY_SENSOR) ? 'P' : 'R';
}

const char *urgency_name(urgency_t u)
{
    switch (u) {
    case URGENCY_ROUTINE: return "routine";
    case URGENCY_SENSOR:  return "sensor P";
    case URGENCY_HIGH:    return "HIGH";
    }
    return "?";
}
