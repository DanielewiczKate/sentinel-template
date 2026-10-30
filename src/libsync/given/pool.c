#include "sentinel/sync.h"
#include "cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>

typedef struct {
    safe_pool_t *pool;
    int          index;
    long         processed;
    long         new_tracks;
    long         updates;
    long         resource_ops;
    long         refused;
    long         refused_for[VALID_ERR_FUTURE + 1];
    long         late;
    double       work_sink;

    pthread_mutex_t    view_lock;
    safe_worker_view_t view;

    work_span_t        spans[WORKER_SPANS];
    int                span_next;
    long               span_total;
} safe_worker_t;

struct safe_pool {
    pthread_t     *tids;
    safe_worker_t *workers;
    int            n_workers;

    sem_queue_t   *queue;
    safe_track_t  *track;
    resources_t   *res;
    int            service_us;
    int            late_ms;
    long           passes_per_us;
    log_t         *log;

    const assoc_gate_t  *gate;
    const valid_rules_t *rules;

    atomic_int stop;

    atomic_int observed;

    atomic_int log_steps;
};

static double waited_ms(const pending_t *item)
{
    return (double)(sc_now_ns() - item->arrived_ns) / 1e6;
}

static void do_work(safe_worker_t *w, const contact_t *c)
{
    safe_pool_t *p = w->pool;
    if (p->service_us <= 0) {
        return;
    }
    w->work_sink = work_do(c, p->service_us, p->passes_per_us);
}

static void *worker_main(void *arg)
{
    safe_worker_t *w = arg;
    safe_pool_t   *p = w->pool;

    pthread_mutex_lock(&w->view_lock);
    w->view.tid = gettid();
    pthread_mutex_unlock(&w->view_lock);

    while (!atomic_load(&p->stop)) {
        pending_t item;

        int got = sem_queue_pop(p->queue, &item);
        if (got != 1) {
            break;
        }

        urgency_t urg = pending_urgency(&item);
        if (p->late_ms > 0 && urg == URGENCY_ROUTINE &&
            sc_now_ns() - item.arrived_ns > (int64_t)p->late_ms * 1000000) {
            w->late++;
            continue;
        }
        contact_t c = item.c;

        int steps = atomic_load(&p->log_steps);
        if (steps && p->log != NULL) {
            log_writef(p->log, LOG_PENDING,
                       "worker %d started %s from %s after %.1f ms in the queue",
                       w->index, c.id, c.sensor, waited_ms(&item));
        }

        int observed = atomic_load(&p->observed);
        if (observed) {
            pthread_mutex_lock(&w->view_lock);
            w->view.busy      = 1;
            memcpy(w->view.last_id, c.id, sizeof w->view.last_id);
            w->view.last_flag = urgency_letter(urg);
            pthread_mutex_unlock(&w->view_lock);
        }

        int64_t began_ns = 0, began_cpu = 0;
        if (observed) {
            began_ns  = sc_now_ns();
            began_cpu = cpu_thread_ns();
        }

        do_work(w, &c);

        if (p->res != NULL && urg >= URGENCY_SENSOR) {
            resources_acquire_both(p->res, RESOURCE_HQ_DATALINK,
                                   RESOURCE_TRACKING_SENSOR);
            w->resource_ops++;
            resources_release_both(p->res, RESOURCE_HQ_DATALINK,
                                   RESOURCE_TRACKING_SENSOR);
        }

        int  created;
        char filed_as[CONTACT_ID_MAX + 1];
        filed_as[0] = '\0';
        if (p->gate != NULL) {
            created = safe_track_submit(p->track, &c, p->gate, p->rules, filed_as);
        } else {
            created = safe_track_update(p->track, &c);
        }

        if (steps && p->log != NULL && created >= 0) {
            if (filed_as[0] != '\0' && strcmp(filed_as, c.id) != 0) {
                log_writef(p->log, LOG_TRACK,
                           "track %s %s from %s at %.4f %.4f, %.0f kn"
                           " (reported as %s)",
                           filed_as, created == 1 ? "created" : "updated",
                           c.sensor, c.lat, c.lon, c.speed, c.id);
            } else {
                log_writef(p->log, LOG_TRACK,
                           "track %s %s from %s at %.4f %.4f, %.0f kn",
                           c.id, created == 1 ? "created" : "updated",
                           c.sensor, c.lat, c.lon, c.speed);
            }
        }

        w->processed++;
        if (created == 1) {
            w->new_tracks++;
        } else if (created == 0) {
            w->updates++;
        } else if (created < -1) {
            int why = -created - 2;
            w->refused++;
            if (why > VALID_OK && why <= VALID_ERR_FUTURE) {
                w->refused_for[why]++;
            }
        }

        if (observed || atomic_load(&p->observed)) {
            int64_t ended_ns = observed ? sc_now_ns() : 0;
            int64_t cpu      = observed ? cpu_thread_ns() - began_cpu : 0;

            pthread_mutex_lock(&w->view_lock);
            w->view.busy      = 0;
            w->view.processed = w->processed;
            if (observed) {
                w->view.cpu_ns += cpu;
                w->view.busy_ns += ended_ns - began_ns;
                w->spans[w->span_next].start_ns = began_ns;
                w->spans[w->span_next].end_ns   = ended_ns;
                w->spans[w->span_next].cpu_ns   = cpu;
                w->spans[w->span_next].flag = urgency_letter(urg);
                w->span_next = (w->span_next + 1) % WORKER_SPANS;
                w->span_total++;
            }
            pthread_mutex_unlock(&w->view_lock);
        }
    }

    return NULL;
}

