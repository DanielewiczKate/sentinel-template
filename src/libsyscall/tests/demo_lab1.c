#include "sentinel/syscall.h"
#include "contact.h"
#include "linereader.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void usage(void)
{
    fprintf(stderr, "usage: demo_lab1 <feed-file> [mission-log] [--follow IDLE_MS]\n");
}

static int parse_idle_ms(const char *s, long *out)
{
    if (NULL == s || '\0' == *s)
        return -1;

    char *end;
    long v = strtol(s, &end, 10);
    if ('\0' != *end || v < 1 || v > 2147483647L)
        return -1;

    *out = v;
    return 0;
}

// don't report a lost log line right away, just count it and the reason
static long g_lost = 0;
static int g_lost_errno = 0;

static void log_line(log_t *lg, log_level_t level, const char *fmt, ...)
{
    char msg[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    int res = log_writef(lg, level, "%s", msg);
    int err = errno;

    if (-1 == res) {
        g_lost++;
        g_lost_errno = err;
    }
}

int main(int argc, char **argv)
{
    const char *feed = NULL;
    const char *logpath = "/tmp/sentinel_mission.log";
    int follow = 0;
    long idle_ms = 0;
    int nfiles = 0;

    for (int i = 1; i < argc; i++) {
        if (0 == strcmp(argv[i], "--follow")) {
            if (i + 1 >= argc || parse_idle_ms(argv[i + 1], &idle_ms) != 0) {
                usage();
                return 2;
            }
            follow = 1;
            i++;
            continue;
        }
        if (0 == nfiles) {
            feed = argv[i];
        } else if (1 == nfiles) {
            logpath = argv[i];
        } else {
            usage();
            return 2;
        }
        nfiles++;
    }
    if (NULL == feed) {
        usage();
        return 2;
    }

    log_t *lg = log_open(logpath);
    if (NULL == lg) {
        fprintf(stderr, "cannot open mission log %s: %s\n", logpath, strerror(errno));
        return 1;
    }

    log_line(lg, LOG_SYSTEM, "SENTINEL Part 1 starting, feed=%s", feed);

    proc_info_t before;
    if (proc_report(0, &before) != 0) {
        fprintf(stderr, "cannot read process report: %s\n", strerror(errno));
        log_close(lg);
        return 1;
    }
    log_line(lg, LOG_SYSTEM, "pid=%d ppid=%d name=%s open_fds=%d",
              (int)before.pid, (int)before.ppid, before.name, before.open_fds);

    int ffd = sc_open_read(feed);
    if (ffd < 0) {
        fprintf(stderr, "cannot open feed %s: %s\n", feed, strerror(errno));
        log_close(lg);
        return 1;
    }

    linereader_t *lr = follow ? lr_open_follow(ffd, 0, 1) : lr_open(ffd, 0, 1);
    if (NULL == lr) {
        fprintf(stderr, "cannot start reading feed: %s\n", strerror(errno));
        sc_close(ffd);
        log_close(lg);
        return 1;
    }

    long nread = 0, accepted = 0, rejected = 0, priority = 0;
    int read_error = 0;

    int64_t t_start = sc_now_ns();
    int64_t idle_since = t_start;

    while(1) {
        const char *line;
        size_t len;
        int res = lr_next_line(lr, &line, &len);
        int err = errno;

        if (1 == res) {
            nread++;
            idle_since = sc_now_ns();
            contact_t c;
            contact_err_t cerr = contact_parse(line, len, &c);
            if (CONTACT_OK == cerr) {
                accepted++;
                if (PRIORITY_PRIORITY == c.flag) {
                    priority++;
                    log_line(lg, LOG_PRIORITY, "priority contact %s from %s at %.4f,%.4f speed %.1f",
                              c.id, c.sensor, c.lat, c.lon, c.speed);
                } else {
                    log_line(lg, LOG_INFO, "contact %s from %s", c.id, c.sensor);
                }
            } else {
                rejected++;
                log_line(lg, LOG_INFO, "rejected line %ld: %s", nread, contact_strerror(cerr));
            }
            continue;
        }

        if (-1 == res && EMSGSIZE == err) {
            nread++;
            rejected++;
            idle_since = sc_now_ns();
            log_line(lg, LOG_INFO, "rejected line %ld: %s", nread, contact_strerror(CONTACT_ERR_TOO_LONG));
            continue;
        }

        if (-1 == res && EAGAIN == err && follow) {
            if (sc_now_ns() - idle_since >= idle_ms * 1000000LL)
                break;
            struct timespec wait = { 0, 50 * 1000 * 1000 };
            nanosleep(&wait, NULL);
            continue;
        }

        if (0 == res)
            break;

        log_line(lg, LOG_SYSTEM, "feed read failed: %s", strerror(err));
        read_error = 1;
        break;
    }

    int64_t t_end = sc_now_ns();
    lr_close(lr);

    proc_info_t after;
    int after_failed = (proc_report(0, &after) != 0);
    if (after_failed)
        fprintf(stderr, "cannot read process report: %s\n", strerror(errno));

    printf("lines read %ld\n", nread);
    printf("accepted   %ld\n", accepted);
    printf("rejected   %ld\n", rejected);
    printf("priority   %ld\n", priority);

    int report_failed = 0;
    if (!after_failed) {
        fflush(stdout);
        if (proc_report_write(STDOUT_FILENO, &after) != 0) {
            fprintf(stderr, "cannot write process report: %s\n", strerror(errno));
            report_failed = 1;
        } else {
            printf("open_fds before=%d after=%d %s\n", before.open_fds, after.open_fds,
                   before.open_fds == after.open_fds ? "(balanced)" : "(LEAKED)");
        }
    }

    double ms = (double)(t_end - t_start) / 1e6;
    log_line(lg, LOG_SYSTEM, "Part 1 complete: %ld read, %ld accepted, %ld rejected, %ld priority, %.3f ms",
              nread, accepted, rejected, priority, ms);

    int close_failed = (log_close(lg) != 0);
    if (close_failed)
        fprintf(stderr, "cannot close mission log: %s\n", strerror(errno));

    if (g_lost > 0)
        fprintf(stderr, "%ld mission log line(s) lost: %s\n", g_lost, strerror(g_lost_errno));

    if (read_error || after_failed || report_failed || close_failed || g_lost > 0)
        return 1;
    if (nread > 0 && 0 == accepted)
        return 1;
    return 0;
}
