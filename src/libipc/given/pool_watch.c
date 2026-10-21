/* pool_watch.c — GIVEN. The instruments of the worker pool, and the rules
 * that are not a part of Lab 3.
 *
 * Your src/pool.c starts the threads, runs the worker loop and stops the
 * pool. This file holds what the course needs to measure the pool, and what
 * the page shows:
 *
 *   pool_worker_start   a worker records its own thread id
 *   pool_report_late    the report deadline (R2: an urgent report is never late)
 *   pool_report_begin   the step log line, the two clocks, and the work
 *   pool_report_end     the second step log line, and the span of the report
 *   pool_calibrate      measure the work, one time, before a thread starts
 *
 * and the readers: the CPU time, the busy time, the thread id and the spans
 * of each worker. No reader and no hook locks anything. Lab 3 is the lab with
 * no protection, and Lab 4 copies the same numbers under a lock.
 */

#include "sentinel/ipc.h"
#include "cpu.h"

#include <errno.h>
#include <unistd.h>

pool_t *pool_create(int n_workers, queue_t *q, track_t *t, int service_us,
                    log_t *log)
{
    return pool_create_deadline(n_workers, q, t, service_us,
                                POOL_LATE_DEFAULT_MS, log);
}

int pool_calibrate(pool_t *p)
{
    if (p == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (p->service_us <= 0) {
        return 0;
    }
    /* The calibration measures the CPU time of this thread. On one processor,
       a worker that runs already interrupts it many times, and the median of
       three tries then pays for that. So the pool calibrates before it starts
       a thread. */
    p->passes_per_us = work_calibrate();
    if (p->passes_per_us < 0) {
        errno = EAGAIN;
        return -1;
    }
    if (p->log != NULL) {
        log_writef(p->log, LOG_SYSTEM,
                   "work calibrated: %ld passes = 1 us of CPU (spread %d%%)",
                   p->passes_per_us, work_calibration_spread_pct());
    }
    return 0;
}

void pool_worker_start(pool_worker_t *w)
{
    /* The id of this thread, for cpu_thread_ctxt. /proc/<pid>/status gives
       the context switches of the MAIN thread, and in a pool the main thread
       does not do the work. The worker records it before its first report,
       because a reader can come at any time. */
    w->tid = gettid();
}

/* THE REPORT DEADLINE.
 *
 * A routine report that waited longer than late_ms in the queue is dropped.
 * The picture shows where each object is NOW, and a position that is applied
 * late puts a symbol where the object is no longer. POOL_LATE_DEFAULT_MS in
 * ipc.h gives the reason for 5000 ms.
 *
 * An urgent report is never late: the sensor's P, and the centre's HIGH. R2
 * stops routine traffic from pushing an urgent contact aside, and a drop for
 * age is the same failure by a different route.
 *
 * arrived_ns is CLOCK_MONOTONIC, set by the centre when it accepted the
 * report. The time in the report line cannot be used: it is CLOCK_REALTIME
 * from another machine, and you must never subtract two realtime readings to
 * measure a duration. */
int pool_report_late(const pool_t *p, const pending_t *item)
{
    if (p == NULL || item == NULL) {
        return 0;
    }
    if (p->late_ms <= 0 || pending_urgency(item) != URGENCY_ROUTINE) {
        return 0;
    }
    int64_t waited_ns = sc_now_ns() - item->arrived_ns;
    return waited_ns > (int64_t)p->late_ms * 1000000;
}

void pool_report_begin(pool_worker_t *w, const pending_t *item, int draining,
                       pool_report_t *r)
{
    pool_t *p = w->pool;

    /* Each flag is read one time for each report. A change that a caller
       makes while the workers run thus starts at the next report, and never
       in the middle of one. */
    r->steps    = p->log_steps;
    r->observed = p->observed;
    r->began_ns = 0;
    r->began_cpu = 0;

    /* "Worker processing started", story vi. The line comes BEFORE the work,
       so the log gives the time at which the worker took the report. */
    if (r->steps && p->log != NULL) {
        double waited_ms = (double)(sc_now_ns() - item->arrived_ns) / 1e6;
        log_writef(p->log, LOG_PENDING,
                   "worker %d started %s from %s after %.1f ms in the queue%s",
                   w->index, item->c.id, item->c.sensor, waited_ms,
                   draining ? " (draining)" : "");
    }

    /* The clocks only when a reader asked for them. cpu_thread_ns costs about
       200 ns, which is 0.1% of a report of 200 us, and a run with no page
       does not pay it. */
    if (r->observed) {
        r->began_ns  = sc_now_ns();
        r->began_cpu = cpu_thread_ns();
    }

    /* The work: real arithmetic on the contact, calibrated to service_us
       microseconds of PROCESSOR time. work.c says why it must be processor
       time. The result goes into the worker, so that the compiler cannot
       remove the loop. */
    if (p->service_us > 0) {
        w->work_sink = work_do(&item->c, p->service_us, p->passes_per_us);
    }
}

void pool_report_end(pool_worker_t *w, const pending_t *item, int created,
                     pool_report_t *r)
{
    pool_t *p = w->pool;
    const contact_t *c = &item->c;

    /* "Track updated", story vi. Only when the picture changed: a failed
       track_update changed nothing, and a line for it would make the log
       disagree with the loss ledger. */
    if (r->steps && p->log != NULL && created >= 0) {
        log_writef(p->log, LOG_TRACK,
                   "track %s %s from %s at %.4f %.4f, %.0f kn",
                   c->id, created == 1 ? "created" : "updated",
                   c->sensor, c->lat, c->lon, c->speed);
    }

    if (r->observed) {
        int64_t cpu   = cpu_thread_ns() - r->began_cpu;
        int64_t ended = sc_now_ns();
        w->cpu_ns  += cpu;
        /* Elapsed time, not processor time: how long the worker HELD the
           report. The difference is the wait for the processor, and on one
           processor most of the time is that wait. */
        w->busy_ns += ended - r->began_ns;
        w->spans[w->span_next].start_ns = r->began_ns;
        w->spans[w->span_next].end_ns   = ended;
        w->spans[w->span_next].cpu_ns   = cpu;
        w->spans[w->span_next].flag     =
            urgency_letter(pending_urgency(item));
        w->span_next = (w->span_next + 1) % WORKER_SPANS;
        w->span_total++;
    }
}

void pool_set_log_steps(pool_t *p, int on)
{
    if (p != NULL) {
        p->log_steps = on ? 1 : 0;
    }
}

int pool_worker_count(const pool_t *p)
{
    return (p == NULL) ? 0 : p->n_workers;
}

const worker_stats_t *pool_worker_stats(const pool_t *p, int i)
{
    if (p == NULL || i < 0 || i >= p->n_workers) {
        return NULL;
    }
    return &p->workers[i].stats;
}

/* The first reader sets the flag that starts the recording. The readers take
   a const pool, so this casts the const away for that one flag. A separate
   pool_observe() is a rule that each caller must remember, and a caller that
   forgets gets empty rows on the page. */
static void observe(const pool_t *p)
{
    if (p != NULL && !p->observed) {
        ((pool_t *)p)->observed = 1;
    }
}

int64_t pool_worker_cpu_ns(const pool_t *p, int i)
{
    if (p == NULL || i < 0 || i >= p->n_workers) {
        return 0;
    }
    observe(p);
    return p->workers[i].cpu_ns;        /* NOT PROTECTED, on purpose */
}

int64_t pool_worker_busy_ns(const pool_t *p, int i)
{
    if (p == NULL || i < 0 || i >= p->n_workers) {
        return 0;
    }
    observe(p);
    return p->workers[i].busy_ns;       /* NOT PROTECTED, on purpose */
}

pid_t pool_worker_tid(const pool_t *p, int i)
{
    if (p == NULL || i < 0 || i >= p->n_workers) {
        return 0;
    }
    observe(p);
    return p->workers[i].tid;
}

int pool_worker_spans(const pool_t *p, int i, work_span_t *out, int max)
{
    if (p == NULL || out == NULL || max < 0 || i < 0 || i >= p->n_workers) {
        errno = EINVAL;
        return -1;
    }
    observe(p);

    const pool_worker_t *w = &p->workers[i];
    long have = w->span_total < WORKER_SPANS ? w->span_total : WORKER_SPANS;
    if (have > max) {
        have = max;
    }

    /* The newest LAST, so that the page draws from left to right. span_next
       is the slot that the worker writes next, so the oldest of the `have`
       spans is `have` slots before it. */
    int start = (w->span_next - (int)have + WORKER_SPANS * 2) % WORKER_SPANS;
    for (long k = 0; k < have; k++) {
        out[k] = w->spans[(start + (int)k) % WORKER_SPANS];
    }
    return (int)have;
}