static safe_pool_t *pool_start(int n_workers, sem_queue_t *q, safe_track_t *t,
                               resources_t *res, int service_us, log_t *log,
                               const assoc_gate_t *gate, const valid_rules_t *rules,
                               int late_ms)
{
    if (n_workers <= 0 || q == NULL || t == NULL) {
        errno = EINVAL;
        return NULL;
    }

    safe_pool_t *p = malloc(sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    memset(p, 0, sizeof *p);

    p->tids    = malloc((size_t)n_workers * sizeof *p->tids);
    p->workers = malloc((size_t)n_workers * sizeof *p->workers);
    if (p->tids == NULL || p->workers == NULL) {
        free(p->tids);
        free(p->workers);
        free(p);
        return NULL;
    }
    memset(p->workers, 0, (size_t)n_workers * sizeof *p->workers);

    p->n_workers  = n_workers;
    p->queue      = q;
    p->track      = t;
    p->res        = res;
    p->service_us = service_us;
    p->late_ms   = late_ms;
    p->log        = log;
    p->gate       = gate;
    p->rules      = rules;

    if (service_us > 0) {
        p->passes_per_us = work_calibrate();
        if (p->passes_per_us < 0) {
            free(p->tids);
            free(p->workers);
            free(p);
            errno = EAGAIN;
            return NULL;
        }
        if (log != NULL) {
            log_writef(log, LOG_SYSTEM,
                       "work calibrated: %ld passes = 1 us of CPU (spread %d%%)",
                       p->passes_per_us, work_calibration_spread_pct());
        }
    }

    for (int i = 0; i < n_workers; i++) {
        p->workers[i].pool       = p;
        p->workers[i].index      = i;
        p->workers[i].view.index = i;
        pthread_mutex_init(&p->workers[i].view_lock, NULL);
    }

    for (int i = 0; i < n_workers; i++) {
        int rc = pthread_create(&p->tids[i], NULL, worker_main,
                                &p->workers[i]);
        if (rc != 0) {
            atomic_store(&p->stop, 1);
            sem_queue_shutdown(q);
            for (int j = 0; j < i; j++) {
                pthread_join(p->tids[j], NULL);
            }
            for (int j = 0; j < n_workers; j++) {
                pthread_mutex_destroy(&p->workers[j].view_lock);
            }
            free(p->tids);
            free(p->workers);
            free(p);
            errno = rc;
            return NULL;
        }
    }

    if (log != NULL) {
        log_writef(log, LOG_SYSTEM, "protected worker pool started: %d workers%s",
                   n_workers, gate != NULL ? ", every report validated and associated" : "");
    }
    return p;
}

safe_pool_t *safe_pool_create(int n_workers, sem_queue_t *q, safe_track_t *t,
                              resources_t *res, int service_us, log_t *log)
{
    return pool_start(n_workers, q, t, res, service_us, log, NULL, NULL,
                      POOL_LATE_DEFAULT_MS);
}

safe_pool_t *safe_pool_create_checked(int n_workers, sem_queue_t *q, safe_track_t *t,
                                      resources_t *res, int service_us, log_t *log,
                                      const assoc_gate_t *gate,
                                      const valid_rules_t *rules,
                                      int late_ms)
{
    if (gate == NULL || rules == NULL) {
        errno = EINVAL;
        return NULL;
    }
    return pool_start(n_workers, q, t, res, service_us, log, gate, rules,
                      late_ms);
}

void safe_pool_set_log_steps(safe_pool_t *p, int on)
{
    if (p != NULL) {
        atomic_store(&p->log_steps, on ? 1 : 0);
    }
}

void safe_pool_stop(safe_pool_t *p)
{
    if (p == NULL) {
        return;
    }

    atomic_store(&p->stop, 1);
    sem_queue_shutdown(p->queue);

    for (int i = 0; i < p->n_workers; i++) {
        pthread_join(p->tids[i], NULL);
    }

    if (p->log != NULL) {
        if (p->gate != NULL) {
            log_writef(p->log, LOG_SYSTEM,
                       "protected worker pool stopped: %ld processed, %ld refused as impossible",
                       safe_pool_total_processed(p), safe_pool_total_refused(p));
        } else {
            log_writef(p->log, LOG_SYSTEM,
                       "protected worker pool stopped: %ld processed",
                       safe_pool_total_processed(p));
        }
    }
}

void safe_pool_destroy(safe_pool_t *p)
{
    if (p == NULL) {
        return;
    }
    for (int i = 0; i < p->n_workers; i++) {
        pthread_mutex_destroy(&p->workers[i].view_lock);
    }
    free(p->tids);
    free(p->workers);
    free(p);
}

long safe_pool_total_processed(const safe_pool_t *p)
{
    if (p == NULL) {
        return 0;
    }
    long n = 0;
    for (int i = 0; i < p->n_workers; i++) {
        n += p->workers[i].processed;
    }
    return n;
}

long safe_pool_total_refused(const safe_pool_t *p)
{
    if (p == NULL) {
        return 0;
    }
    long n = 0;
    for (int i = 0; i < p->n_workers; i++) {
        n += p->workers[i].refused;
    }
    return n;
}

long safe_pool_total_late(const safe_pool_t *p)
{
    if (p == NULL) {
        return 0;
    }
    long n = 0;
    for (int i = 0; i < p->n_workers; i++) {
        n += p->workers[i].late;
    }
    return n;
}

long safe_pool_refused_for(const safe_pool_t *p, valid_err_t why)
{
    if (p == NULL || (unsigned)why > VALID_ERR_FUTURE) {
        return 0;
    }
    long n = 0;
    for (int i = 0; i < p->n_workers; i++) {
        n += p->workers[i].refused_for[why];
    }
    return n;
}

int safe_pool_snapshot(safe_pool_t *p, safe_worker_view_t *out, int max)
{
    if (p == NULL || (out == NULL && max > 0) || max < 0) {
        errno = EINVAL;
        return -1;
    }
    atomic_store(&p->observed, 1);

    int n = (max < p->n_workers) ? max : p->n_workers;
    for (int i = 0; i < n; i++) {
        safe_worker_t *w = &p->workers[i];
        pthread_mutex_lock(&w->view_lock);
        out[i] = w->view;
        pthread_mutex_unlock(&w->view_lock);
    }
    return p->n_workers;
}

int safe_pool_worker_spans(safe_pool_t *p, int i, work_span_t *out, int max)
{
    if (p == NULL || out == NULL || max < 0 || i < 0 || i >= p->n_workers) {
        errno = EINVAL;
        return -1;
    }
    atomic_store(&p->observed, 1);

    safe_worker_t *w = &p->workers[i];
    pthread_mutex_lock(&w->view_lock);

    long have = w->span_total < WORKER_SPANS ? w->span_total : WORKER_SPANS;
    if (have > max) {
        have = max;
    }
    int start = (w->span_next - (int)have + WORKER_SPANS * 2) % WORKER_SPANS;
    for (long k = 0; k < have; k++) {
        out[k] = w->spans[(start + (int)k) % WORKER_SPANS];
    }

    pthread_mutex_unlock(&w->view_lock);
    return (int)have;
}
