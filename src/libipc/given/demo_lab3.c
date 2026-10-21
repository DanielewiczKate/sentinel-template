/* demo_lab3 — the demonstration program of Lab 3.
 *
 * GIVEN CODE. You read this file, and you do not change it.
 *
 * It runs the centre with a pool of worker threads that apply the reports
 * to the track picture, with NO protection:
 *
 *   contactfeed x4 -> a pipe each -> poll -> scheduler -> queue -> workers
 *                                                                    |
 *                                                              track picture
 *
 *   demo_lab3 [--workers N] [--sched fcfs|priority|rr] [--ms N]
 *             [--service-us N] [--log PATH] [--feed PATH]
 *             [--live PATH] [--live-ms N] [--tracks PATH]
 *             [--expected PATH] [--cpus N|all] [--late-ms N]
 *             [--queue N] [--log-detail full|events]
 *
 * The defaults: 8 workers, priority, 3000 ms, 200 us of work for each
 * report, 1 processor for the centre, a deadline of 5000 ms, and a queue of
 * 8192 slots. --feed names the program that each sensor process runs. The
 * demo does not start a stopped sensor again.
 *
 * At the end it prints the totals, a line for each worker, the LOSS LEDGER
 * and one of two verdicts: "shared data agrees" or "shared data
 * DISAGREES". With 1 worker the shared data must agree. With more, it can
 * disagree, and that is the result of Lab 3, not a fault of the demo.
 *
 * Exit status: 0 when the run completes, with each verdict; 1 when a call
 * fails; 2 for a wrong argument.
 *
 * NOTHING HERE IS PROTECTED, AND THE DISPLAY ALSO HAS NO PROTECTION.
 *
 * The main thread pushes into the queue while the workers pop from it. That
 * is the race of Lab 3, at full speed.
 *
 * live.json: the main thread writes it. Its own numbers are exact: the
 * reports read, refused and queued. It reads the numbers of the workers and
 * the depth of the queue while the workers write them, with no lock, so they
 * can be wrong, and the file says "protected": false. Lab 4 shows the same
 * panel with exact numbers.
 *
 * It does NOT read the track picture while the workers run. A worker that
 * adds to a chain leaves a pointer half-written for a moment, and a walk that
 * follows it can crash. Wrong numbers teach Lab 3; a crash teaches nothing.
 * So it writes tracks.json one time, after pool_stop.
 *
 * WHY THE STOP HAS A TIME LIMIT. The queue keeps a count. Two workers that
 * take one report at the same time can subtract two times. The count is a
 * size_t, so below 0 it becomes very large, and the queue then always says
 * that reports wait. pool_stop drains a queue that never becomes empty, and
 * never returns. That occurred on the first run of 8 workers here.
 *
 * That is R4 at its worst: not a wrong number, but a centre that never ends.
 * So another thread joins the workers, with a few seconds for it. If they do
 * not stop, the demo says so, and leaves with status 0. The semaphores of
 * Lab 4 remove the cause.
 */

#include "sentinel/ipc.h"
#include "sentinel/proc.h"
#include "traffic.h"
#include "linereader.h"
#include "cpu.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef SENTINEL_BUILD_DIR
#define SENTINEL_BUILD_DIR "./build"
#endif

#define FEED SENTINEL_BUILD_DIR "/contactfeed"

static volatile sig_atomic_t g_interrupted = 0;

