/* centre.c — the loop of the operations centre.
 *
 * GIVEN CODE. You read this file, and you do not change it. It calls four
 * of your functions: sched_push, sched_next, sensor_reap, and (through
 * channels.c) the table that sensor_spawn fills.
 *
 * The centre reads five channels with poll(): the pipe of each of the four
 * sensors, and the priority FIFO. It takes whole lines from each channel,
 * gives each report to the scheduler, and serves the report that the policy
 * chooses next. It writes each action to the mission log.
 *
 * WHY poll() AND NOT FIVE CALLS OF read(). A read() on an empty pipe waits.
 * If the centre waited in read() on the quiet FIFO, it would not serve the
 * sensors, and AIS alone sends 50 reports each second. poll() waits on the
 * five descriptors at the same time, and returns when one has data.
 *
 * WHY A LINE READER FOR EACH CHANNEL. A pipe gives a stream of bytes. It
 * does not keep the lines apart, so a read() usually stops in the middle of
 * a line. The reader keeps that part until the rest arrives.
 *
 * THE WAIT THAT THE CENTRE MEASURES starts when a line comes out of its
 * channel, and stops when the scheduler gives the report back. It uses the
 * monotonic clock, so a change of the system clock cannot give a negative
 * wait.
 */

#include "sentinel/proc.h"
#include "linereader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>

/* How long poll() waits when no channel has data. The centre uses this
 * time to collect a stopped sensor and to keep its run limit. A shorter time
 * uses more processor time for nothing; a longer time notices a stop later. */
#define POLL_TIMEOUT_MS 100

/* Take one line and record it. from is the sensor whose channel carried
   the line, or -1 for the priority FIFO. */
static void accept_line(const char *line, size_t len, int from,
                        const traffic_t *expected,
                        sched_t *sched, centre_stats_t *st, log_t *log)
{
    st->total++;

    contact_t     c;
    contact_err_t e = contact_parse(line, len, &c);
    if (e != CONTACT_OK) {
        st->rejected++;
        if (log != NULL) {
            log_writef(log, LOG_INFO, "rejected report: %s",
                       contact_strerror(e));
        }
        return;
    }

    /* THE CHANNEL NAMES THE SENDER. A line on the NWS channel came from the
       NWS process, so a line on it that says "AIS" is false. If the centre
       used the text, it would count the line as AIS, and round robin would
       serve it in the turn of AIS. The FIFO has no sensor of its own, so
       this check does not apply to it. */
    if (from >= 0 && strcmp(c.sensor, sensor_name((sensor_id_t)from)) != 0) {
        st->rejected++;
        st->wrong_channel++;
        if (log != NULL) {
            log_writef(log, LOG_INFO,
                       "rejected report: sensor %s on the %s channel",
                       c.sensor, sensor_name((sensor_id_t)from));
        }
        return;
    }

    st->accepted++;
    if (from < 0) {
        st->from_prio_channel++;
    } else {
        st->per_sensor[from]++;
    }
    if (c.flag == PRIORITY_PRIORITY) {
        st->priority++;
    }

    pending_t p;
    memset(&p, 0, sizeof p);
    p.c                  = c;
    p.arrived_ns         = sc_now_ns();
    p.from_prio_channel  = (from < 0);

    /* The class that the centre gives: HIGH for a contact that is not
       filed and is in a monitored region. The flag of the sensor does not
       change. */
    p.urgency = centre_assess(expected, &c, &p.region);
    if (p.urgency == URGENCY_HIGH) {
        st->high++;
    }

    if (sched_push(sched, &p) != 0) {
        /* The scheduler is full: the centre cannot keep up. Lab 2 has one
           worker, and that is its limit. The log records each lost report. */
        if (log != NULL) {
            log_writef(log, LOG_ALERT,
                       "scheduler full, report %s from %s dropped",
                       c.id, c.sensor);
        }
    }
}

/* Serve each report that the scheduler gives now.
 *
 * service_us is the work that one report costs. Without it, the centre
 * empties the scheduler as fast as reports arrive, no queue forms, and the
 * three policies give the same order. A policy has a choice only when
 * reports wait. */
