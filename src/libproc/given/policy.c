/* policy.c — the names of the scheduler policies.
 *
 * GIVEN CODE. You read this file, and you do not change it. demo_lab2 uses
 * these two functions to read --sched and to print the policy.
 */

#include "sentinel/proc.h"

#include <string.h>

/* The order is the order of sched_policy_t in proc.h. */
static const char *const kPolicyNames[] = { "fcfs", "priority", "rr" };

const char *sched_policy_name(sched_policy_t p)
{
    if (p < 0 || (size_t)p >= sizeof kPolicyNames / sizeof kPolicyNames[0]) {
        return NULL;
    }
    return kPolicyNames[p];
}

int sched_policy_parse(const char *name, sched_policy_t *out)
{
    if (name == NULL || out == NULL) {
        return -1;
    }
    for (size_t i = 0; i < sizeof kPolicyNames / sizeof kPolicyNames[0]; i++) {
        if (strcmp(name, kPolicyNames[i]) == 0) {
            *out = (sched_policy_t)i;
            return 0;
        }
    }
    return -1;
}