static void on_interrupt(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

typedef struct {
    int64_t                elapsed_ms;
    int                    running;
    int                    stalled;      /* the workers would not stop */
    sched_policy_t         policy;
    long                   read, rejected, priority, high, pushed, dropped, lost_sched;
    const long            *per_sensor;
    const sensor_net_t    *net;
    const queue_t         *queue;
    const track_t         *track;
    const pool_t          *pool;
    const traffic_watch_t *watch;
    int                    cpus_allowed;  /* what the centre may really use */
    int                    cpus_online;   /* what the machine has */
    int                    log_full;      /* every step, or events only */
} live_t;

static int write_live(const char *path, const live_t *lv)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        return -1;
    }

    int64_t now = sc_now_ns();
    int     n   = pool_worker_count(lv->pool);

    fprintf(f, "{\n  \"part\": 3,\n  \"protected\": false,\n");
    fprintf(f, "  \"written\": %lld,\n  \"running\": %s,\n",
            (long long)now, lv->running ? "true" : "false");
    fprintf(f, "  \"elapsed_ms\": %lld,\n  \"policy\": \"%s\",\n",
            (long long)lv->elapsed_ms, sched_policy_name(lv->policy));
    /* The page states the number of processors, because each other
       number depends on it: the permitted ones, not the online ones. */
    fprintf(f, "  \"cpus\": %d,\n  \"cpus_online\": %d,\n",
            lv->cpus_allowed, lv->cpus_online);
    /* Which of the five steps of story vi this run records. */
    fprintf(f, "  \"log_detail\": \"%s\",\n", lv->log_full ? "full" : "events");
    /* NOT PROTECTED: the workers change count during this read. */
    fprintf(f, "  \"queue\": %zu,\n  \"stalled\": %s,\n",
            queue_count(lv->queue), lv->stalled ? "true" : "false");
    fprintf(f, "  \"read\": %ld,\n  \"rejected\": %ld,\n  \"priority\": %ld,\n"
               "  \"high\": %ld,\n",
            lv->read, lv->rejected, lv->priority, lv->high);
    fprintf(f, "  \"pushed\": %ld,\n  \"dropped\": %ld,\n", lv->pushed, lv->dropped);
    /* NOT PROTECTED, as each number of a worker here. THE LEDGER, for
       the page. Each number in it, except read, rejected and dropped, is
       read while the workers change it. So the ledger can fail to
       balance for two reasons: a report was lost, or its count is wrong.
       In Lab 3 the two cannot be told apart. */
    long lv_late    = pool_total_late(lv->pool);
    long lv_counted = track_total_reports(lv->track);
    long lv_queued  = (long)queue_count(lv->queue);
    long lv_acc     = lv->rejected + lv->lost_sched + lv->dropped + lv_late +
                      lv_counted + lv_queued;
    fprintf(f, "  \"late\": %ld,\n  \"lost_sched\": %ld,\n", lv_late, lv->lost_sched);
    fprintf(f, "  \"ledger\": {\"read\":%ld,\"rejected\":%ld,\"lost_sched\":%ld,"
               "\"dropped_full\":%ld,\"late\":%ld,\"refused\":0,"
               "\"queued_now\":%ld,\"counted\":%ld,\"accounted\":%ld,"
               "\"balanced\":%s},\n",
            lv->read, lv->rejected, lv->lost_sched, lv->dropped, lv_late,
            lv_queued, lv_counted, lv_acc,
            lv_acc == lv->read ? "true" : "false");
    fprintf(f, "  \"queue_cap\": %zu,\n  \"queue_peak\": %zu,\n",
            lv->queue->capacity, queue_peak(lv->queue));
    fprintf(f, "  \"waits\": 0,\n  \"wait_ms\": 0.0,\n");

    long processed = 0;
    int64_t pool_cpu_ns = 0, pool_busy_ns = 0;
    fprintf(f, "  \"workers\": [");
    for (int i = 0; i < n; i++) {
        /* NOT PROTECTED, on purpose: see the top of this file. */
        const worker_stats_t *ws = pool_worker_stats(lv->pool, i);
        long done = ws ? ws->processed : 0;
        processed += done;
        fprintf(f, "%s{\"index\":%d,\"processed\":%ld", i ? "," : "", i, done);

        /* The numbers that Lab 4 publishes, read with NO LOCK. A span can
           be half written, and cpu_ns can change during the read. The
           file says "protected": false. */
        int64_t cpu  = pool_worker_cpu_ns(lv->pool, i);
        int64_t held = pool_worker_busy_ns(lv->pool, i);
        pid_t   tid  = pool_worker_tid(lv->pool, i);
        pool_cpu_ns  += cpu;
        pool_busy_ns += held;
        fprintf(f, ",\"cpu_ns\":%lld,\"busy_ns\":%lld,\"tid\":%d",
                (long long)cpu, (long long)held, (int)tid);

        long vol = 0, invol = 0;
        if (tid > 0) {
            cpu_thread_ctxt(tid, &vol, &invol);
        }
        fprintf(f, ",\"vol_ctxt\":%ld,\"invol_ctxt\":%ld", vol, invol);

        work_span_t sp[WORKER_SPANS];
        int nsp = pool_worker_spans(lv->pool, i, sp, WORKER_SPANS);
        fprintf(f, ",\"spans\":[");
        for (int k = 0; k < nsp; k++) {
            char sflag[2] = { sp[k].flag ? sp[k].flag : 'R', '\0' };
            fprintf(f, "%s{\"s\":%lld,\"e\":%lld,\"cpu\":%lld,\"f\":\"%s\"}",
                    k ? "," : "", (long long)sp[k].start_ns,
                    (long long)sp[k].end_ns, (long long)sp[k].cpu_ns, sflag);
        }
        fprintf(f, "]}");
    }
    fprintf(f, "],\n  \"processed\": %ld,\n", processed);
    fprintf(f, "  \"pool_cpu_ns\": %lld,\n", (long long)pool_cpu_ns);
    fprintf(f, "  \"pool_busy_ns\": %lld,\n", (long long)pool_busy_ns);

    fprintf(f, "  \"sensors\": [");
    for (int s = 0; s < SENSOR_COUNT; s++) {
        const sensor_t *sn = &lv->net->sensors[s];
        fprintf(f, "%s{\"name\":\"%s\",\"reports\":%ld,\"pid\":%d,\"running\":%s,\"restarts\":%ld}",
                s ? "," : "", sensor_name((sensor_id_t)s), lv->per_sensor[s],
                (int)sn->pid, sn->pid > 0 ? "true" : "false", sn->restarts);
    }
    fprintf(f, "],\n  \"expected\": %s,\n", lv->watch ? "true" : "false");
    fprintf(f, "  \"unknown_reports\": %ld,\n  \"unknown\": ",
            lv->watch ? lv->watch->reports : 0L);
    if (lv->watch) {
        traffic_watch_write_json(lv->watch, f, now);
    } else {
        fputs("[]", f);
    }
    fprintf(f, "\n}\n");

    int bad = ferror(f);
    if (fclose(f) != 0 || bad) {
        unlink(tmp);
        return -1;
    }
    return rename(tmp, path);
}

