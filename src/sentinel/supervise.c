#include "sentinel/supervise.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

static volatile sig_atomic_t g_sigchld_pending = 0;

static void sigchld_handler(int sig)
{
    (void)sig;

    g_sigchld_pending = 1;
}

int supervise_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);

    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);

    sa.sa_flags = SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        return -1;
    }
    return 0;
}

int supervise_pending(void)
{
    return g_sigchld_pending != 0;
}

static int64_t restart_wait_ns(long failures)
{
    int64_t wait = 1000000000LL;
    for (long i = 1; i < failures && i < 5; i++) {
        wait *= 2;
    }
    return wait;
}

static void respawn_due(sensor_net_t *net)
{
    int64_t now = sc_now_ns();
    for (int i = 0; i < SENSOR_COUNT; i++) {
        sensor_t *s = &net->sensors[i];
        if (s->pid != 0 || s->fd >= 0 || s->restart_at_ns == 0 ||
            now < s->restart_at_ns) {
            continue;
        }
        long        prev = s->restarts;
        int         rate = s->rate, burst = s->burst;
        const char *name = sensor_name((sensor_id_t)i);

        s->restart_at_ns = 0;
        if (sensor_spawn(net, (sensor_id_t)i, rate, burst) == 0) {
            net->sensors[i].restarts = prev + 1;
            if (net->log != NULL) {
                log_writef(net->log, LOG_SYSTEM, "sensor %s restarted (restart %ld)",
                           name, prev + 1);
            }
        } else {
            s->restarts      = prev + 1;
            s->restart_at_ns = now + restart_wait_ns(prev + 1);
            if (net->log != NULL) {
                log_writef(net->log, LOG_ALERT, "sensor %s could not be restarted: %s",
                           name, strerror(errno));
            }
        }
    }
}

int supervise_reap(sensor_net_t *net, int restart)
{
    if (net == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (restart) {
        respawn_due(net);
    }

    g_sigchld_pending = 0;

    int collected = 0;

    for (;;) {
        int   status = 0;

        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid == 0) {
            break;
        }
        if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        sensor_t *s = sensor_by_pid(net, pid);
        if (s == NULL) {
            collected++;
            continue;
        }

        sensor_id_t id   = s->id;
        const char *name = sensor_name(id);

        if (net->log != NULL) {
            if (WIFSIGNALED(status)) {
                log_writef(net->log, LOG_ALERT,
                           "sensor %s (pid %d) died on signal %d (%s)",
                           name, (int)pid, WTERMSIG(status),
                           strsignal(WTERMSIG(status)));
            } else if (WEXITSTATUS(status) == 0) {
                log_writef(net->log, LOG_SYSTEM,
                           "sensor %s (pid %d) finished with status 0",
                           name, (int)pid);
            } else {
                log_writef(net->log, LOG_ALERT,
                           "sensor %s (pid %d) exited with code %d",
                           name, (int)pid, WEXITSTATUS(status));
            }
        }

        s->pid = 0;
        net->n_sensors--;
        collected++;

        if (restart) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                s->restart_at_ns = 0;
                if (net->log != NULL) {
                    log_writef(net->log, LOG_SYSTEM,
                               "sensor %s finished; it is not restarted", name);
                }
            } else {
                int64_t now = sc_now_ns();
                if (now - s->started_ns > 60000000000LL) {
                    s->restarts = 0;
                }
                s->restart_at_ns = now + restart_wait_ns(s->restarts + 1);
                if (net->log != NULL && s->restarts > 0) {
                    log_writef(net->log, LOG_SYSTEM,
                               "sensor %s starts again in %lld ms (failure %ld)",
                               name, (long long)(restart_wait_ns(s->restarts + 1) / 1000000),
                               s->restarts + 1);
                }
            }
        }
    }

    return collected;
}
