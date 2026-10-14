/* demo_lab2 — the demonstration program of Lab 2.
 *
 * GIVEN CODE. You read this file, and you do not change it.
 *
 * It runs the whole sensor network: four sensor processes, each with its
 * own pipe; a named FIFO that brings urgent contacts from outside; poll()
 * on all five channels; and a scheduler policy that chooses the next
 * report. With --expected, the centre also gives its own class: HIGH for a
 * contact that is not filed and is in a monitored region.
 *
 * It shows the two requirements of Lab 2:
 *
 *   R1  the four sensors report at the same time, and the centre continues
 *       when one sensor stops
 *   R2  a priority contact overtakes routine traffic
 *
 *   demo_lab2 [--sched fcfs|priority|rr] [--ms N] [--service-us N]
 *             [--kill-sensor NWS|AUR|AIS|SAT] [--log PATH] [--feed PATH]
 *             [--expected PATH] [--cpus N|all]
 *
 * --feed names the program that each sensor process runs: contactfeed by
 * default. The demo does not start a stopped sensor again.
 *
 * Exit status: 0 when each requirement line says yes, 1 when one says NO or
 * when a call fails, and 2 for a wrong argument.
 */

#include "sentinel/proc.h"
#include "cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

#ifndef SENTINEL_BUILD_DIR
#define SENTINEL_BUILD_DIR "./build"
#endif

#define FEED  SENTINEL_BUILD_DIR "/contactfeed"
#define FIFOP "/tmp/sentinel_priority.fifo"

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [--sched fcfs|priority|rr] [--ms N]\n"
        "          [--service-us N] [--kill-sensor NWS|AUR|AIS|SAT]\n"
        "          [--log PATH] [--feed PATH] [--expected PATH]\n"
        "          [--cpus N|all]   processors for the centre, default 1\n", prog);
}

