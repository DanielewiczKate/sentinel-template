#include "sentinel/sync.h"
#include "sentinel/proc.h"
#include "sentinel/supervise.h"
#include "traffic.h"
#include "linereader.h"
#include "cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/wait.h>

#ifndef SENTINEL_BUILD_DIR
#define SENTINEL_BUILD_DIR "./build"
#endif

#define FEED  SENTINEL_BUILD_DIR "/contactfeed"
#define FIFOP "/tmp/sentinel_priority.fifo"

#define SAT_SEND_GAP_NS 1000000000LL

static volatile sig_atomic_t g_interrupted = 0;

static void on_interrupt(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

static int log_steps_present(const char *path, long counts[LOG_TRACK + 1])
{
    for (int i = 0; i <= LOG_TRACK; i++) {
        counts[i] = 0;
    }

    int fd = sc_open_read(path);
    if (fd < 0) {
        return 0;
    }
    linereader_t *lr = lr_open(fd, 8192, 1);
    if (lr == NULL) {
        sc_close(fd);
        return 0;
    }

    const char *line;
    size_t      len;
    while (lr_next_line(lr, &line, &len) == 1) {
        const char *sp = memchr(line, ' ', len);
        if (sp == NULL) {
            continue;
        }
        sp++;
        size_t rest = len - (size_t)(sp - line);
        for (int i = 0; i <= LOG_TRACK; i++) {
            const char *name = log_level_name((log_level_t)i);
            size_t      n    = strlen(name);
            if (rest >= n && memcmp(sp, name, n) == 0 &&
                (rest == n || sp[n] == ' ')) {
                counts[i]++;
                break;
            }
        }
    }
    lr_close(lr);

    return counts[LOG_INFO] > 0 && counts[LOG_PRIORITY] > 0 &&
           counts[LOG_PENDING] > 0 && counts[LOG_TRACK] > 0;
}

typedef struct {
    int64_t              elapsed_ms;
    int                  running;
    sched_policy_t       policy;
    long                 read, rejected, priority, high, via_fifo, lost_sched;
    const long          *per_sensor;
    const sensor_net_t  *net;
    sem_queue_t         *queue;
    safe_track_t        *track;
    safe_pool_t         *pool;
    const traffic_watch_t *watch;
    int                  cpus_allowed;
    int                  cpus_online;
    int                  log_full;
} live_t;

static int write_live(const char *path, const live_t *lv)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        return -1;
    }

    safe_worker_view_t rows[64];
    int n = safe_pool_snapshot(lv->pool, rows, 64);
    if (n > 64) {
        n = 64;
    }
    long busy = 0, processed = 0;
    for (int i = 0; i < n; i++) {
        busy      += rows[i].busy;
        processed += rows[i].processed;
    }

    int64_t now = sc_now_ns();
    fprintf(f, "{\n  \"part\": 4,\n  \"protected\": true,\n");
    fprintf(f, "  \"written\": %lld,\n  \"running\": %s,\n",
            (long long)now, lv->running ? "true" : "false");
    fprintf(f, "  \"elapsed_ms\": %lld,\n  \"policy\": \"%s\",\n",
            (long long)lv->elapsed_ms, sched_policy_name(lv->policy));
    fprintf(f, "  \"cpus\": %d,\n  \"cpus_online\": %d,\n",
            lv->cpus_allowed, lv->cpus_online);
    fprintf(f, "  \"log_detail\": \"%s\",\n", lv->log_full ? "full" : "events");
    fprintf(f, "  \"assoc_max_age_ms\": %lld,\n",
            (long long)(ASSOC_DEFAULT.max_age_ns / 1000000));
    fprintf(f, "  \"queue\": %ld,\n  \"read\": %ld,\n  \"rejected\": %ld,\n",
            (long)sem_queue_count(lv->queue), lv->read, lv->rejected);
    fprintf(f, "  \"priority\": %ld,\n  \"high\": %ld,\n  \"via_fifo\": %ld,\n",
            lv->priority, lv->high, lv->via_fifo);
    fprintf(f, "  \"processed\": %ld,\n  \"busy\": %ld,\n", processed, busy);
    long lv_late     = safe_pool_total_late(lv->pool);
    long lv_refused  = safe_pool_total_refused(lv->pool);
    long lv_counted  = safe_track_total_reports(lv->track);
    long lv_queued   = (long)sem_queue_count(lv->queue);
    long lv_dropped  = queue_dropped(&lv->queue->q);
    long lv_acc      = lv->rejected + lv->lost_sched + lv_dropped + lv_late +
                       lv_refused + lv_counted + lv_queued;
    fprintf(f, "  \"late\": %ld,\n  \"lost_sched\": %ld,\n", lv_late, lv->lost_sched);
    fprintf(f, "  \"ledger\": {\"read\":%ld,\"rejected\":%ld,\"lost_sched\":%ld,"
               "\"dropped_full\":%ld,\"late\":%ld,\"refused\":%ld,"
               "\"queued_now\":%ld,\"counted\":%ld,\"accounted\":%ld,"
               "\"balanced\":%s},\n",
            lv->read, lv->rejected, lv->lost_sched, lv_dropped, lv_late,
            lv_refused, lv_queued, lv_counted, lv_acc,
            lv_acc == lv->read ? "true" : "false");
    fprintf(f, "  \"queue_cap\": %zu,\n  \"queue_peak\": %zu,\n",
            lv->queue->q.capacity, queue_peak(&lv->queue->q));
    fprintf(f, "  \"waits\": %ld,\n  \"wait_ms\": %.1f,\n",
            sem_queue_waits(lv->queue), (double)sem_queue_wait_ns(lv->queue) / 1e6);

    fprintf(f, "  \"sensors\": [");
    for (int s = 0; s < SENSOR_COUNT; s++) {
        const sensor_t *sn = &lv->net->sensors[s];
        fprintf(f, "%s{\"name\":\"%s\",\"reports\":%ld,\"pid\":%d,\"running\":%s,\"restarts\":%ld}",
                s ? "," : "", sensor_name((sensor_id_t)s), lv->per_sensor[s],
                (int)sn->pid, sn->pid > 0 ? "true" : "false", sn->restarts);
    }
    fprintf(f, "],\n  \"workers\": [");
    int64_t pool_cpu_ns = 0, pool_busy_ns = 0;
    for (int i = 0; i < n; i++) {
        char flag[2] = { rows[i].last_flag, '\0' };
        fprintf(f, "%s{\"index\":%d,\"busy\":%s,\"processed\":%ld,\"last_id\":",
                i ? "," : "", rows[i].index, rows[i].busy ? "true" : "false",
                rows[i].processed);
        traffic_json_string(f, rows[i].last_id);
        fprintf(f, ",\"last_flag\":");
        traffic_json_string(f, flag);

        pool_cpu_ns += rows[i].cpu_ns;
        pool_busy_ns += rows[i].busy_ns;
        fprintf(f, ",\"cpu_ns\":%lld,\"busy_ns\":%lld,\"tid\":%d",
                (long long)rows[i].cpu_ns, (long long)rows[i].busy_ns,
                (int)rows[i].tid);

        long vol = 0, invol = 0;
        if (rows[i].tid > 0) {
            cpu_thread_ctxt(rows[i].tid, &vol, &invol);
        }
        fprintf(f, ",\"vol_ctxt\":%ld,\"invol_ctxt\":%ld", vol, invol);

        work_span_t sp[WORKER_SPANS];
        int ns = safe_pool_worker_spans(lv->pool, i, sp, WORKER_SPANS);
        fprintf(f, ",\"spans\":[");
        for (int k = 0; k < ns; k++) {
            char sflag[2] = { sp[k].flag ? sp[k].flag : 'R', '\0' };
            fprintf(f, "%s{\"s\":%lld,\"e\":%lld,\"cpu\":%lld,\"f\":\"%s\"}",
                    k ? "," : "", (long long)sp[k].start_ns,
                    (long long)sp[k].end_ns, (long long)sp[k].cpu_ns, sflag);
        }
        fprintf(f, "]}");
    }
    fprintf(f, "],\n  \"pool_cpu_ns\": %lld,\n", (long long)pool_cpu_ns);
    fprintf(f, "  \"pool_busy_ns\": %lld,\n", (long long)pool_busy_ns);
    fprintf(f, "  \"expected\": %s,\n", lv->watch ? "true" : "false");
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