/* pool_stop, in a thread of its own, so that a drain that never ends
   does not stop the demonstration. done is atomic: it is the flag of the
   demo, not a part of the lesson, and ThreadSanitizer must report only
   the shared data of Lab 3. */
typedef struct {
    pool_t    *pool;
    atomic_int done;
} stopper_t;

static void *stopper(void *arg)
{
    stopper_t *st = arg;
    pool_stop(st->pool);
    atomic_store(&st->done, 1);
    return NULL;
}

/* The picture, in the form that Lab 4 writes, so that the page reads
   either one. Call it only after pool_stop. */
static int write_tracks(const track_t *t, const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        return -1;
    }
    fprintf(f, "{\n  \"tracks\": [\n");
    long n = 0;
    for (size_t b = 0; b < TRACK_BUCKETS; b++) {
        for (const track_entry_t *e = t->buckets[b]; e; e = e->next) {
            fprintf(f, "%s    {\"id\":", n ? ",\n" : "");
            traffic_json_string(f, e->id);
            fprintf(f, ",\"sensor\":");
            traffic_json_string(f, e->sensor);
            fprintf(f, ",\"lat\":%.4f,\"lon\":%.4f,\"speed\":%.1f,\"reports\":%ld,\"priority\":%d",
                    e->lat, e->lon, e->speed, e->reports, e->priority);
            /* The same form as Lab 4: the flag of the LAST report, and when
               it arrived, so that a page can age a track. Lab 4 writes these
               with the lock of the picture held, and this lab does not. */
            fprintf(f, ",\"flag\":\"%c\",\"last_ns\":%lld,\"first_ns\":%lld}",
                    e->last_flag ? e->last_flag : 'R',
                    (long long)e->last_seen_ns, (long long)e->first_seen_ns);
            n++;
        }
    }
    fprintf(f, "\n  ],\n  \"now\": %lld,\n", (long long)sc_now_ns());
    fprintf(f, "  \"count\": %ld,\n  \"reports\": %ld,\n", n, t->total_reports);
    fprintf(f, "  \"evicted\": %ld,\n  \"refused\": 0\n}\n", t->total_evicted);
    int bad = ferror(f);
    if (fclose(f) != 0 || bad) {
        unlink(tmp);
        return -1;
    }
    return rename(tmp, path);
}