static void drain(sched_t *sched, centre_stats_t *st, log_t *log,
                  int service_us)
{
    pending_t p;
    while (sched_next(sched, &p) == 1) {
        if (service_us > 0) {
            /* A busy wait, not nanosleep. The cost must be work on the
               processor. A sleep lets the kernel run other processes, and
               the queue then looks shorter than it is. */
            int64_t until = sc_now_ns() + (int64_t)service_us * 1000;
            while (sc_now_ns() < until) {
                /* spin */
            }
        }

        int64_t waited = sc_now_ns() - p.arrived_ns;

        st->lat_sum_ns += waited;
        st->lat_count++;
        if (waited > st->lat_max_ns) {
            st->lat_max_ns = waited;
        }

        urgency_t u = pending_urgency(&p);
        if (u >= URGENCY_SENSOR) {
            st->prio_lat_sum_ns += waited;
            st->prio_lat_count++;
            if (waited > st->prio_lat_max_ns) {
                st->prio_lat_max_ns = waited;
            }
            if (log != NULL && u == URGENCY_HIGH) {
                log_writef(log, LOG_PRIORITY,
                           "priority contact %s from %s served after %.3f ms, "
                           "HIGH: unknown in %s",
                           p.c.id, p.c.sensor, (double)waited / 1e6, p.region);
            } else if (log != NULL) {
                log_writef(log, LOG_PRIORITY,
                           "priority contact %s from %s served after %.3f ms",
                           p.c.id, p.c.sensor, (double)waited / 1e6);
            }
        } else if (log != NULL) {
            log_writef(log, LOG_TRACK, "contact %s from %s",
                       p.c.id, p.c.sensor);
        }
    }
}

/* 1 when a sensor has no channel, but its process is not collected yet. */
static int ended_not_collected(const sensor_net_t *net)
{
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (net->sensors[i].fd < 0 && net->sensors[i].pid != 0) {
            return 1;
        }
    }
    return 0;
}

int centre_run(sensor_net_t *net, prio_channel_t *prio, sched_t *sched,
               const traffic_t *expected, int64_t run_ms, int service_us,
               centre_stats_t *stats)
{
    if (net == NULL || sched == NULL || stats == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(stats, 0, sizeof *stats);
    log_t *log = net->log;

    /* THE FIFO MUST BE NON-BLOCKING. lr_next_line calls read(), and on a
     * blocking descriptor read() waits until data arrives. The loop below
     * reads each line that is available. With a blocking FIFO, that loop
     * waits in read() when the FIFO is empty, and the centre stops: it does
     * not read the sensors, and it does not keep the run limit. So
     * prio_open must open the read end with O_NONBLOCK. The given
     * sensor_channel_open does the same for each sensor channel. */
    linereader_t *lr_prio = NULL;
    if (prio != NULL && prio->fd >= 0) {
        lr_prio = lr_open(prio->fd, 4096, 0);
        if (lr_prio == NULL) {
            return -1;
        }
    }

    /* The descriptors for poll(): the four sensor channels, then the FIFO. */
    struct pollfd fds[SENSOR_COUNT + 1];
    const int     FIFO = SENSOR_COUNT;

    int64_t started_ns = sc_now_ns();

    while (sensor_net_open_count(net) > 0) {
        if (run_ms > 0 && (sc_now_ns() - started_ns) / 1000000 >= run_ms) {
            break;
        }

        sensor_net_pollfds(net, fds);
        fds[FIFO].fd      = (lr_prio != NULL) ? prio->fd : -1;
        fds[FIFO].events  = POLLIN;
        fds[FIFO].revents = 0;

        /* A channel that stopped at the limit can hold lines in its reader,
           and poll() cannot see them there. Then do not wait. */
        int timeout = sensor_net_more(net) ? 0 : POLL_TIMEOUT_MS;
        int ready = poll(fds, SENSOR_COUNT + 1, timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                /* A signal interrupted poll(). This is not an error: start
                   the pass again. */
                continue;
            }
            lr_close(lr_prio);
            return -1;
        }

        /* --- the sensor channels -------------------------------------- */
        /* At most SENSOR_LINES_PER_PASS lines from each channel in one
           pass, so that a fast sensor cannot make the others wait.
           sensor_net_next_line closes a channel at its end. */
        sensor_id_t from;
        const char *line;
        size_t      len;
        int         rc;
        while ((rc = sensor_net_next_line(net, fds, &from, &line, &len)) != 0) {
            if (rc < 0) {
                /* EMSGSIZE: one line was longer than the buffer of the
                   reader, and it is lost. The channel is still good, so
                   count the line and read on. One bad line must not stop
                   the reports of a sensor. */
                stats->total++;
                stats->rejected++;
                if (log != NULL) {
                    log_writef(log, LOG_INFO,
                               "rejected report: longer than %d bytes, on the %s channel",
                               4096, sensor_name(from));
                }
                continue;
            }
            accept_line(line, len, (int)from, expected, sched, stats, log);
        }

        /* --- the priority channel ------------------------------------- */
        if (fds[FIFO].fd >= 0 && (fds[FIFO].revents & POLLIN)) {
            while ((rc = lr_next_line(lr_prio, &line, &len)) == 1 ||
                   (rc < 0 && errno == EMSGSIZE)) {
                if (rc == 1) {
                    accept_line(line, len, -1, expected, sched, stats, log);
                } else {
                    stats->total++;
                    stats->rejected++;
                }
            }
            /* The FIFO does not end: prio_open holds a write end open. So
               the loop stops on EAGAIN, and never on end of file. */
        }

        /* Serve the reports that the policy chooses. Do it in each pass,
           also after a timeout, so that a report never waits for the next
           report to arrive. */
        drain(sched, stats, log, service_us);

        /* Collect each sensor that stopped. Do it in each pass, so that the
           mission log records a lost sensor while the centre still runs,
           and the stopped process does not stay a zombie. */
        sensor_reap(net);
    }

    /* Serve each report that still waits when the sensors stop. If not, the
       totals do not agree with what the sensors sent. */
    drain(sched, stats, log, service_us);

    /* A sensor whose channel ended has closed its write end: it is in its
       exit. The kernel can need a moment more before waitpid() can collect
       it. Give each such sensor up to 200 ms, so that the log says how it
       stopped, and sensor_net_shutdown does not log it as "stopped". */
    for (int tries = 0; tries < 20 && ended_not_collected(net); tries++) {
        struct timespec t = { 0, 10 * 1000000L };
        nanosleep(&t, NULL);
        sensor_reap(net);
    }
    sensor_reap(net);

    lr_close(lr_prio);
    return 0;
}