int main(int argc, char **argv)
{
    sched_policy_t policy        = POLICY_PRIORITY;
    int64_t        run_ms        = 2000;
    int            service_us    = 500;
    const char    *log_path      = "/tmp/sentinel_lab2.log";
    sensor_id_t    kill_id       = SENSOR_COUNT;    /* none */
    int            cpus          = 1;   /* the centre runs on ONE processor */
    const char    *feed_path     = FEED;
    const char    *expected_path = NULL;  /* no class from the centre */

    for (int i = 1; i < argc; i++) {
        int has_next = (i + 1 < argc);
        if (strcmp(argv[i], "--sched") == 0 && has_next) {
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
        } else if (strcmp(argv[i], "--feed") == 0 && has_next) {
            feed_path = argv[++i];
        } else if (strcmp(argv[i], "--expected") == 0 && has_next) {
            expected_path = argv[++i];
        } else if (strcmp(argv[i], "--cpus") == 0 && has_next) {
            /* How many processors the CENTRE can use. The default is one,
               the machine that SENTINEL must run on. "all" removes the
               limit, for a comparison. The sensor processes have no limit:
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
        } else if (strcmp(argv[i], "--kill-sensor") == 0 && has_next) {
            const char *want = argv[++i];
            for (int j = 0; j < SENSOR_COUNT; j++) {
                if (strcmp(want, sensor_name((sensor_id_t)j)) == 0) {
                    kill_id = (sensor_id_t)j;
                }
            }
            if (kill_id == SENSOR_COUNT) {
                fprintf(stderr, "unknown sensor \"%s\"\n", want);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    /* A sensor whose centre has stopped must not kill this process. */
    signal(SIGPIPE, SIG_IGN);

    /* The traffic that the centre expects, with the regions it monitors. */
    traffic_t traffic;
    memset(&traffic, 0, sizeof traffic);
    if (expected_path != NULL) {
        long bad_line = 0;
        if (traffic_load(expected_path, &traffic, &bad_line) != 0) {
            fprintf(stderr, "cannot read %s (line %ld): %s\n",
                    expected_path, bad_line, strerror(errno));
            return 2;
        }
    }

    int            status    = 1;     /* 0 only when each check passes */
    int            failed    = 0;     /* a requirement line said NO */
    log_t         *lg        = NULL;
    sched_t       *s         = NULL;
    int            net_ready = 0;
    int            fifo_open = 0;
    pid_t          helper    = -1;
    sensor_net_t   net;
    prio_channel_t ch;

    lg = log_open(log_path);
    if (lg == NULL) {
        fprintf(stderr, "cannot open %s: %s\n", log_path, strerror(errno));
        goto done;
    }
    log_writef(lg, LOG_SYSTEM, "SENTINEL Part 2 starting, policy=%s",
               sched_policy_name(policy));

    /* Restrict the centre before it starts a child. Each child gets the
       mask, so each sensor gives it back in sensor_spawn. */
    int cpus_online = cpu_count_online();
    if (cpus > 0 && cpu_pin(cpus) != 0) {
        log_writef(lg, LOG_SYSTEM, "cannot restrict to %d processors: %s",
                   cpus, strerror(errno));
    }
    log_writef(lg, LOG_SYSTEM, "the centre runs on %d of %d processors",
               cpu_allowed(), cpus_online);

    if (sensor_net_init(&net, feed_path, lg) != 0) {
        fprintf(stderr, "sensor_net_init: %s\n", strerror(errno));
        goto done;
    }
    net_ready = 1;

    if (prio_open(&ch, FIFOP) != 0) {
        fprintf(stderr, "prio_open: %s\n", strerror(errno));
        goto done;
    }
    fifo_open = 1;

    int started = sensor_spawn_all(&net);
    printf("SENTINEL Part 2 — operations centre\n");
    printf("  sensors started   %d of %d\n", started, SENSOR_COUNT);
    printf("  policy            %s\n", sched_policy_name(policy));
    printf("  work per report   %d us\n", service_us);
    printf("  priority channel  %s\n", FIFOP);
    printf("  mission log       %s\n\n", log_path);

    /* A helper process sends an urgent contact through the FIFO while the
       centre runs, and can stop one sensor. Flush first, so that the child
       does not get a copy of the text above. */
    fflush(stdout);
    helper = fork();
    if (helper < 0) {
        fprintf(stderr, "fork: %s\n", strerror(errno));
        goto done;
    }
    if (helper == 0) {
        usleep(300000);

        const char *urgent =
            "1755534061.482913 NWS ACCF-URGENT 68.7100 -105.7000 600.0 P\n";
        if (prio_send(FIFOP, urgent) != 0) {
            _exit(1);
        }

        if (kill_id != SENSOR_COUNT && net.sensors[kill_id].pid > 0) {
            usleep(200000);
            kill(net.sensors[kill_id].pid, SIGKILL);
        }
        _exit(0);
    }

    /* Make the scheduler after the fork, so that the helper does not hold a
       copy of it. valgrind follows a child of fork() that does not exec, and
       it would report the copy as lost when the helper calls _exit(). */
    s = sched_create(policy, 65536);
    if (s == NULL) {
        fprintf(stderr, "sched_create: %s\n", strerror(errno));
        goto done;
    }

    int64_t        t0 = sc_now_ns();
    centre_stats_t st;
    if (centre_run(&net, &ch, s, expected_path ? &traffic : NULL,
                   run_ms, service_us, &st) != 0) {
        fprintf(stderr, "centre_run: %s\n", strerror(errno));
        goto done;
    }
    int64_t elapsed = sc_now_ns() - t0;

    while (waitpid(helper, NULL, 0) < 0 && errno == EINTR) {
        /* wait again */
    }
    helper = -1;

    /* printf keeps its text in a buffer, and centre_stats_write uses
       write() directly. Without this flush, the statistics appear before
       the heading. */
    fflush(stdout);
    if (centre_stats_write(STDOUT_FILENO, &st, policy, elapsed) != 0) {
        fprintf(stderr, "centre_stats_write: %s\n", strerror(errno));
        goto done;
    }

    printf("\nrequirements\n");

    int r1_all = 1;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (st.per_sensor[i] == 0) {
            r1_all = 0;
        }
    }
    printf("  R1 four sensors report at once      %s\n",
           r1_all ? "yes" : "NO");
    if (!r1_all) {
        failed = 1;
    }

    if (kill_id != SENSOR_COUNT) {
        /* The centre must still serve, and the other three sensors must
           still run. Each sensor process runs for the whole run. SAT sends
           nothing between two passes, but its process does not stop. */
        int killed_is_gone = (net.sensors[kill_id].pid == 0);
        int steady_alive   = 1;
        for (int i = 0; i < SENSOR_COUNT; i++) {
            if ((sensor_id_t)i != kill_id && net.sensors[i].pid == 0) {
                steady_alive = 0;
            }
        }
        int ok = killed_is_gone && steady_alive && st.accepted > 0;
        printf("  R1 centre survives a sensor death   %s "
               "(%s killed, steady sensors alive, %ld reports served)\n",
               ok ? "yes" : "NO", sensor_name(kill_id), st.accepted);
        if (!ok) {
            failed = 1;
        }
    }

    printf("  R2 priority contact overtakes       ");
    if (st.prio_lat_count > 0 && st.lat_count > st.prio_lat_count) {
        double p_mean   = (double)st.prio_lat_sum_ns /
                          (double)st.prio_lat_count / 1e6;
        double all_mean = (double)st.lat_sum_ns / (double)st.lat_count / 1e6;
        if (policy == POLICY_PRIORITY) {
            int ok = p_mean < all_mean;
            printf("%s (priority waits %.1f ms, all traffic %.1f ms)\n",
                   ok ? "yes" : "NO", p_mean, all_mean);
            if (!ok) {
                failed = 1;
            }
        } else {
            /* Only the priority policy promises R2. The other two policies
               show the wait without it, for the comparison. */
            printf("n/a under %s (priority waits %.1f ms, all traffic %.1f ms)\n",
                   sched_policy_name(policy), p_mean, all_mean);
        }
    } else if (policy == POLICY_PRIORITY) {
        printf("NO (no priority traffic seen)\n");
        failed = 1;
    } else {
        printf("n/a (no priority traffic seen)\n");
    }

    if (expected_path != NULL) {
        printf("  centre priority HIGH                %ld reports (not filed, "
               "in a monitored region)\n", st.high);
    }
    printf("  R6 every action timestamped         yes (%s)\n", log_path);
    printf("  urgent contact via FIFO             %s\n",
           st.from_prio_channel > 0 ? "received" : "NOT received");
    if (st.from_prio_channel == 0) {
        failed = 1;
    }

    log_writef(lg, LOG_SYSTEM,
               "Part 2 complete: %ld accepted, %ld priority, %.3f ms",
               st.accepted, st.priority, (double)elapsed / 1e6);

    status = failed ? 1 : 0;

done:
    /* One path out, so that an error also stops the sensors and removes the
       FIFO. A FIFO that stays in /tmp makes the next run replace it. */
    fflush(stdout);
    if (helper > 0) {
        kill(helper, SIGKILL);
        while (waitpid(helper, NULL, 0) < 0 && errno == EINTR) {
            /* wait again */
        }
    }
    sched_destroy(s);
    if (fifo_open) {
        prio_close(&ch);
    }
    if (net_ready) {
        sensor_net_shutdown(&net);
    }
    traffic_free(&traffic);
    if (lg != NULL && log_close(lg) != 0) {
        fprintf(stderr, "log_close: %s\n", strerror(errno));
        status = 1;
    }
    return status;
}
