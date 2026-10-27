/* test_policies.c — the three policies of the scheduler. GIVEN.
 *
 * The course wrote this test. Do not change it. It uses your sched_create,
 * sched_push, sched_pending and sched_destroy, and the given sched_next.
 * make test builds and runs it. It is also an example of a test with
 * assert(): read it before you write test_lab2.c.
 *
 * Each policy gets the eight reports of Section 3.6 of the Lab 2 Brief, in
 * the same order. So a difference in the output comes from the policy only.
 */

#include "sentinel/proc.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* One waiting report. The scheduler keeps a copy, so a local variable is
   enough. */
static pending_t make(const char *sensor, const char *id, priority_t flag,
                      urgency_t urgency)
{
    pending_t p;
    memset(&p, 0, sizeof p);
    snprintf(p.c.sensor, sizeof p.c.sensor, "%s", sensor);
    snprintf(p.c.id, sizeof p.c.id, "%s", id);
    p.c.flag  = flag;
    p.urgency = urgency;
    return p;
}

/* Push the eight reports of Section 3.6 into a new scheduler. N1 has the
   P flag of its sensor, and the centre gave S4 the class HIGH. */
static sched_t *fill(sched_policy_t policy)
{
    sched_t *s = sched_create(policy, 16);
    assert(s != NULL);

    pending_t seq[] = {
        make("SAT", "S1", PRIORITY_ROUTINE,  URGENCY_ROUTINE),
        make("SAT", "S2", PRIORITY_ROUTINE,  URGENCY_ROUTINE),
        make("NWS", "N1", PRIORITY_PRIORITY, URGENCY_SENSOR),
        make("SAT", "S3", PRIORITY_ROUTINE,  URGENCY_ROUTINE),
        make("AIS", "A1", PRIORITY_ROUTINE,  URGENCY_ROUTINE),
        make("SAT", "S4", PRIORITY_ROUTINE,  URGENCY_HIGH),
        make("AUR", "U1", PRIORITY_ROUTINE,  URGENCY_ROUTINE),
        make("SAT", "S5", PRIORITY_ROUTINE,  URGENCY_ROUTINE),
    };
    for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        assert(sched_push(s, &seq[i]) == 0);
    }
    assert(sched_pending(s) == 8);
    return s;
}

/* Take the eight reports, and compare each id with the row of the table. */
static void check_order(sched_policy_t policy, const char *want[8])
{
    sched_t *s = fill(policy);
    for (int i = 0; i < 8; i++) {
        pending_t got;
        assert(sched_next(s, &got) == 1);
        if (strcmp(got.c.id, want[i]) != 0) {
            fprintf(stderr, "  %s: report %d is %s, not %s\n",
                    sched_policy_name(policy), i + 1, got.c.id, want[i]);
        }
        assert(strcmp(got.c.id, want[i]) == 0);
    }
    pending_t none;
    assert(sched_next(s, &none) == 0);      /* empty now */
    assert(sched_pending(s) == 0);
    sched_destroy(s);
}

static void test_fcfs(void)
{
    const char *want[8] = { "S1", "S2", "N1", "S3", "A1", "S4", "U1", "S5" };
    check_order(POLICY_FCFS, want);
}

static void test_priority(void)
{
    const char *want[8] = { "S4", "N1", "S1", "S2", "S3", "A1", "U1", "S5" };
    check_order(POLICY_PRIORITY, want);
}

static void test_round_robin(void)
{
    const char *want[8] = { "N1", "U1", "A1", "S1", "S2", "S3", "S4", "S5" };
    check_order(POLICY_RR, want);
}

/* A full scheduler. With a capacity of 2, the third push gives -1 with
   ENOSPC, and the two reports in it do not change. */
static void test_full(void)
{
    sched_t *s = sched_create(POLICY_FCFS, 2);
    assert(s != NULL);

    pending_t a = make("NWS", "A", PRIORITY_ROUTINE, URGENCY_ROUTINE);
    pending_t b = make("NWS", "B", PRIORITY_ROUTINE, URGENCY_ROUTINE);
    pending_t c = make("NWS", "C", PRIORITY_ROUTINE, URGENCY_ROUTINE);
    assert(sched_push(s, &a) == 0);
    assert(sched_push(s, &b) == 0);
    errno = 0;
    assert(sched_push(s, &c) == -1);
    assert(errno == ENOSPC);
    assert(sched_pending(s) == 2);

    pending_t got;
    assert(sched_next(s, &got) == 1 && strcmp(got.c.id, "A") == 0);
    assert(sched_next(s, &got) == 1 && strcmp(got.c.id, "B") == 0);
    sched_destroy(s);
}

int main(void)
{
    test_fcfs();
    test_priority();
    test_round_robin();
    test_full();
    printf("test_policies: OK\n");
    return 0;
}