int main(int argc, char **argv)
{
    sched_policy_t policy      = POLICY_PRIORITY;
    int            workers     = 8;
    int64_t        run_ms      = 3000;
    int            service_us  = 200;
    const char    *log_path    = "missions.log";
    /* Story vi of the course documentation: the log records contact
       detected, priority assigned, worker processing started, track
       updated and alert generated. Three of them are one line for each
       report. */
    int            log_full    = 1;
    const char    *feed_path   = FEED;
    const char    *live_path   = NULL;
    int64_t        live_ms     = 500;
    const char    *tracks_path = NULL;
    const char    *expected_path = NULL;
    int            cpus       = 1;   /* the centre runs on ONE processor */
    int            late_ms   = POOL_LATE_DEFAULT_MS;
    size_t         queue_cap = 8192;

    for (int i = 1; i < argc; i++) {
        int has_next = (i + 1 < argc);
        if (strcmp(argv[i], "--workers") == 0 && has_next) {
            workers = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--sched") == 0 && has_next) {
            if (sched_policy_parse(argv[++i], &policy) != 0) {
                fprintf(stderr, "unknown policy \"%s\"\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--ms") == 0 && has_next) {
            run_ms = strtoll(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--service-us") == 0 && has_next) {
            service_us = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--log") == 0 && has_next) {
            log_path = argv[++i];
        } else if (strcmp(argv[i], "--log-detail") == 0 && has_next) {
            /* The same option as the complete centre, so a student meets one
               form in Lab 3 and in Lab 4. full records each step of each
               report; events records only the events. */
            const char *d = argv[++i];
            if (strcmp(d, "full") == 0) {
                log_full = 1;
            } else if (strcmp(d, "events") == 0) {
                log_full = 0;
            } else {
                fprintf(stderr, "unknown log detail \"%s\": "
                                "use full or events\n", d);
                return 2;
            }
        } else if (strcmp(argv[i], "--feed") == 0 && has_next) {
            feed_path = argv[++i];
        } else if (strcmp(argv[i], "--live") == 0 && has_next) {
            live_path = argv[++i];
        } else if (strcmp(argv[i], "--live-ms") == 0 && has_next) {
            live_ms = strtoll(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--tracks") == 0 && has_next) {
            tracks_path = argv[++i];
        } else if (strcmp(argv[i], "--expected") == 0 && has_next) {
            expected_path = argv[++i];
        } else if (strcmp(argv[i], "--cpus") == 0 && has_next) {
            /* How many processors the CENTRE can use. One by default: the
               workers then share one processor, as SENTINEL must run. "all"
               is the contrast run. The sensor processes are not restricted:
               sensor_spawn gives the mask back in the child. */
            const char *cpus_want = argv[++i];
            if (strcmp(cpus_want, "all") == 0) {
                cpus = 0;
            } else {
                cpus = (int)strtol(cpus_want, NULL, 10);
                if (cpus < 1) {
                    fprintf(stderr, "--cpus must be 1 or more, or \"all\"\n");
                    return 2;
                }
            }
        } else if (strcmp(argv[i], "--late-ms") == 0 && has_next) {
            /* The report deadline. A routine report that waited longer than
               this is dropped, and is not applied: a late position puts a
               symbol where the target is not. 0 switches it off, and a
               priority report is never dropped for its age. See R2. */
            late_ms = (int)strtol(argv[++i], NULL, 10);
            if (late_ms < 0) {
                fprintf(stderr, "--late-ms must be 0 or more\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--queue") == 0 && has_next) {
            /* HOW MANY REPORTS CAN WAIT FOR A WORKER. A decision with a cost
               on each side. Too small, and a burst is refused. Too large, and
               the centre holds reports so long that they lose their value, and
               the memory is real. 8192 holds four seconds of the 2000 reports
               each second of the satellite, which R3 asks for. Make it small to
               SEE the refusal: --queue 256. */
            queue_cap = (size_t)strtoul(argv[++i], NULL, 10);
            if (queue_cap < 1) {
                fprintf(stderr, "--queue must be 1 or more\n");
                return 2;
            }
        } else {
            fprintf(stderr,
                "usage: %s [--workers N] [--sched fcfs|priority|rr] [--ms N]\n"
                "          [--service-us N] [--log PATH] [--feed PATH]\n"
                "          [--live PATH] [--live-ms N] [--tracks PATH]\n"
                "          [--expected PATH]\n"
                "          [--cpus N|all]   processors for the centre, default 1\n"
                "          [--late-ms N]   the report deadline, 0 to switch it off\n"
                "          [--queue N]     how many reports may wait, default 8192\n"
                "          [--log-detail full|events]  full logs every step\n",
                argv[0]);
            return 2;
        }
    }
    if (workers <= 0 || live_ms <= 0) {
        fprintf(stderr, "--workers and --live-ms must be more than 0\n");
        return 2;
    }

    static traffic_t       traffic;
    static traffic_watch_t watch;
    traffic_watch_init(&watch);
    if (expected_path != NULL) {
        long bad_line = 0;
        if (traffic_load(expected_path, &traffic, &bad_line) != 0) {
            if (bad_line > 0) {
                fprintf(stderr, "%s, line %ld: not an id, a zone or a comment\n",
                        expected_path, bad_line);
            } else {
                fprintf(stderr, "cannot read %s: %s\n", expected_path, strerror(errno));
            }
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_interrupt);

    log_t *lg = log_open(log_path);
    if (lg == NULL) {
        fprintf(stderr, "cannot open %s: %s\n", log_path, strerror(errno));
        return 1;
    }
    log_writef(lg, LOG_SYSTEM, "SENTINEL Part 3 starting: %d workers, policy %s, %lld ms",
               workers, sched_policy_name(policy), (long long)run_ms);

    /* Restrict the centre before a fork and before a thread starts.
       fork() and pthread_create() both keep this mask, so one call covers
       the loop and the whole pool. Report the mask that was READ: a cpuset
       can permit fewer processors than asked, and the call still succeeds. */
    int cpus_online = cpu_count_online();
    if (cpus > 0 && cpu_pin(cpus) != 0) {
        log_writef(lg, LOG_SYSTEM, "cannot restrict to %d processors: %s",
                   cpus, strerror(errno));
    }
    int cpus_allowed = cpu_allowed();
    log_writef(lg, LOG_SYSTEM, "the centre runs on %d of %d processors%s",
               cpus_allowed, cpus_online,
               cpus_allowed == 1 ? ", so its workers share one" : "");

    queue_t q;
    track_t t;
    if (queue_init(&q, queue_cap) != 0 || track_init(&t) != 0) {
        fprintf(stderr, "cannot create the queue and the track picture\n");
        return 1;
    }
    pool_t *pool = pool_create_deadline(workers, &q, &t, service_us, late_ms, lg);
    if (pool == NULL) {
        fprintf(stderr, "pool_create: %s\n", strerror(errno));
        return 1;
    }
    /* "Worker processing started" and "track updated" happen in a
       worker, where this loop cannot see them. */
    pool_set_log_steps(pool, log_full);

    sensor_net_t net;
    if (sensor_net_init(&net, feed_path, lg) != 0) {
        fprintf(stderr, "cannot start the sensor network: %s\n", strerror(errno));
        return 1;
    }
    int started = sensor_spawn_all(&net);

    sched_t *sched = sched_create(policy, 65536);
    if (sched == NULL) {
        fprintf(stderr, "sched_create failed\n");
        return 1;
    }

    printf("SENTINEL Part 3 — worker threads, unprotected\n");
    printf("  workers         %d\n", workers);
    printf("  policy          %s\n", sched_policy_name(policy));
    printf("  sensors started %d of %d\n", started, SENSOR_COUNT);
    printf("  log detail      %s\n\n", log_full
           ? "full (every step: R6 and story vi)"
           : "events (priority, alerts and one summary a second)");

    /* One pipe for each sensor. sensor_spawn made each read end
       non-blocking and closed each write end in the centre, so a channel
       ends when its sensor stops. */
    struct pollfd fds[SENSOR_COUNT];
    long    read_total = 0, rejected = 0, priority_seen = 0, pushed = 0, dropped = 0;
    long    high_seen  = 0;   /* the centre's priority: HIGH (Part 14) */
    long    lost_sched = 0;   /* refused by the scheduler: it was full */
    long    wrong_channel = 0;/* refused: the sensor field did not match */
    long    per_sensor[SENSOR_COUNT] = { 0 };
    int64_t t0 = sc_now_ns();
    int64_t live_last_ns = 0;
    while (!g_interrupted && sensor_net_open_count(&net) > 0) {
        if (run_ms > 0 && (sc_now_ns() - t0) / 1000000 >= run_ms) {
            break;
        }
        sensor_net_pollfds(&net, fds);
        int ready = poll(fds, SENSOR_COUNT, sensor_net_more(&net) ? 0 : 100);
        if (ready < 0 && errno != EINTR) {
            break;
        }

        if (ready >= 0) {
            const char *line;
            size_t      len;
            int         rc;
            sensor_id_t from;
            /* At most SENSOR_LINES_PER_PASS lines from each channel in one
               pass, so that SAT cannot make the other sensors wait. */
            while ((rc = sensor_net_next_line(&net, fds, &from, &line, &len)) != 0) {
                if (rc < 0) {
                    read_total++;           /* EMSGSIZE: one line is lost */
                    rejected++;
                    continue;
                }
                read_total++;

                contact_t c;
                if (contact_parse(line, len, &c) != CONTACT_OK) {
                    rejected++;
                    continue;
                }
                /* The channel names the sender. */
                if (strcmp(c.sensor, sensor_name(from)) != 0) {
                    rejected++;
                    wrong_channel++;
                    continue;
                }
                per_sensor[from]++;
                /* "Contact detected", story vi, and the routine sensor
                   updates of story i. */
                if (log_full) {
                    /* "contact <id> from <sensor>" is the form that demo_lab1
                       writes, and the parser of the page looks for it. With
                       another form, the page counts 0 contacts in a log of
                       17,129 lines. */
                    log_writef(lg, LOG_INFO,
                               "contact %s from %s at %.4f %.4f, %.0f kn, %s",
                               c.id, c.sensor, c.lat, c.lon, c.speed,
                               c.flag == PRIORITY_PRIORITY ? "priority"
                                                           : "routine");
                }
                /* THE PRIORITY OF THE CENTRE (Part 14): HIGH for a contact that is
                   not filed and is in a monitored region. The flag of the sensor
                   in c does not change. */
                const char *region  = NULL;
                urgency_t   urgency = centre_assess(expected_path ? &traffic : NULL,
                                                    &c, &region);
                if (c.flag == PRIORITY_PRIORITY) {
                    priority_seen++;
                }
                if (urgency == URGENCY_HIGH) {
                    high_seen++;
                }
                /* "Priority assigned", story vi. The complete centre writes this
                   line, and so does Lab 3. The scheduler decides the order in both. */
                if (urgency == URGENCY_HIGH) {
                    log_writef(lg, LOG_PRIORITY,
                               "priority contact %s from %s queued for the workers, "
                               "HIGH: unknown in %s", c.id, c.sensor, region);
                } else if (urgency == URGENCY_SENSOR) {
                    log_writef(lg, LOG_PRIORITY,
                               "priority contact %s from %s queued for the workers",
                               c.id, c.sensor);
                }
                if (expected_path != NULL &&
                    traffic_watch(&watch, &traffic, &c, sc_now_ns()) == 1) {
                    log_writef(lg, LOG_ALERT,
                               "unknown contact %s from %s at %.4f %.4f: not filed",
                               c.id, c.sensor, c.lat, c.lon);
                }

                pending_t p;
                memset(&p, 0, sizeof p);
                p.c = c;
                p.arrived_ns = sc_now_ns();
                p.urgency    = urgency;
                p.region     = region;
                /* Do not discard the result. sched_push refuses with ENOSPC when the
                   scheduler is full, and a refused report is lost with no message.
                   On this machine it was never full, because the centre empties it
                   in each turn of the loop. But on ONE processor the centre shares
                   the processor with eight workers, so it runs less often, and the
                   time between two turns grows. */
                if (sched_push(sched, &p) != 0) {
                    lost_sched++;
                }
            }
        }

        pending_t p;
        while (sched_next(sched, &p) == 1) {
            /* The whole pending_t goes in, not only the contact. The worker
               needs arrived_ns to decide if the report waited too long. */
            /* NOT PROTECTED: the workers pop while this thread pushes. */
            if (queue_push(&q, &p) == 0) {
                pushed++;
            } else {
                dropped++;
            }
        }

        if (live_path != NULL && sc_now_ns() - live_last_ns >= live_ms * 1000000) {
            live_last_ns = sc_now_ns();
            live_t lv = {
                .elapsed_ms = (live_last_ns - t0) / 1000000, .running = 1,
                .policy = policy, .read = read_total, .rejected = rejected,
                .priority = priority_seen, .high = high_seen, .pushed = pushed, .dropped = dropped,
                .per_sensor = per_sensor, .net = &net, .queue = &q, .track = &t, .pool = pool,
                .watch = expected_path ? &watch : NULL,
                .cpus_allowed = cpus_allowed, .cpus_online = cpus_online,
                .log_full = log_full,
                .lost_sched = lost_sched,
            };
            if (write_live(live_path, &lv) != 0) {
                fprintf(stderr, "cannot write %s: %s\n", live_path, strerror(errno));
                live_path = NULL;
            }
        }

        /* Collect each sensor that stopped, in each pass. sensor_reap asks
           about each sensor with WNOHANG, so it never waits. */
        sensor_reap(&net);
    }

    int64_t elapsed = sc_now_ns() - t0;
    sensor_reap(&net);

    /* pool_stop drains the queue, and joins the workers. After it, the
       picture and the counters have one user again. When the count is
       corrupt, the drain never ends, and pool_stop does not return. */
    stopper_t st = { .pool = pool, .done = 0 };
    pthread_t stop_thread;
    int       stalled = 0;
    if (pthread_create(&stop_thread, NULL, stopper, &st) != 0) {
        pool_stop(pool);
    } else {
        for (int waited_ms = 0; !atomic_load(&st.done); waited_ms += 20) {
            if (waited_ms >= 5000) {
                stalled = 1;
                break;
            }
            usleep(20000);
        }
        if (!stalled) {
            pthread_join(stop_thread, NULL);
        }
    }

    if (stalled) {
        /* The workers still run, so the picture has another user, and this
           thread must not walk it. Each value below belongs to this thread. */
        log_writef(lg, LOG_ALERT,
                   "the workers did not stop in 5 s: the queue count says %zu reports wait "
                   "that were never pushed. Two unprotected decrements lost count",
                   queue_count(&q));
        if (live_path != NULL) {
            live_t lv = {
                .elapsed_ms = elapsed / 1000000, .running = 0, .stalled = 1,
                .policy = policy, .read = read_total, .rejected = rejected,
                .priority = priority_seen, .high = high_seen, .pushed = pushed, .dropped = dropped,
                .per_sensor = per_sensor, .net = &net, .queue = &q, .track = &t, .pool = pool,
                .watch = expected_path ? &watch : NULL,
                .cpus_allowed = cpus_allowed, .cpus_online = cpus_online,
                .log_full = log_full,
                .lost_sched = lost_sched,
            };
            write_live(live_path, &lv);
        }
        fflush(stdout);
        printf("\nrun stopped after %.0f ms\n\n", (double)elapsed / 1e6);
        printf("  reports read      %ld\n", read_total);
        printf("  queued            %ld\n", pushed);
        printf("  queue says        %zu reports still wait, in a queue of %zu\n",
               queue_count(&q), queue_cap);
        printf("\n  THE WORKERS DID NOT STOP. Two of them took one report away from the\n");
        printf("  count at the same instant, so the count no longer matches the queue.\n");
        printf("  It never reaches 0, so the drain never ends. The track picture is not\n");
        printf("  written: the workers still hold it. This is R4, and Lab 4 answers it.\n");
        /* No log_close: the workers still write to the log. _exit() closes
           each descriptor after the last thread stops. */
        fflush(stdout);
        _exit(0);          /* the workers still run: leave, and free nothing */
    }

    long processed = pool_total_processed(pool);
    long counted   = track_total_reports(&t);
    long tracks    = track_total_tracks(&t);
    long walked    = track_count_entries(&t);

    if (live_path != NULL) {
        live_t lv = {
            .elapsed_ms = elapsed / 1000000, .running = 0,
            .policy = policy, .read = read_total, .rejected = rejected,
            .priority = priority_seen, .high = high_seen, .pushed = pushed, .dropped = dropped,
            .per_sensor = per_sensor, .net = &net, .queue = &q, .track = &t, .pool = pool,
            .watch = expected_path ? &watch : NULL,
                .cpus_allowed = cpus_allowed, .cpus_online = cpus_online,
                .log_full = log_full,
                .lost_sched = lost_sched,
        };
        write_live(live_path, &lv);
    }
    if (tracks_path != NULL) {
        if (write_tracks(&t, tracks_path) != 0) {
            fprintf(stderr, "cannot write %s: %s\n", tracks_path, strerror(errno));
        } else {
            log_writef(lg, LOG_TRACK, "track picture written to %s (%ld tracks)",
                       tracks_path, tracks);
        }
    }

    printf("run complete in %.0f ms\n\n", (double)elapsed / 1e6);
    printf("  reports read      %ld\n", read_total);
    printf("  rejected          %ld (%ld of them on the wrong channel)\n",
           rejected, wrong_channel);
    printf("  queued            %ld (dropped %ld, queue full)\n", pushed, dropped);
    printf("  priority          %ld (the sensor's P)\n", priority_seen);
    if (expected_path != NULL) {
        printf("  centre HIGH       %ld (not filed, in a monitored region)\n", high_seen);
    }
    for (int s = 0; s < SENSOR_COUNT; s++) {
        printf("    %-4s            %ld\n", sensor_name((sensor_id_t)s), per_sensor[s]);
    }
    printf("\n  workers\n");
    for (int i = 0; i < workers; i++) {
        printf("    worker %-2d       %ld processed\n", i, pool_worker_stats(pool, i)->processed);
    }
    printf("\n  processed         %ld of %ld queued\n", processed, pushed);
    printf("  late              %ld (waited past the %d ms deadline)\n",
           pool_total_late(pool), late_ms);
    printf("  picture counted   %ld reports, %ld tracks (%ld walked)\n", counted, tracks, walked);

    /* --- THE LOSS LEDGER ---------------------------------------------
     *
     * Each report that the sensors sent reached the picture, or has a NAMED
     * reason why it did not, and the two sides must add up.
     *
     * HERE THEY CAN DISAGREE, AND THAT IS THE RESULT. Nothing in this lab is
     * protected, so `counted` is a shared counter that many workers add to at
     * the same time, and `processed` is read while the workers write it. The
     * ledger has no fault: it REPORTS that its numbers cannot be trusted.
     * Lab 4 makes the same line balance.
     *
     * With ONE worker it balances. That is the control: the arithmetic is
     * correct, and the sharing is wrong. */
    long still_queued = (long)queue_count(&q);
    long late         = pool_total_late(pool);
    long accounted    = rejected + lost_sched + dropped + late + counted + still_queued;

    printf("\n  LOSS LEDGER\n");
    printf("    read from the sensors        %8ld\n", read_total);
    printf("      malformed, rejected        %8ld\n", rejected);
    printf("      refused by the scheduler   %8ld\n", lost_sched);
    printf("      dropped, the queue was full%8ld\n", dropped);
    printf("      dropped, past the deadline %8ld\n", late);
    printf("      still waiting in the queue %8ld\n", still_queued);
    printf("      REACHED THE PICTURE        %8ld\n", counted);
    printf("    -----------------------------%8s\n", "--------");
    printf("    accounted for                %8ld   %s\n", accounted,
           accounted == read_total ? "BALANCED"
                                   : "DOES NOT BALANCE -- nothing here is protected (R4)");
    printf("    queue %zu slots, deepest %zu, %ld refused\n",
           queue_cap, queue_peak(&q), queue_dropped(&q));
    if (expected_path != NULL) {
        printf("  unknown contacts  %d objects, %ld reports\n", watch.n, watch.reports);
    }

    /* The verdict of Lab 3. It is exact only with one worker. With more,
       the numbers above can disagree, and Lab 4 makes them agree. A report
       that was late was taken from the queue and not applied, so it counts
       with the processed ones against the queued ones. */
    int exact = (processed + late == pushed) && (counted == processed) && (tracks == walked);
    printf("\n  shared data %s\n", exact
           ? "agrees this time (1 worker, or a lucky run)"
           : "DISAGREES: unprotected workers lost or repeated updates (Part 4 fixes this)");

    log_writef(lg, LOG_SYSTEM,
               "SENTINEL Part 3 complete: %ld read, %ld queued, %ld processed, %ld counted, %ld tracks",
               read_total, pushed, processed, counted, tracks);

    pool_destroy(pool);
    sched_destroy(sched);
    sensor_net_shutdown(&net);
    queue_destroy(&q);
    track_destroy(&t);
    traffic_free(&traffic);
    log_close(lg);
    return 0;
}