int centre_stats_write(int fd, const centre_stats_t *st,
                       sched_policy_t policy, int64_t elapsed_ns)
{
    if (st == NULL) {
        errno = EINVAL;
        return -1;
    }

    char buf[1024];
    int  n = 0;

    n += snprintf(buf + n, sizeof buf - (size_t)n,
                  "operations centre\n"
                  "  policy          %s\n"
                  "  elapsed         %.3f ms\n"
                  "  reports read    %ld\n"
                  "  accepted        %ld\n"
                  "  rejected        %ld\n"
                  "  wrong channel   %ld (in rejected)\n"
                  "  sensor P        %ld\n"
                  "  centre HIGH     %ld\n"
                  "  via FIFO        %ld\n",
                  sched_policy_name(policy),
                  (double)elapsed_ns / 1e6,
                  st->total, st->accepted, st->rejected, st->wrong_channel,
                  st->priority, st->high, st->from_prio_channel);

    for (int i = 0; i < SENSOR_COUNT; i++) {
        n += snprintf(buf + n, sizeof buf - (size_t)n,
                      "    %-4s          %ld\n",
                      sensor_name((sensor_id_t)i), st->per_sensor[i]);
    }

    if (st->lat_count > 0) {
        n += snprintf(buf + n, sizeof buf - (size_t)n,
                      "  latency mean    %.3f ms\n"
                      "  latency worst   %.3f ms\n",
                      (double)st->lat_sum_ns / (double)st->lat_count / 1e6,
                      (double)st->lat_max_ns / 1e6);
    }
    if (st->prio_lat_count > 0) {
        n += snprintf(buf + n, sizeof buf - (size_t)n,
                      "  urgent mean     %.3f ms   (HIGH and P)\n"
                      "  urgent worst    %.3f ms\n",
                      (double)st->prio_lat_sum_ns /
                          (double)st->prio_lat_count / 1e6,
                      (double)st->prio_lat_max_ns / 1e6);
    }

    if (n < 0 || (size_t)n >= sizeof buf) {
        errno = EOVERFLOW;
        return -1;
    }
    return sc_write_all(fd, buf, (size_t)n);
}
