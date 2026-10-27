/* sensor.c — one process for each sensor, each with a pipe of its own.
 *
 * YOU WRITE THIS FILE. The comment above each function gives its steps, and
 * proc.h gives the same function with each error and each log line. Each
 * function is a stub now: it fails, so that the library builds before you
 * write it. Replace the whole body of each stub, its (void) lines too.
 * Lecture 4.2 (Process Management) shows fork, exec and wait, and lecture
 * 4.4 (Programming IPC) shows pipe. Read also man 2 pipe2, man 2 dup2,
 * man 2 waitpid and man 2 kill.
 *
 * sensor_spawn puts four system calls together, in this order:
 *
 *   pipe2()  make a new pipe for the sensor, before fork(), so that the
 *            child gets a copy of its write end
 *   fork()   make the child process
 *   dup2()   in the child, put the write end on standard output
 *   execvp() in the child, replace the program with contactfeed
 *
 * The order is necessary. fork() copies only the descriptors that exist at
 * that time. If fork() comes before pipe2(), the child and the centre each
 * make a different pipe, and nothing connects them.
 */

#include "sentinel/proc.h"
#include "cpu.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>

/* sensor_spawn: start one sensor as a child process, with a new pipe.
 *
 *   1. If net is NULL, or id is below 0 or not below SENSOR_COUNT, set errno
 *      to EINVAL and return -1.
 *   2. sensor_t *s = &net->sensors[id]. If s->pid is not 0, the sensor
 *      runs: set errno to EEXIST and return -1. Then, if s->fd >= 0, its
 *      old channel is still open: set errno to EBUSY and return -1.
 *   3. Write rate as text: char rate_buf[32];
 *      snprintf(rate_buf, sizeof rate_buf, "%d", rate).
 *   4. int fds[2]; pipe2(fds, O_CLOEXEC). If it fails, return -1: it has
 *      set errno. fds[0] is the read end, and fds[1] is the write end.
 *   5. sensor_channel_open(s, fds[0]) (GIVEN). If it fails: save errno,
 *      close both ends, put errno back, and return -1.
 *   6. fflush(stdout) and fflush(stderr). The child gets a copy of the
 *      stdio buffer: text that is still in it prints two times if you do
 *      not flush. No check: a failed flush loses only text of the centre.
 *   7. pid_t pid = fork(). If it fails: save errno,
 *      sensor_channel_close(s) (this closes fds[0]), close(fds[1]), put
 *      errno back, and return -1.
 *   8. In the child (pid == 0). It never returns. It ends with _exit(), not
 *      exit(): exit() flushes the copy of the stdio buffer again.
 *        a. If fds[1] is not STDOUT_FILENO: dup2(fds[1], STDOUT_FILENO).
 *           If fds[1] IS STDOUT_FILENO, dup2 does nothing and does not
 *           clear O_CLOEXEC, so call fcntl(STDOUT_FILENO, F_SETFD, 0)
 *           in its place. If the call fails: _exit(126).
 *        b. signal(SIGPIPE, SIG_IGN). Then a write to a pipe that has no
 *           reader gives EPIPE, and contactfeed stops normally. No check:
 *           write a comment that says why (it cannot fail for a valid
 *           signal).
 *        c. cpu_unpin() (common/cpu.h). The child gets the one processor
 *           of the centre; this gives it each processor. No check.
 *        d. Make the argument list, with a NULL at its end:
 *             char *argv[] = { (char *)net->feed_path, "--sensor",
 *                              (char *)sensor_name(id),
 *                              burst ? "--burst" : "--rate",
 *                              rate_buf, NULL };
 *        e. execvp(net->feed_path, argv). It returns only when it fails:
 *           then _exit(127).
 *   9. In the centre: close(fds[1]). The child now holds the only write
 *      end, so the channel ends when the child stops. No check: write a
 *      comment that says why (a close of a valid pipe end does not fail). Then set s->pid,
 *      s->rate, s->burst and s->started_ns (sc_now_ns()), and add 1 to
 *      net->n_sensors.
 *  10. If net->log is not NULL, write the line of proc.h:
 *        log_writef(net->log, LOG_SYSTEM, "sensor %s started pid=%d %s=%d fd=%d",
 *                   sensor_name(id), (int)pid, burst ? "burst" : "rate",
 *                   rate, s->fd);
 *      No check: write a comment that says why (a lost log line is not an
 *      error of the sensor, the rule of Lab 1).
 *  11. Return 0.
 */
int sensor_spawn(sensor_net_t *net, sensor_id_t id, int rate, int burst)
{
    (void)net;
    (void)id;
    (void)rate;
    (void)burst;
    errno = ENOSYS;
    return -1;
}

/* sensor_stop: stop one sensor and collect it.
 *
 *   1. If net is NULL, or id is out of range (as in sensor_spawn), set errno
 *      to EINVAL and return -1.
 *   2. sensor_t *s = &net->sensors[id]. If s->pid is 0, return 0: it does
 *      not run.
 *   3. kill(s->pid, SIGTERM). No check: write a comment that says why (the
 *      pid is a child that this process has not collected, so it exists).
 *   4. Wait for it, and wait again after a signal:
 *        while (waitpid(pid, NULL, 0) < 0) {
 *            if (errno != EINTR) {
 *                break;
 *            }
 *        }
 *   5. Set s->pid to 0, and take 1 from net->n_sensors. Do not close the
 *      channel: the centre reads the lines that are still in it.
 *   6. If net->log is not NULL, write the line of proc.h:
 *        log_writef(net->log, LOG_SYSTEM, "sensor %s (pid %d) stopped",
 *                   sensor_name(id), (int)pid);
 *      Keep the pid in a local variable before step 5.
 *   7. Return 0.
 */
int sensor_stop(sensor_net_t *net, sensor_id_t id)
{
    (void)net;
    (void)id;
    errno = ENOSYS;
    return -1;
}

/* sensor_reap: collect each sensor that has stopped, and do not wait.
 *
 *   1. If net is NULL, set errno to EINVAL and return -1.
 *   2. int collected = 0. For each i from 0 to SENSOR_COUNT - 1, with
 *      sensor_t *s = &net->sensors[i]:
 *        a. If s->pid is 0, go to the next sensor.
 *        b. Ask about this child only, and do not wait:
 *             int status = 0;
 *             pid_t got;
 *             do {
 *                 got = waitpid(s->pid, &status, WNOHANG);
 *             } while (got < 0 && errno == EINTR);
 *           Do not use waitpid(-1, ...): proc.h says why.
 *        c. If got <= 0, go to the next sensor: it still runs (0), or the
 *           call failed (-1).
 *        d. If net->log is not NULL, write ONE line of proc.h. NAME is
 *           sensor_name(s->id), and PID is got:
 *             WIFSIGNALED(status):        LOG_ALERT
 *               "sensor %s (pid %d) died on signal %d (%s)", with
 *               WTERMSIG(status) and strsignal(WTERMSIG(status))
 *             WEXITSTATUS(status) == 0:   LOG_SYSTEM
 *               "sensor %s (pid %d) finished with status 0"
 *             else:                       LOG_ALERT
 *               "sensor %s (pid %d) exited with code %d", with
 *               WEXITSTATUS(status)
 *        e. Set s->pid to 0, take 1 from net->n_sensors, and add 1 to
 *           collected.
 *   3. Return collected.
 */
int sensor_reap(sensor_net_t *net)
{
    (void)net;
    errno = ENOSYS;
    return -1;
}