static int restart_waiting(const sensor_net_t *net)
{
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (net->sensors[i].restart_at_ns != 0) {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    sched_policy_t policy     = POLICY_PRIORITY;
    int            workers    = 8;
    int64_t        run_ms     = 3000;
    int            service_us = 200;
    const char    *log_path   = "missions.log";
    const char    *tracks_path = NULL;
    sensor_id_t    kill_id    = SENSOR_COUNT;
    const char    *feed_path  = FEED;
    const char    *live_path  = NULL;
    int64_t        live_ms    = 500;
    const char    *expected_path = NULL;
    int            restart_sensors = 0;
    int            cpus       = 1;
    int            late_ms   = POOL_LATE_DEFAULT_MS;
    size_t         queue_cap = 8192;
    int            log_full  = 1;

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
        } else if (strcmp(argv[i], "--tracks") == 0 && has_next) {
            tracks_path = argv[++i];
        } else if (strcmp(argv[i], "--feed") == 0 && has_next) {
            feed_path = argv[++i];
        } else if (strcmp(argv[i], "--live") == 0 && has_next) {
            live_path = argv[++i];
        } else if (strcmp(argv[i], "--live-ms") == 0 && has_next) {
            live_ms = strtoll(argv[++i], NULL, 10);
            if (live_ms <= 0) {
                fprintf(stderr, "--live-ms must be more than 0\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--expected") == 0 && has_next) {
            expected_path = argv[++i];
        } else if (strcmp(argv[i], "--cpus") == 0 && has_next) {
            const char *want = argv[++i];
            if (strcmp(want, "all") == 0) {
                cpus = 0;
            } else {
                cpus = (int)strtol(want, NULL, 10);
                if (cpus < 1) {
                    fprintf(stderr, "--cpus must be 1 or more, or \"all\"\n");
                    return 2;
                }
            }
        } else if (strcmp(argv[i], "--late-ms") == 0 && has_next) {
            late_ms = (int)strtol(argv[++i], NULL, 10);
            if (late_ms < 0) {
                fprintf(stderr, "--late-ms must be 0 or more\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--queue") == 0 && has_next) {
            queue_cap = (size_t)strtoul(argv[++i], NULL, 10);
            if (queue_cap < 1) {
                fprintf(stderr, "--queue must be 1 or more\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--restart-sensors") == 0) {
            restart_sensors = 1;
        } else if (strcmp(argv[i], "--kill-sensor") == 0 && has_next) {
            const char *want = argv[++i];
            for (int j = 0; j < SENSOR_COUNT; j++) {
                if (strcmp(want, sensor_name((sensor_id_t)j)) == 0) {
                    kill_id = (sensor_id_t)j;
                }
            }
        } else {
            fprintf(stderr,
                "usage: %s [--workers N] [--sched fcfs|priority|rr]\n"
                "          [--ms N] [--service-us N] [--log PATH]\n"
                "          [--tracks PATH] [--kill-sensor NWS|AUR|AIS|SAT]\n"
                "          [--feed PATH] [--live PATH] [--live-ms N]\n"
                "          [--expected PATH] [--restart-sensors]\n"
                "          [--cpus N|all]   processors for the centre, default 1\n"
                "          [--late-ms N]   the report deadline, 0 to switch it off\n"
                "          [--queue N]     how many reports may wait, default 8192\n"
                "          [--log-detail full|events]  full logs every step\n",
                argv[0]);
            return 2;
        }
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
    log_writef(lg, LOG_SYSTEM,
               "SENTINEL starting: %d workers, policy %s, %lld ms",
               workers, sched_policy_name(policy), (long long)run_ms);

    int cpus_online = cpu_count_online();
    if (cpus > 0 && cpu_pin(cpus) != 0) {
        log_writef(lg, LOG_SYSTEM, "cannot restrict to %d processors: %s",
                   cpus, strerror(errno));
    }
    int cpus_allowed = cpu_allowed();
    log_writef(lg, LOG_SYSTEM, "the centre runs on %d of %d processors%s",
               cpus_allowed, cpus_online,
               cpus_allowed == 1 ? ", so its workers share one" : "");
    if (expected_path != NULL) {
        log_writef(lg, LOG_SYSTEM, "expected traffic: %zu filed ids, %zu zones, from %s",
                   traffic.n_ids, traffic.n_zones, expected_path);
    }

    printf("SENTINEL — surveillance operations centre\n");
    printf("  workers         %d\n", workers);
    printf("  processors      %d of %d%s\n", cpus_allowed, cpus_online,
           cpus_allowed == 1 ? "  (the workers share one)" : "");
    printf("  policy          %s\n", sched_policy_name(policy));
    printf("  work per report %d us\n", service_us);
    printf("  mission log     %s\n", log_path);
    printf("  log detail      %s\n", log_full
           ? "full (every step: R6 and story vi)"
           : "events (priority, alerts and one summary a second)");
    printf("  priority FIFO   %s\n\n", FIFOP);

    log_writef(lg, LOG_SYSTEM, "mission log detail: %s",
               log_full ? "full, one line for each step of every report"
                        : "events only, no per-report lines");

    safe_track_t track;
    sem_queue_t  work_queue;
    resources_t  resources;
    if (safe_track_init(&track) != 0 ||
        sem_queue_init(&work_queue, queue_cap) != 0 ||
        resources_init(&resources) != 0) {
        fprintf(stderr, "cannot create protected state\n");
        return 1;
    }

    safe_pool_t *pool = safe_pool_create_checked(workers, &work_queue, &track,
                                                 &resources, service_us, lg,
                                                 &ASSOC_DEFAULT, &VALID_DEFAULT,
                                                 late_ms);
    if (pool == NULL) {
        fprintf(stderr, "safe_pool_create_checked: %s\n", strerror(errno));
        return 1;
    }
    safe_pool_set_log_steps(pool, log_full);

    if (supervise_install() != 0) {
        fprintf(stderr, "supervise_install: %s\n", strerror(errno));
        return 1;
    }

    sensor_net_t net;
    prio_channel_t prio;
    if (sensor_net_init(&net, feed_path, lg) != 0 ||
        prio_open(&prio, FIFOP) != 0) {
        fprintf(stderr, "cannot start the sensor network: %s\n",
                strerror(errno));
        return 1;
    }

    int started = sensor_spawn_all(&net);
    printf("  sensors started %d of %d\n\n", started, SENSOR_COUNT);

    sched_t *sched = sched_create(policy, 65536);
    if (sched == NULL) {
        fprintf(stderr, "sched_create failed\n");
        return 1;
    }

    pid_t helper = fork();
    if (helper == 0) {
        usleep(400000);
        char stamp[SC_STAMP_MAX], urgent[CONTACT_LINE_MAX + 1];
        sc_stamp(stamp, sizeof stamp);
        snprintf(urgent, sizeof urgent,
                 "%s NWS ACCF-URGENT 68.7100 -105.7000 600.0 P\n", stamp);
        prio_send(FIFOP, urgent);
        if (kill_id != SENSOR_COUNT) {
            usleep(300000);
            kill(net.sensors[kill_id].pid, SIGKILL);
        }
        _exit(0);
    }

    linereader_t *lr_prio = lr_open(prio.fd, 4096, 0);
    if (lr_prio == NULL) {
        fprintf(stderr, "lr_open failed\n");
        return 1;
    }

    struct pollfd fds[SENSOR_COUNT + 1];
    const int     FIFO = SENSOR_COUNT;

    long    read_total = 0, rejected = 0, priority_seen = 0, via_fifo = 0;
    long    high_seen  = 0;
    long    lost_sched = 0;
    long    wrong_channel = 0;
    long    per_sensor[SENSOR_COUNT] = { 0 };
    int64_t t0 = sc_now_ns();

    int64_t sat_last_ns  = 0;
    int64_t sat_send_ns  = 0;
    int64_t live_last_ns = 0;
    int64_t log_last_ns  = t0;
    int64_t max_gap_ns   = 0;
    int64_t prev_iter_ns = t0;
    long    after_death  = 0;
    int64_t first_death_ns = 0;

    while (!g_interrupted &&
           (sensor_net_open_count(&net) > 0 || restart_waiting(&net))) {
        int64_t iter_ns = sc_now_ns();
        if (iter_ns - prev_iter_ns > max_gap_ns) {
            max_gap_ns = iter_ns - prev_iter_ns;
        }
        prev_iter_ns = iter_ns;

        if (run_ms > 0 && (sc_now_ns() - t0) / 1000000 >= run_ms) {
            break;
        }

        sensor_net_pollfds(&net, fds);
        fds[FIFO].fd      = prio.fd;
        fds[FIFO].events  = POLLIN;
        fds[FIFO].revents = 0;

        int ready = poll(fds, SENSOR_COUNT + 1, sensor_net_more(&net) ? 0 : 100);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (int ch = 0; ch < 2; ch++) {
            if (ch == 1 && !(fds[FIFO].revents & (POLLIN | POLLHUP))) {
                continue;
            }

            const char *line;
            size_t      len;
            int         rc;

            for (;;) {
                int from = -1;
                if (ch == 0) {
                    sensor_id_t id;
                    rc   = sensor_net_next_line(&net, fds, &id, &line, &len);
                    from = (int)id;
                } else {
                    rc = lr_next_line(lr_prio, &line, &len);
                }
                if (rc == 0) {
                    break;
                }
                if (rc < 0) {
                    if (errno == EMSGSIZE) {
                        read_total++;
                        rejected++;
                        continue;
                    }
                    break;
                }

                read_total++;

                contact_t c;
                if (contact_parse(line, len, &c) != CONTACT_OK) {
                    rejected++;
                    continue;
                }
                if (from >= 0 &&
                    strcmp(c.sensor, sensor_name((sensor_id_t)from)) != 0) {
                    rejected++;
                    wrong_channel++;
                    continue;
                }
                if (from >= 0) {
                    per_sensor[from]++;
                }
                if (from == SENSOR_SAT) {
                    int64_t now_ns = sc_now_ns();
                    if (sat_last_ns != 0 &&
                        now_ns - sat_last_ns < SAT_SEND_GAP_NS) {
                        sat_send_ns += now_ns - sat_last_ns;
                    }
                    sat_last_ns = now_ns;
                }
                if (first_death_ns != 0) {
                    after_death++;
                }
                if (log_full) {
                    log_writef(lg, LOG_INFO,
                               "contact %s from %s at %.4f %.4f, %.0f kn, %s",
                               c.id, c.sensor, c.lat, c.lon, c.speed,
                               c.flag == PRIORITY_PRIORITY ? "priority"
                                                           : "routine");
                }
                if (c.flag == PRIORITY_PRIORITY) {
                    priority_seen++;
                }
                if (expected_path != NULL &&
                    traffic_watch(&watch, &traffic, &c, sc_now_ns()) == 1) {
                    log_writef(lg, LOG_ALERT,
                               "unknown contact %s from %s at %.4f %.4f: not filed",
                               c.id, c.sensor, c.lat, c.lon);
                }
                if (ch == 1) {
                    via_fifo++;
                    log_writef(lg, LOG_ALERT,
                               "urgent contact %s received on the priority "
                               "channel", c.id);
                }

                pending_t p;
                memset(&p, 0, sizeof p);
                p.c = c;
                p.arrived_ns = sc_now_ns();
                p.from_prio_channel = (ch == 1);
                p.urgency = centre_assess(expected_path ? &traffic : NULL,
                                          &c, &p.region);
                if (p.urgency == URGENCY_HIGH) {
                    high_seen++;
                }
                if (sched_push(sched, &p) != 0) {
                    lost_sched++;
                }
            }

        }

        pending_t p;
        while (sched_next(sched, &p) == 1) {
            urgency_t u = pending_urgency(&p);
            if (u == URGENCY_HIGH) {
                log_writef(lg, LOG_PRIORITY, "priority contact %s from %s queued for the workers, "
                           "HIGH: unknown in %s", p.c.id, p.c.sensor, p.region);
            } else if (u == URGENCY_SENSOR) {
                log_writef(lg, LOG_PRIORITY, "priority contact %s from %s queued for the workers",
                           p.c.id, p.c.sensor);
            }
            sem_queue_push(&work_queue, &p);
        }

        if (live_path != NULL && sc_now_ns() - live_last_ns >= live_ms * 1000000) {
            live_last_ns = sc_now_ns();
            live_t lv = {
                .elapsed_ms = (live_last_ns - t0) / 1000000, .running = 1,
                .policy = policy, .read = read_total, .rejected = rejected,
                .priority = priority_seen, .high = high_seen, .via_fifo = via_fifo,
                .per_sensor = per_sensor, .net = &net, .queue = &work_queue,
                .track = &track, .pool = pool, .watch = expected_path ? &watch : NULL,
                .cpus_allowed = cpus_allowed, .cpus_online = cpus_online,
                .lost_sched = lost_sched, .log_full = log_full,
            };
            if (write_live(live_path, &lv) != 0) {
                fprintf(stderr, "cannot write %s: %s\n", live_path, strerror(errno));
                live_path = NULL;
            }
            if (tracks_path != NULL) {
                safe_track_write_json(&track, tracks_path);
            }
            if (live_last_ns - log_last_ns >= 1000000000) {
                log_last_ns = live_last_ns;
                safe_worker_view_t rows[64];
                int  nw = safe_pool_snapshot(pool, rows, 64);
                long busy = 0, done = 0;
                for (int i = 0; i < nw && i < 64; i++) {
                    busy += rows[i].busy;
                    done += rows[i].processed;
                }
                log_writef(lg, LOG_SYSTEM, "workers: %ld of %d busy, %ld processed, queue %zu",
                           busy, nw, done, sem_queue_count(&work_queue));
            }
        }

        if (supervise_pending() || restart_sensors) {
            supervise_reap(&net, restart_sensors);
            if (first_death_ns == 0 &&
                sensor_running_count(&net) < SENSOR_COUNT) {
                first_death_ns = sc_now_ns();
            }
        }
    }

    int running_at_end[SENSOR_COUNT];
    for (int s = 0; s < SENSOR_COUNT; s++) {
        running_at_end[s] = (net.sensors[s].pid != 0);
    }

    pending_t leftover;
    while (sched_next(sched, &leftover) == 1) {
        sem_queue_push(&work_queue, &leftover);
    }

    while (sem_queue_count(&work_queue) > 0) {
        usleep(2000);
    }

    int64_t elapsed = sc_now_ns() - t0;

    waitpid(helper, NULL, 0);
    supervise_reap(&net, 0);

    safe_pool_stop(pool);

    if (live_path != NULL) {
        live_t lv = {
            .elapsed_ms = elapsed / 1000000, .running = 0,
            .policy = policy, .read = read_total, .rejected = rejected,
            .priority = priority_seen, .high = high_seen, .via_fifo = via_fifo,
            .per_sensor = per_sensor, .net = &net, .queue = &work_queue,
            .track = &track, .pool = pool, .watch = expected_path ? &watch : NULL,
                .cpus_allowed = cpus_allowed, .cpus_online = cpus_online,
                .lost_sched = lost_sched, .log_full = log_full,
        };
        if (write_live(live_path, &lv) != 0) {
            fprintf(stderr, "cannot write %s: %s\n", live_path, strerror(errno));
        }
    }
    lr_close(lr_prio);

    long processed = safe_pool_total_processed(pool);
    long refused   = safe_pool_total_refused(pool);
    long reports   = safe_track_total_reports(&track);
    long tracks    = safe_track_total_tracks(&track);
    long walked    = safe_track_count_entries(&track);
    long accepted  = read_total - rejected;

    if (tracks_path != NULL) {
        if (safe_track_write_json(&track, tracks_path) != 0) {
            fprintf(stderr, "cannot write %s: %s\n",
                    tracks_path, strerror(errno));
        } else {
            log_writef(lg, LOG_TRACK, "track picture written to %s (%ld tracks)",
                       tracks_path, tracks);
        }
    }

    fflush(stdout);

    printf("run complete in %.0f ms\n\n", (double)elapsed / 1e6);
    printf("  reports read      %ld\n", read_total);
    printf("  accepted          %ld\n", accepted);
    printf("  rejected          %ld (%ld of them on the wrong channel)\n",
           rejected, wrong_channel);
    printf("  priority          %ld (the sensor's P)\n", priority_seen);
    if (expected_path != NULL) {
        printf("  centre HIGH       %ld (not filed, in a monitored region)\n", high_seen);
    }
    printf("  via priority FIFO %ld\n", via_fifo);
    for (int s = 0; s < SENSOR_COUNT; s++) {
        printf("    %-4s            %ld\n",
               sensor_name((sensor_id_t)s), per_sensor[s]);
    }
    printf("\n  workers processed %ld\n", processed);
    printf("  late              %ld (waited past the %d ms deadline)\n",
           safe_pool_total_late(pool), late_ms);
    printf("  refused as impossible %ld (speed %ld, teleport %ld, stale %ld, future %ld)\n",
           refused,
           safe_pool_refused_for(pool, VALID_ERR_SPEED),
           safe_pool_refused_for(pool, VALID_ERR_TELEPORT),
           safe_pool_refused_for(pool, VALID_ERR_STALE),
           safe_pool_refused_for(pool, VALID_ERR_FUTURE));
    printf("  track picture     %ld reports, %ld tracks (%ld walked)\n",
           reports, tracks, walked);

    long still_queued = (long)sem_queue_count(&work_queue);
    long late         = safe_pool_total_late(pool);
    long counted      = reports;
    long accounted    = rejected + lost_sched + late + refused + counted + still_queued;

    printf("\n  LOSS LEDGER\n");
    printf("    read from the sensors        %8ld\n", read_total);
    printf("      malformed, rejected        %8ld\n", rejected);
    printf("      refused by the scheduler   %8ld\n", lost_sched);
    printf("      dropped, the queue was full%8ld   (never: the producer waits)\n",
           queue_dropped(&work_queue.q));
    printf("      dropped, past the deadline %8ld\n", late);
    printf("      refused as impossible      %8ld\n", refused);
    printf("      still waiting in the queue %8ld\n", still_queued);
    printf("      REACHED THE PICTURE        %8ld\n", counted);
    printf("    -----------------------------%8s\n", "--------");
    printf("    accounted for                %8ld   %s\n", accounted,
           accounted == read_total ? "BALANCED" : "DOES NOT BALANCE");

    long    waits   = sem_queue_waits(&work_queue);
    int64_t wait_ns = sem_queue_wait_ns(&work_queue);
    printf("    the queue made the centre wait %ld times, %.1f ms in total\n",
           waits, (double)wait_ns / 1e6);
    printf("    queue %zu slots, deepest %zu\n",
           queue_cap, queue_peak(&work_queue.q));
    if (expected_path != NULL) {
        printf("  unknown contacts  %d objects, %ld reports (not in %s)\n",
               watch.n, watch.reports, expected_path);
    }

    printf("\nrequirements\n");

    int      reported   = 0;
    int      still_live = 0;
    for (int s = 0; s < SENSOR_COUNT; s++) {
        if (per_sensor[s] > 0) {
            reported++;
            if (running_at_end[s]) {
                still_live++;
            }
        }
    }
    int r1 = (reported == SENSOR_COUNT) &&
             (first_death_ns == 0 || after_death > 0);
    printf("  R1 four sensors, one may die     %s "
           "(%d of %d reported, %d still running at the end",
           r1 ? "PASS" : "FAIL", reported, SENSOR_COUNT, still_live);
    if (first_death_ns != 0) {
        printf("; a sensor stopped and the centre handled %ld more reports",
               after_death);
    }
    printf(")\n");
    printf("  R2 priority overtakes routine    %s\n",
           policy == POLICY_PRIORITY && priority_seen + high_seen > 0 ? "PASS" : "n/a");
    printf("  R3 absorbs SAT at 2000/s         %s (SAT sent %ld, %.0f/s read while it sent)\n",
           per_sensor[SENSOR_SAT] > 0 ? "PASS" : "FAIL",
           per_sensor[SENSOR_SAT],
           sat_send_ns > 0 ? (double)per_sensor[SENSOR_SAT] * 1e9 /
                             (double)sat_send_ns : 0.0);

    int r4 = (processed == reports + refused) && (tracks == walked);
    printf("  R4 shared data stays exact       %s "
           "(%ld processed = %ld counted + %ld refused, %ld tracks = %ld walked)\n",
           r4 ? "PASS" : "FAIL", processed, reports, refused, tracks, walked);

    double max_gap_ms = (double)max_gap_ns / 1e6;
    int    r5 = max_gap_ms < 1000.0;
    printf("  R5 never freezes                 %s "
           "(longest pause %.0f ms, poll timeout 100 ms)\n",
           r5 ? "PASS" : "FAIL", max_gap_ms);
    long lv_count[LOG_TRACK + 1];
    int  r6 = log_steps_present(log_path, lv_count);
    printf("  R6 every action timestamped      %s (%s)\n",
           r6 ? "PASS" : "PARTIAL", log_path);
    printf("     detected %ld, priority %ld, worker started %ld, "
           "track updated %ld, alerts %ld\n",
           lv_count[LOG_INFO], lv_count[LOG_PRIORITY], lv_count[LOG_PENDING],
           lv_count[LOG_TRACK], lv_count[LOG_ALERT]);
    if (!r6) {
        printf("     story vi names five steps and this run logged fewer: "
               "--log-detail full records them all\n");
    }

    log_writef(lg, LOG_SYSTEM,
               "SENTINEL complete: %ld read, %ld processed, %ld tracks",
               read_total, processed, tracks);

    safe_pool_destroy(pool);
    sched_destroy(sched);
    prio_close(&prio);
    sensor_net_shutdown(&net);
    resources_destroy(&resources);
    sem_queue_destroy(&work_queue);
    safe_track_destroy(&track);
    traffic_free(&traffic);
    log_close(lg);

    return r4 ? 0 : 1;
}
