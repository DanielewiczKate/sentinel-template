/* accept_lab2 — the acceptance test of Lab 2.
 *
 *   accept_lab2 [--demo PATH] [--contactfeed PATH]
 *
 * `make accept2` compiles this file with your build/libproc.a, your
 * build/libsyscall.a and build/libcommon.a. So each call to a sensor_*,
 * prio_* or sched_* function below runs your code, or the given code of
 * src/libproc/given/. Then the test runs build/demo_lab2 as a program. It
 * prints PASS or FAIL for each of 26 checks, then the share that passed for
 * each file.
 *
 * Read a check to see what it does. Do not change this file. The marks come
 * from the course copy of it, and Section 7 of docs/lab2/lab2_brief.pdf
 * gives them.
 *
 * How the test works:
 *
 *   - Each check runs in its own child process, in its own process group,
 *     with a time limit. A crash, or a call that does not return, stops
 *     that check only. At the end of a check, the test stops each process
 *     that the check started.
 *
 *   - The test reads /proc to see what your code did: the arguments and the
 *     descriptors of a sensor process, and its processors.
 *
 *   - This file defines write(). Your library is linked to this definition,
 *     so a check can count the write() calls of prio_send. In each other
 *     case it only passes the call to the kernel.
 *
 *   - The test uses no assert(). A failed check prints a sentence about the
 *     contract, not a line number in this file.
 */

#undef _FORTIFY_SOURCE      /* its inline write() would clash with the one below */

#include "sentinel/proc.h"
#include "contact.h"
#include "cpu.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#define TIMEOUT_S 15
#define FIFO_DEMO "/tmp/sentinel_priority.fifo"

static char        g_dir[64];          /* the temporary directory of this run */
static const char *g_demo = "build/demo_lab2";
static char        g_feedtool[256];
static int         g_msgfd = -1;       /* in a check child: where fail() writes */

/* ---- reporting --------------------------------------------------------- */

/* End the check with a message for the student. Only a check child calls it. */
__attribute__((format(printf, 1, 2), noreturn))
static void fail(const char *fmt, ...)
{
    char    buf[768];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) {
        n = 0;
    }
    if ((size_t)n >= sizeof buf) {
        n = (int)sizeof buf - 1;
    }
    if (g_msgfd >= 0 && syscall(SYS_write, g_msgfd, buf, (size_t)n) < 0) {
        /* nothing more can be reported */
    }
    fflush(NULL);
    _exit(1);
}

#define CHECK(cond, ...) do { if (!(cond)) fail(__VA_ARGS__); } while (0)

/* ---- write(), counted -------------------------------------------------- */

static volatile int g_count_writes;   /* 1 while a check counts the calls */
static volatile int g_writes;

ssize_t write(int fd, const void *buf, size_t n)
{
    if (g_count_writes) {
        g_writes++;
    }
    return syscall(SYS_write, fd, buf, n);
}

/* ---- small helpers, plain libc only ------------------------------------ */

static void path_of(char *buf, size_t n, const char *name)
{
    snprintf(buf, n, "%s/%s", g_dir, name);
}

static void put_file(const char *path, const char *data)
{
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0, "the test cannot create %s: %s", path, strerror(errno));
    size_t len = strlen(data), done = 0;
    while (done < len) {
        ssize_t w = write(fd, data + done, len - done);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        CHECK(w > 0, "the test cannot write %s: %s", path, strerror(errno));
        done += (size_t)w;
    }
    close(fd);
}

/* The whole file, with a '\0' after it. NULL if it cannot be opened. */
static char *get_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    size_t cap = 4096, used = 0;
    char  *buf = malloc(cap);
    CHECK(buf != NULL, "the test is out of memory");
    for (;;) {
        if (used + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            CHECK(buf != NULL, "the test is out of memory");
        }
        ssize_t r = read(fd, buf + used, cap - used - 1);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            break;
        }
        used += (size_t)r;
    }
    close(fd);
    buf[used] = '\0';
    return buf;
}

/* The number of times that needle is in hay. */
static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle)) {
        n++;
    }
    return n;
}

/* 1 if one line of the file has each of the texts a and b. */
static int file_has_line(const char *path, const char *a, const char *b)
{
    char *text = get_file(path);
    if (text == NULL) {
        return 0;
    }
    int found = 0;
    for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        if (strstr(line, a) != NULL && (b == NULL || strstr(line, b) != NULL)) {
            found = 1;
            break;
        }
    }
    free(text);
    return found;
}

static void sleep_ms(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {
    }
}

/* The target of /proc/<pid>/fd/<fd>, or "" when there is no such fd. */
static void fd_target(pid_t pid, int fd, char *buf, size_t n)
{
    char link[96];
    snprintf(link, sizeof link, "/proc/%d/fd/%d", (int)pid, fd);
    ssize_t r = readlink(link, buf, n - 1);
    buf[r > 0 ? (size_t)r : 0] = '\0';
}

/* Wait until pid runs contactfeed: /proc/<pid>/comm changes at exec. */
static void wait_for_exec(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
    for (int i = 0; i < 300; i++) {
        char *comm = get_file(path);
        int   ok   = comm != NULL && strncmp(comm, "contactfeed", 11) == 0;
        free(comm);
        if (ok) {
            return;
        }
        sleep_ms(10);
    }
    fail("the sensor process %d did not run contactfeed within 3 s. "
         "Check the arguments of execvp", (int)pid);
}

/* The arguments of pid, joined with one space. */
static void cmdline_of(pid_t pid, char *buf, size_t n)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0, "the test cannot read %s", path);
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    CHECK(r > 0, "the test cannot read %s", path);
    for (ssize_t i = 0; i < r; i++) {
        if (buf[i] == '\0') {
            buf[i] = ' ';
        }
    }
    while (r > 0 && buf[r - 1] == ' ') {
        r--;
    }
    buf[r] = '\0';
}

/* The value of a key in /proc/<pid>/status, such as "Cpus_allowed_list". */
static void status_value(pid_t pid, const char *key, char *buf, size_t n)
{
    char path[64];
    if (pid == 0) {
        snprintf(path, sizeof path, "/proc/self/status");
    } else {
        snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    }
    char *text = get_file(path);
    CHECK(text != NULL, "the test cannot read %s", path);
    buf[0] = '\0';
    size_t klen = strlen(key);
    for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            const char *v = line + klen + 1;
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            snprintf(buf, n, "%s", v);
            break;
        }
    }
    free(text);
}

/* A network with a mission log in the directory of this run. */
static log_t *net_up(sensor_net_t *net, char *logp, size_t n, const char *feed)
{
    path_of(logp, n, "mission.log");
    unlink(logp);
    log_t *lg = log_open(logp);
    CHECK(lg != NULL, "log_open(%s) failed: %s", logp, strerror(errno));
    CHECK(sensor_net_init(net, feed, lg) == 0, "sensor_net_init failed");
    return lg;
}

/* Read the channels until each one is closed, or for at most ms. Counts the
   lines of each sensor, and the lines whose text names another sensor. */
static void read_channels(sensor_net_t *net, long per[SENSOR_COUNT], long *wrong, int ms)
{
    int64_t end = sc_now_ns() + (int64_t)ms * 1000000;
    while (sensor_net_open_count(net) > 0 && sc_now_ns() < end) {
        struct pollfd fds[SENSOR_COUNT];
        sensor_net_pollfds(net, fds);
        poll(fds, SENSOR_COUNT, sensor_net_more(net) ? 0 : 50);
        sensor_id_t from;
        const char *line;
        size_t      len;
        while (sensor_net_next_line(net, fds, &from, &line, &len) == 1) {
            per[from]++;
            char want[8];
            snprintf(want, sizeof want, " %s ", sensor_name(from));
            if (wrong != NULL && memmem(line, len, want, strlen(want)) == NULL) {
                (*wrong)++;
            }
        }
    }
}

/* Call sensor_reap until it collects one sensor, or for at most ms. */
static int reap_one(sensor_net_t *net, int ms)
{
    for (int waited = 0; waited < ms; waited += 10) {
        int n = sensor_reap(net);
        CHECK(n >= 0, "sensor_reap returned %d with errno %s", n, strerror(errno));
        if (n > 0) {
            return n;
        }
        sleep_ms(10);
    }
    return 0;
}

static pending_t pend(const char *sensor, const char *id, int p_flag, urgency_t u)
{
    pending_t p;
    memset(&p, 0, sizeof p);
    snprintf(p.c.sensor, sizeof p.c.sensor, "%s", sensor);
    snprintf(p.c.id, sizeof p.c.id, "%s", id);
    p.c.flag  = p_flag ? PRIORITY_PRIORITY : PRIORITY_ROUTINE;
    p.urgency = u;
    return p;
}

/* Take each report out of s, and join their ids with ' '. */
static void drain_ids(sched_t *s, char *buf, size_t n)
{
    buf[0] = '\0';
    pending_t out;
    int       rc;
    while ((rc = sched_next(s, &out)) == 1) {
        size_t used = strlen(buf);
        snprintf(buf + used, n - used, "%s%s", used ? " " : "", out.c.id);
    }
    CHECK(rc == 0, "sched_next returned %d, not 0, when no report waits", rc);
}

/* ================================================================ sensor.c */

static void spawn_runs_contactfeed(void)
{
    sensor_net_t net;
    char         logp[128];
    log_t       *lg = net_up(&net, logp, sizeof logp, g_feedtool);

    int rc = sensor_spawn(&net, SENSOR_NWS, 20, 1);
    CHECK(rc == 0, "sensor_spawn(NWS, 20, burst) returned %d: %s", rc, strerror(errno));
    CHECK(net.sensors[SENSOR_NWS].pid > 0, "after sensor_spawn, the pid of NWS is %d",
          (int)net.sensors[SENSOR_NWS].pid);
    CHECK(net.n_sensors == 1, "after one sensor_spawn, n_sensors is %d, not 1", net.n_sensors);

    long per[SENSOR_COUNT] = {0}, wrong = 0;
    read_channels(&net, per, &wrong, 5000);
    CHECK(per[SENSOR_NWS] == 20, "the NWS channel gave %ld lines, not 20. The child must "
          "put the write end on standard output (dup2) and run contactfeed", per[SENSOR_NWS]);
    CHECK(wrong == 0, "%ld lines on the NWS channel do not name NWS", wrong);
    sensor_net_shutdown(&net);
    log_close(lg);
}

static void spawn_exact_argv(void)
{
    sensor_net_t net;
    char         logp[128];
    log_t       *lg = net_up(&net, logp, sizeof logp, g_feedtool);

    CHECK(sensor_spawn(&net, SENSOR_AIS, 7, 0) == 0, "sensor_spawn(AIS, 7, 0) failed: %s",
          strerror(errno));
    /* A big burst fills the pipe and then waits, so the process stays. */
    CHECK(sensor_spawn(&net, SENSOR_SAT, 200000, 1) == 0, "sensor_spawn(SAT, 200000, 1) "
          "failed: %s", strerror(errno));

    char want[512], got[512];
    wait_for_exec(net.sensors[SENSOR_AIS].pid);
    cmdline_of(net.sensors[SENSOR_AIS].pid, got, sizeof got);
    snprintf(want, sizeof want, "%s --sensor AIS --rate 7", g_feedtool);
    CHECK(strcmp(got, want) == 0, "the AIS process has the arguments \"%s\", not \"%s\"",
          got, want);

    wait_for_exec(net.sensors[SENSOR_SAT].pid);
    cmdline_of(net.sensors[SENSOR_SAT].pid, got, sizeof got);
    snprintf(want, sizeof want, "%s --sensor SAT --burst 200000", g_feedtool);
    CHECK(strcmp(got, want) == 0, "with burst != 0, the SAT process has the arguments "
          "\"%s\", not \"%s\"", got, want);
    sensor_net_shutdown(&net);
    log_close(lg);
}

static void spawn_child_descriptors(void)
{
    sensor_net_t net;
    char         logp[128], logreal[4096];
    log_t       *lg = net_up(&net, logp, sizeof logp, g_feedtool);
    CHECK(realpath(logp, logreal) != NULL, "the test cannot find %s", logp);

    for (int i = 0; i < SENSOR_COUNT; i++) {
        CHECK(sensor_spawn(&net, (sensor_id_t)i, 20, 0) == 0, "sensor_spawn(%s) failed: %s",
              sensor_name((sensor_id_t)i), strerror(errno));
    }

    /* The pipe of each channel, as the centre sees it: "pipe:[inode]". */
    char chan[SENSOR_COUNT][64];
    for (int i = 0; i < SENSOR_COUNT; i++) {
        fd_target(getpid(), net.sensors[i].fd, chan[i], sizeof chan[i]);
        CHECK(strncmp(chan[i], "pipe:", 5) == 0, "the channel of %s, fd %d, is \"%s\", "
              "not a pipe", sensor_name((sensor_id_t)i), net.sensors[i].fd, chan[i]);
    }

    for (int i = 0; i < SENSOR_COUNT; i++) {
        pid_t pid = net.sensors[i].pid;
        const char *name = sensor_name((sensor_id_t)i);
        wait_for_exec(pid);

        char one[64];
        fd_target(pid, 1, one, sizeof one);
        CHECK(strcmp(one, chan[i]) == 0, "standard output of %s is \"%s\", not its own "
              "pipe \"%s\". Use dup2(write end, 1)", name, one, chan[i]);

        char dir[64];
        snprintf(dir, sizeof dir, "/proc/%d/fd", (int)pid);
        DIR *d = opendir(dir);
        CHECK(d != NULL, "the test cannot read %s", dir);
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') {
                continue;
            }
            int  fd = atoi(e->d_name);
            char t[4096];
            fd_target(pid, fd, t, sizeof t);
            CHECK(strcmp(t, logreal) != 0, "%s holds the mission log at fd %d after exec. "
                  "Open it with O_CLOEXEC: add the flag in sc_open_write_mode", name, fd);
            if (fd == 1) {
                continue;
            }
            for (int j = 0; j < SENSOR_COUNT; j++) {
                CHECK(strcmp(t, chan[j]) != 0, "%s holds an end of the pipe of %s at fd %d "
                      "after exec. Make each pipe with pipe2(fds, O_CLOEXEC)",
                      name, sensor_name((sensor_id_t)j), fd);
            }
        }
        closedir(d);
    }
    sensor_net_shutdown(&net);
    log_close(lg);
}

static void spawn_channel_ends(void)
{
    sensor_net_t net;
    char         logp[128];
    log_t       *lg = net_up(&net, logp, sizeof logp, g_feedtool);

    CHECK(sensor_spawn(&net, SENSOR_AUR, 5, 1) == 0, "sensor_spawn(AUR, 5, 1) failed: %s",
          strerror(errno));
    long per[SENSOR_COUNT] = {0};
    read_channels(&net, per, NULL, 5000);
    CHECK(sensor_net_open_count(&net) == 0, "the AUR process sent its 5 lines and exited, "
          "but its channel did not reach end of file in 5 s. The centre must close its "
          "copy of the write end after fork()");
    CHECK(per[SENSOR_AUR] == 5, "the AUR channel gave %ld lines, not 5", per[SENSOR_AUR]);
    sensor_net_shutdown(&net);
    log_close(lg);
}

static void spawn_log_line(void)
{
    sensor_net_t net;
    char         logp[128];
    log_t       *lg = net_up(&net, logp, sizeof logp, g_feedtool);

    CHECK(sensor_spawn(&net, SENSOR_NWS, 20, 0) == 0, "sensor_spawn(NWS) failed: %s",
          strerror(errno));
    CHECK(sensor_spawn(&net, SENSOR_AUR, 3, 1) == 0, "sensor_spawn(AUR, burst) failed: %s",
          strerror(errno));
    char want[128];
    snprintf(want, sizeof want, "sensor NWS started pid=%d rate=20 fd=%d",
             (int)net.sensors[SENSOR_NWS].pid, net.sensors[SENSOR_NWS].fd);
    CHECK(file_has_line(logp, "SYSTEM", want), "the mission log has no SYSTEM line "
          "\"%s\"", want);
    snprintf(want, sizeof want, "sensor AUR started pid=%d burst=3 fd=%d",
             (int)net.sensors[SENSOR_AUR].pid, net.sensors[SENSOR_AUR].fd);
    CHECK(file_has_line(logp, "SYSTEM", want), "the mission log has no SYSTEM line "
          "\"%s\"", want);
    CHECK(net.sensors[SENSOR_NWS].started_ns > 0, "started_ns of NWS is not set");
    sensor_net_shutdown(&net);
    log_close(lg);
}

static void spawn_errors(void)
{
    sensor_net_t net;
    char         logp[128];
    log_t       *lg = net_up(&net, logp, sizeof logp, g_feedtool);

    errno = 0;
    CHECK(sensor_spawn(NULL, SENSOR_NWS, 20, 0) == -1 && errno == EINVAL,
          "sensor_spawn(NULL, ...) must give -1 with EINVAL");
    errno = 0;
    CHECK(sensor_spawn(&net, (sensor_id_t)99, 20, 0) == -1 && errno == EINVAL,
          "sensor_spawn with id 99 must give -1 with EINVAL");

    CHECK(sensor_spawn(&net, SENSOR_NWS, 20, 0) == 0, "sensor_spawn(NWS) failed: %s",
          strerror(errno));
    errno = 0;
    CHECK(sensor_spawn(&net, SENSOR_NWS, 20, 0) == -1 && errno == EEXIST,
          "a second sensor_spawn of a running sensor must give -1 with EEXIST");

    CHECK(sensor_stop(&net, SENSOR_NWS) == 0, "sensor_stop(NWS) failed");
    errno = 0;
    CHECK(sensor_spawn(&net, SENSOR_NWS, 20, 0) == -1 && errno == EBUSY,
          "sensor_spawn while the old channel is open must give -1 with EBUSY");

    long per[SENSOR_COUNT] = {0};
    read_channels(&net, per, NULL, 3000);
    CHECK(sensor_spawn(&net, SENSOR_NWS, 20, 0) == 0, "after the old channel ended, "
          "sensor_spawn(NWS) failed: %s", strerror(errno));
    sensor_net_shutdown(&net);
    log_close(lg);
}

static void spawn_exec_fails(void)
{
    sensor_net_t net;
    char         logp[128];
    log_t       *lg = net_up(&net, logp, sizeof logp, "/nonexistent/contactfeed");

    int rc = sensor_spawn(&net, SENSOR_AIS, 50, 0);
    CHECK(rc == 0, "sensor_spawn returned %d. A failed execvp is not an error of "
          "sensor_spawn: the child exits 127", rc);
    pid_t pid = net.sensors[SENSOR_AIS].pid;
    CHECK(reap_one(&net, 3000) == 1, "the child of a program that does not exist did "
          "not stop, or sensor_reap did not collect it");
    char want[96];
    snprintf(want, sizeof want, "sensor AIS (pid %d) exited with code 127", (int)pid);
    CHECK(file_has_line(logp, "ALERT", want), "the mission log has no ALERT line \"%s\"",
          want);
    sensor_net_shutdown(&net);
    log_close(lg);
}

static void spawn_no_double_output(void)
{
    char outp[128];
    path_of(outp, sizeof outp, "check.out");

    sensor_net_t net;
    char         logp[128];
    log_t       *lg = net_up(&net, logp, sizeof logp, "/nonexistent/contactfeed");

    /* Standard output is a file here, so this line stays in the stdio
       buffer. fork() copies the buffer into the child. After dup2, fd 1 of
       the child is the pipe, so a child that calls exit() after a failed
       exec writes its copy of the line into the channel of the sensor. */
    printf("MARK-OF-THE-CENTRE\n");
    CHECK(sensor_spawn(&net, SENSOR_AIS, 50, 0) == 0, "sensor_spawn failed: %s",
          strerror(errno));
    long per[SENSOR_COUNT] = {0};
    read_channels(&net, per, NULL, 3000);
    reap_one(&net, 3000);
    fflush(stdout);

    CHECK(per[SENSOR_AIS] == 0, "the exec of the AIS process failed, but its channel gave "
          "%ld line(s): the child wrote the text of the centre into the pipe. Flush stdout "
          "before fork(), and use _exit(), not exit(), in the child", per[SENSOR_AIS]);
    char *text = get_file(outp);
    int   n    = text != NULL ? count_of(text, "MARK-OF-THE-CENTRE") : 0;
    free(text);
    CHECK(n == 1, "text that the centre printed before sensor_spawn is %d times in its "
          "output, not 1. Flush stdout before fork(), and use _exit() in the child", n);
    sensor_net_shutdown(&net);
    log_close(lg);
}

static void spawn_gives_processors_back(void)
{
    if (cpu_count_online() < 2) {
        printf("one processor online: nothing to give back\n");
        return;
    }
    char before[128], after[128];
    status_value(0, "Cpus_allowed_list", before, sizeof before);
    CHECK(cpu_pin(1) == 0, "the test cannot restrict itself to one processor");

    sensor_net_t net;
    char         logp[128];
    log_t       *lg = net_up(&net, logp, sizeof logp, g_feedtool);
    CHECK(sensor_spawn(&net, SENSOR_NWS, 20, 0) == 0, "sensor_spawn(NWS) failed: %s",
          strerror(errno));
    wait_for_exec(net.sensors[SENSOR_NWS].pid);
    status_value(net.sensors[SENSOR_NWS].pid, "Cpus_allowed_list", after, sizeof after);
    CHECK(strcmp(before, after) == 0, "the centre runs on one processor, and the sensor "
          "process may use \"%s\", not \"%s\". Call cpu_unpin() in the child", after, before);
    sensor_net_shutdown(&net);
    log_close(lg);
}

static void stop_collects(void)
{
    sensor_net_t net;
    char         logp[128];
    log_t       *lg = net_up(&net, logp, sizeof logp, g_feedtool);

    errno = 0;
    CHECK(sensor_stop(NULL, SENSOR_NWS) == -1 && errno == EINVAL,
          "sensor_stop(NULL, ...) must give -1 with EINVAL");
    errno = 0;
    CHECK(sensor_stop(&net, (sensor_id_t)-1) == -1 && errno == EINVAL,
          "sensor_stop with id -1 must give -1 with EINVAL");
    CHECK(sensor_stop(&net, SENSOR_NWS) == 0, "sensor_stop of a sensor that does not run "
          "must give 0");

    CHECK(sensor_spawn(&net, SENSOR_NWS, 20, 0) == 0, "sensor_spawn(NWS) failed: %s",
          strerror(errno));
    pid_t pid = net.sensors[SENSOR_NWS].pid;
    CHECK(sensor_stop(&net, SENSOR_NWS) == 0, "sensor_stop(NWS) failed");
    CHECK(net.sensors[SENSOR_NWS].pid == 0, "after sensor_stop, the pid of NWS is %d, not 0",
          (int)net.sensors[SENSOR_NWS].pid);
    CHECK(net.n_sensors == 0, "after sensor_stop, n_sensors is %d, not 0", net.n_sensors);
    errno = 0;
    CHECK(waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD, "after sensor_stop, the "
          "process %d is still a child that nobody collected", (int)pid);
    CHECK(net.sensors[SENSOR_NWS].fd >= 0, "sensor_stop closed the channel. It must stay "
          "open, so that the centre can read the lines in it");
    char want[96];
    snprintf(want, sizeof want, "sensor NWS (pid %d) stopped", (int)pid);
    CHECK(file_has_line(logp, "SYSTEM", want), "the mission log has no SYSTEM line \"%s\"",
          want);
    CHECK(sensor_stop(&net, SENSOR_NWS) == 0, "a second sensor_stop must give 0");
    sensor_net_shutdown(&net);
    log_close(lg);
}

static void reap_killed(void)
{
    sensor_net_t net;
    char         logp[128];
    log_t       *lg = net_up(&net, logp, sizeof logp, g_feedtool);

    CHECK(sensor_spawn(&net, SENSOR_NWS, 20, 0) == 0, "sensor_spawn(NWS) failed: %s",
          strerror(errno));
    pid_t pid = net.sensors[SENSOR_NWS].pid;
    wait_for_exec(pid);
    CHECK(kill(pid, SIGKILL) == 0, "the test cannot kill %d", (int)pid);
    CHECK(reap_one(&net, 3000) == 1, "sensor_reap did not collect the killed NWS process "
          "in 3 s");
    CHECK(net.sensors[SENSOR_NWS].pid == 0, "after sensor_reap, the pid of NWS is %d, not 0",
          (int)net.sensors[SENSOR_NWS].pid);
    CHECK(net.n_sensors == 0, "after sensor_reap, n_sensors is %d, not 0", net.n_sensors);
    char want[96];
    snprintf(want, sizeof want, "sensor NWS (pid %d) died on signal 9 (", (int)pid);
    CHECK(file_has_line(logp, "ALERT", want), "the mission log has no ALERT line that "
          "starts \"%s\"", want);
    sensor_net_shutdown(&net);
    log_close(lg);
}

static void reap_finished(void)
{
    sensor_net_t net;
    char         logp[128];
    log_t       *lg = net_up(&net, logp, sizeof logp, g_feedtool);

    CHECK(sensor_spawn(&net, SENSOR_AUR, 3, 1) == 0, "sensor_spawn(AUR, 3, 1) failed: %s",
          strerror(errno));
    pid_t pid = net.sensors[SENSOR_AUR].pid;
    long  per[SENSOR_COUNT] = {0};
    read_channels(&net, per, NULL, 5000);
    CHECK(reap_one(&net, 3000) == 1, "sensor_reap did not collect AUR after its burst");
    char want[96];
    snprintf(want, sizeof want, "sensor AUR (pid %d) finished with status 0", (int)pid);
    CHECK(file_has_line(logp, "SYSTEM", want), "the mission log has no SYSTEM line \"%s\"",
          want);
    sensor_net_shutdown(&net);
    log_close(lg);
}

static void reap_does_not_wait(void)
{
    sensor_net_t net;
    char         logp[128];
    log_t       *lg = net_up(&net, logp, sizeof logp, g_feedtool);

    errno = 0;
    CHECK(sensor_reap(NULL) == -1 && errno == EINVAL,
          "sensor_reap(NULL) must give -1 with EINVAL");
    CHECK(sensor_spawn(&net, SENSOR_NWS, 20, 0) == 0, "sensor_spawn(NWS) failed: %s",
          strerror(errno));

    int64_t t0 = sc_now_ns();
    int     n  = sensor_reap(&net);
    int64_t dt = (sc_now_ns() - t0) / 1000000;
    CHECK(n == 0, "sensor_reap returned %d while each sensor runs, not 0", n);
    CHECK(dt < 100, "sensor_reap took %lld ms with a sensor that runs. It must not wait: "
          "use WNOHANG", (long long)dt);
    CHECK(net.sensors[SENSOR_NWS].pid != 0, "sensor_reap changed the pid of a sensor that "
          "runs");

    fflush(NULL);
    pid_t other = fork();
    CHECK(other >= 0, "the test cannot fork");
    if (other == 0) {
        _exit(7);
    }
    sleep_ms(100);
    n = sensor_reap(&net);
    CHECK(n == 0, "sensor_reap returned %d when only a child that is not a sensor stopped", n);
    int st = 0;
    CHECK(waitpid(other, &st, 0) == other, "sensor_reap collected a child that is not a "
          "sensor. Ask about each sensor: waitpid(pid, &status, WNOHANG), not waitpid(-1)");
    sensor_net_shutdown(&net);
    log_close(lg);
}

/* ========================================================== prio_channel.c */

static void prio_open_private_fifo(void)
{
    char path[128];
    path_of(path, sizeof path, "prio.fifo");
    umask(0);

    /* prio_open in a child, so that a call that waits is reported. */
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0, "the test cannot fork");
    if (pid == 0) {
        prio_channel_t ch;
        _exit(prio_open(&ch, path) == 0 ? 0 : 1);
    }
    int st = 0, waited = 0;
    while (waitpid(pid, &st, WNOHANG) == 0) {
        if (waited >= 2000) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            fail("prio_open did not return in 2 s with no writer. Open the read end "
                 "with O_NONBLOCK");
        }
        sleep_ms(10);
        waited += 10;
    }
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "prio_open(%s) failed", path);

    struct stat sb;
    CHECK(stat(path, &sb) == 0, "after prio_open, %s does not exist", path);
    CHECK(S_ISFIFO(sb.st_mode), "%s is not a FIFO", path);
    CHECK((sb.st_mode & 0777) == 0600, "the FIFO has mode %03o with umask 0, not 0600",
          (unsigned)(sb.st_mode & 0777));
    unlink(path);
}

static void prio_open_empty_is_not_eof(void)
{
    char path[128];
    path_of(path, sizeof path, "prio.fifo");
    prio_channel_t ch;
    CHECK(prio_open(&ch, path) == 0, "prio_open failed: %s", strerror(errno));

    int fl = fcntl(ch.fd, F_GETFL);
    CHECK(fl >= 0 && (fl & O_ACCMODE) == O_RDONLY, "ch.fd is not a read end");
    CHECK(fl & O_NONBLOCK, "ch.fd has no O_NONBLOCK");
    int wl = fcntl(ch.fd_hold, F_GETFL);
    CHECK(wl >= 0 && (wl & O_ACCMODE) == O_WRONLY, "ch.fd_hold is not an open write end");
    CHECK(fcntl(ch.fd, F_GETFD) & FD_CLOEXEC, "ch.fd has no O_CLOEXEC");
    CHECK(fcntl(ch.fd_hold, F_GETFD) & FD_CLOEXEC, "ch.fd_hold has no O_CLOEXEC");
    CHECK(strcmp(ch.path, path) == 0, "ch.path is \"%s\", not \"%s\"", ch.path, path);

    struct pollfd p = { ch.fd, POLLIN, 0 };
    int r = poll(&p, 1, 200);
    CHECK(r == 0, "poll() marks the empty FIFO (revents 0x%x): a read gives end of file. "
          "prio_open must hold a write end open in ch.fd_hold", (unsigned)p.revents);
    char c;
    errno = 0;
    ssize_t n = read(ch.fd, &c, 1);
    CHECK(n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK), "a read of the empty FIFO "
          "gave %zd, not -1 with EAGAIN", n);
    prio_close(&ch);
}

static void prio_close_removes(void)
{
    char path[128];
    path_of(path, sizeof path, "prio.fifo");
    put_file(path, "an old entry\n");

    prio_channel_t ch;
    CHECK(prio_open(&ch, path) == 0, "prio_open did not replace an old file at its path: %s",
          strerror(errno));
    struct stat sb;
    CHECK(stat(path, &sb) == 0 && S_ISFIFO(sb.st_mode), "the old file at %s is still there, "
          "not a FIFO", path);
    int fd = ch.fd, hold = ch.fd_hold;
    prio_close(&ch);
    CHECK(stat(path, &sb) != 0 && errno == ENOENT, "after prio_close, %s still exists", path);
    CHECK(fcntl(fd, F_GETFD) == -1, "after prio_close, the read end %d is still open", fd);
    CHECK(fcntl(hold, F_GETFD) == -1, "after prio_close, the write end %d is still open", hold);
    CHECK(ch.fd == -1 && ch.fd_hold == -1 && ch.path[0] == '\0', "after prio_close, ch is "
          "not closed: fd %d, fd_hold %d, path \"%s\"", ch.fd, ch.fd_hold, ch.path);
    prio_close(&ch);
    prio_close(NULL);
}

static void prio_open_errors(void)
{
    prio_channel_t ch;
    errno = 0;
    CHECK(prio_open(NULL, "/tmp/x") == -1 && errno == EINVAL,
          "prio_open(NULL, ...) must give -1 with EINVAL");
    errno = 0;
    CHECK(prio_open(&ch, NULL) == -1 && errno == EINVAL,
          "prio_open(ch, NULL) must give -1 with EINVAL");

    char longp[400];
    memset(longp, 'x', sizeof longp - 1);
    memcpy(longp, g_dir, strlen(g_dir));
    longp[strlen(g_dir)] = '/';
    longp[sizeof longp - 1] = '\0';
    errno = 0;
    CHECK(prio_open(&ch, longp) == -1 && errno == ENAMETOOLONG,
          "a path of 399 characters must give -1 with ENAMETOOLONG");
    CHECK(access(longp, F_OK) != 0, "prio_open made a file at a path that is too long");

    char missing[128];
    path_of(missing, sizeof missing, "no-such-dir/prio.fifo");
    errno = 0;
    CHECK(prio_open(&ch, missing) == -1 && errno == ENOENT,
          "prio_open in a directory that does not exist must give -1 with ENOENT");
    CHECK(ch.fd == -1 && ch.fd_hold == -1 && ch.path[0] == '\0', "after a failed prio_open, "
          "ch is not closed: fd %d, fd_hold %d, path \"%s\"", ch.fd, ch.fd_hold, ch.path);
    prio_close(&ch);
}

static void prio_send_one_write(void)
{
    char path[128];
    path_of(path, sizeof path, "prio.fifo");
    prio_channel_t ch;
    CHECK(prio_open(&ch, path) == 0, "prio_open failed: %s", strerror(errno));

    const char *line = "1755534061.482913 NWS ACCF-URGENT 68.7100 -105.7000 600.0 P";
    g_writes       = 0;
    g_count_writes = 1;
    int rc         = prio_send(path, line);
    g_count_writes = 0;
    CHECK(rc == 0, "prio_send returned %d: %s", rc, strerror(errno));
    CHECK(g_writes == 1, "prio_send made %d write() calls for one line with no '\\n', "
          "not 1. Put the line and its '\\n' in one buffer, and write it once", g_writes);

    char    buf[512];
    ssize_t n = read(ch.fd, buf, sizeof buf - 1);
    CHECK(n > 0, "the FIFO is empty after prio_send");
    buf[n] = '\0';
    char want[256];
    snprintf(want, sizeof want, "%s\n", line);
    CHECK(strcmp(buf, want) == 0, "the FIFO holds \"%s\", not the line and one '\\n'", buf);

    CHECK(prio_send(path, want) == 0, "prio_send of a line with its '\\n' failed");
    n = read(ch.fd, buf, sizeof buf - 1);
    CHECK(n > 0, "the FIFO is empty after prio_send");
    buf[n] = '\0';
    CHECK(strcmp(buf, want) == 0, "a line that has its '\\n' arrived as \"%s\": add a "
          "'\\n' only when it is not there", buf);
    prio_close(&ch);
}

static void prio_send_limits(void)
{
    char path[128];
    path_of(path, sizeof path, "no-fifo-here");
    char line[CONTACT_LINE_MAX + 8];
    memset(line, 'A', CONTACT_LINE_MAX);
    line[CONTACT_LINE_MAX] = '\0';
    errno = 0;
    CHECK(prio_send(path, line) == -1 && errno == EMSGSIZE, "a line of %d characters "
          "with no '\\n' (%d with it) must give -1 with EMSGSIZE, before any open",
          CONTACT_LINE_MAX, CONTACT_LINE_MAX + 1);
    errno = 0;
    CHECK(prio_send(NULL, "x") == -1 && errno == EINVAL,
          "prio_send(NULL, ...) must give -1 with EINVAL");
    errno = 0;
    CHECK(prio_send(path, NULL) == -1 && errno == EINVAL,
          "prio_send(path, NULL) must give -1 with EINVAL");
}

/* ================================================================ sched.c */

static void sched_fcfs_order(void)
{
    sched_t *s = sched_create(POLICY_FCFS, 16);
    CHECK(s != NULL, "sched_create(POLICY_FCFS, 16) returned NULL");
    pending_t p[] = {
        pend("SAT", "a", 0, URGENCY_ROUTINE), pend("NWS", "b", 1, URGENCY_SENSOR),
        pend("AUR", "c", 0, URGENCY_HIGH),    pend("SAT", "d", 0, URGENCY_ROUTINE),
    };
    for (size_t i = 0; i < sizeof p / sizeof p[0]; i++) {
        CHECK(sched_push(s, &p[i]) == 0, "sched_push failed: %s", strerror(errno));
    }
    CHECK(sched_pending(s) == 4, "sched_pending is %zu after 4 pushes", sched_pending(s));
    char got[128];
    drain_ids(s, got, sizeof got);
    CHECK(strcmp(got, "a b c d") == 0, "FCFS gave the order \"%s\", not \"a b c d\"", got);
    sched_destroy(s);
}

static void sched_priority_order(void)
{
    sched_t *s = sched_create(POLICY_PRIORITY, 16);
    CHECK(s != NULL, "sched_create(POLICY_PRIORITY, 16) returned NULL");
    /* r: routine; p: the P flag of the sensor; h: HIGH from the centre. The
       report "p2" has the P flag and no class: pending_urgency makes it P. */
    pending_t p[] = {
        pend("SAT", "r1", 0, URGENCY_ROUTINE), pend("NWS", "p1", 1, URGENCY_SENSOR),
        pend("AIS", "h1", 0, URGENCY_HIGH),    pend("SAT", "r2", 0, URGENCY_ROUTINE),
        pend("AUR", "p2", 1, URGENCY_ROUTINE), pend("NWS", "h2", 1, URGENCY_HIGH),
    };
    for (size_t i = 0; i < sizeof p / sizeof p[0]; i++) {
        CHECK(sched_push(s, &p[i]) == 0, "sched_push failed: %s", strerror(errno));
    }
    char got[128];
    drain_ids(s, got, sizeof got);
    CHECK(strcmp(got, "h1 h2 p1 p2 r1 r2") == 0, "priority gave the order \"%s\", not "
          "\"h1 h2 p1 p2 r1 r2\": HIGH, then P, then routine, the oldest first in each. "
          "Use pending_urgency", got);
    sched_destroy(s);
}

static void sched_rr_order(void)
{
    sched_t *s = sched_create(POLICY_RR, 16);
    CHECK(s != NULL, "sched_create(POLICY_RR, 16) returned NULL");
    const char *names[] = { "SAT", "SAT", "SAT", "AUR", "NWS", "AIS", "SAT" };
    const char *ids[]   = { "s1",  "s2",  "s3",  "u1",  "n1",  "i1",  "s4"  };
    for (int i = 0; i < 7; i++) {
        pending_t p = pend(names[i], ids[i], 0, URGENCY_ROUTINE);
        CHECK(sched_push(s, &p) == 0, "sched_push failed: %s", strerror(errno));
    }
    char got[128];
    drain_ids(s, got, sizeof got);
    CHECK(strcmp(got, "n1 u1 i1 s1 s2 s3 s4") == 0, "round robin gave the order \"%s\", "
          "not \"n1 u1 i1 s1 s2 s3 s4\": NWS, AUR, AIS, SAT in turn, from NWS", got);

    /* The next turn starts after the sensor served last, not at NWS. */
    pending_t a = pend("AIS", "i2", 0, URGENCY_ROUTINE), n = pend("NWS", "n2", 0, URGENCY_ROUTINE);
    pending_t out;
    CHECK(sched_push(s, &a) == 0 && sched_push(s, &n) == 0, "sched_push failed");
    CHECK(sched_next(s, &out) == 1 && strcmp(out.c.id, "n2") == 0, "after SAT, the turn is "
          "NWS: round robin gave \"%s\", not \"n2\"", out.c.id);
    pending_t n3 = pend("NWS", "n3", 0, URGENCY_ROUTINE);
    CHECK(sched_push(s, &n3) == 0, "sched_push failed");
    CHECK(sched_next(s, &out) == 1 && strcmp(out.c.id, "i2") == 0, "after NWS, the turn "
          "goes to AUR and then AIS: round robin gave \"%s\", not \"i2\"", out.c.id);
    CHECK(sched_next(s, &out) == 1 && strcmp(out.c.id, "n3") == 0, "round robin gave \"%s\", "
          "not \"n3\"", out.c.id);

    pending_t x = pend("XYZ", "x1", 0, URGENCY_ROUTINE);
    CHECK(sched_push(s, &x) == 0, "sched_push failed");
    CHECK(sched_next(s, &out) == 1 && strcmp(out.c.id, "x1") == 0, "a report with no "
          "sensor name must still come out, the oldest first");
    sched_destroy(s);
}

static void sched_capacity(void)
{
    sched_t *s = sched_create(POLICY_FCFS, 2);
    CHECK(s != NULL, "sched_create(POLICY_FCFS, 2) returned NULL");
    pending_t a = pend("NWS", "a", 0, URGENCY_ROUTINE), b = pend("NWS", "b", 0, URGENCY_ROUTINE);
    pending_t c = pend("NWS", "c", 0, URGENCY_ROUTINE), out;
    CHECK(sched_push(s, &a) == 0 && sched_push(s, &b) == 0, "two pushes into room for 2 failed");
    errno = 0;
    CHECK(sched_push(s, &c) == -1 && errno == ENOSPC, "a push into a full scheduler must "
          "give -1 with ENOSPC");
    CHECK(sched_pending(s) == 2, "after ENOSPC, sched_pending is %zu, not 2", sched_pending(s));
    CHECK(sched_next(s, &out) == 1 && strcmp(out.c.id, "a") == 0, "after ENOSPC, "
          "sched_next must still give \"a\"");
    CHECK(sched_push(s, &c) == 0, "with room again, sched_push failed");
    char got[64];
    drain_ids(s, got, sizeof got);
    CHECK(strcmp(got, "b c") == 0, "after ENOSPC, the order is \"%s\", not \"b c\"", got);
    sched_destroy(s);

    s = sched_create(POLICY_FCFS, 0);
    CHECK(s != NULL, "sched_create(POLICY_FCFS, 0) returned NULL");
    for (int i = 0; i < 4096; i++) {
        CHECK(sched_push(s, &a) == 0, "capacity 0 means 4096, but push %d failed", i + 1);
    }
    errno = 0;
    CHECK(sched_push(s, &a) == -1 && errno == ENOSPC, "capacity 0 means 4096: push 4097 "
          "must give ENOSPC");
    sched_destroy(s);
}

static void sched_errors(void)
{
    errno = 0;
    CHECK(sched_create((sched_policy_t)9, 8) == NULL && errno == EINVAL,
          "sched_create with policy 9 must give NULL with EINVAL");
    errno = 0;
    CHECK(sched_create(POLICY_RR, SIZE_MAX) == NULL && errno == ENOMEM,
          "sched_create with capacity SIZE_MAX must give NULL with ENOMEM");
    sched_t  *s = sched_create(POLICY_FCFS, 4);
    CHECK(s != NULL, "sched_create returned NULL");
    pending_t p = pend("NWS", "a", 0, URGENCY_ROUTINE);
    errno = 0;
    CHECK(sched_push(NULL, &p) == -1 && errno == EINVAL, "sched_push(NULL, ...) must give "
          "-1 with EINVAL");
    errno = 0;
    CHECK(sched_push(s, NULL) == -1 && errno == EINVAL, "sched_push(s, NULL) must give -1 "
          "with EINVAL");
    errno = 0;
    CHECK(sched_next(NULL, &p) == -1 && errno == EINVAL, "sched_next(NULL, ...) must give "
          "-1 with EINVAL");
    errno = 0;
    CHECK(sched_next(s, NULL) == -1 && errno == EINVAL, "sched_next(s, NULL) must give -1 "
          "with EINVAL");
    CHECK(sched_next(s, &p) == 0, "sched_next on an empty scheduler must give 0");
    CHECK(sched_pending(NULL) == 0, "sched_pending(NULL) must give 0");
    sched_destroy(s);
    sched_destroy(NULL);
}

/* ============================================================== libsyscall */

static void libsyscall_cloexec(void)
{
    char path[128];
    path_of(path, sizeof path, "cloexec.txt");
    int w = sc_open_write_mode(path, 1, 0600);
    CHECK(w >= 0, "sc_open_write_mode failed: %s", strerror(errno));
    CHECK(fcntl(w, F_GETFD) & FD_CLOEXEC, "sc_open_write_mode gives a descriptor with no "
          "O_CLOEXEC. Each sensor then holds the mission log after exec");
    int r = sc_open_read(path);
    CHECK(r >= 0, "sc_open_read failed: %s", strerror(errno));
    CHECK(fcntl(r, F_GETFD) & FD_CLOEXEC, "sc_open_read gives a descriptor with no O_CLOEXEC");
    close(w);
    close(r);
}

/* =============================================================== demo_lab2 */

static void demo_end_to_end(void)
{
    char logp[128], outp[128];
    path_of(logp, sizeof logp, "demo.log");
    path_of(outp, sizeof outp, "demo.out");
    unlink(logp);

    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0, "the test cannot fork");
    if (pid == 0) {
        int out = open(outp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out >= 0) {
            dup2(out, STDOUT_FILENO);
            dup2(out, STDERR_FILENO);
            close(out);
        }
        execl(g_demo, g_demo, "--sched", "priority", "--ms", "2000", "--kill-sensor", "AUR",
              "--log", logp, "--feed", g_feedtool, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    CHECK(!(WIFEXITED(st) && WEXITSTATUS(st) == 127), "%s does not run. Run make first",
          g_demo);

    char *out = get_file(outp);
    CHECK(out != NULL, "demo_lab2 wrote no output");
    const char *need[] = {
        "R1 four sensors report at once      yes",
        "R1 centre survives a sensor death   yes",
        "R2 priority contact overtakes       yes",
        "urgent contact via FIFO             received",
    };
    for (size_t i = 0; i < sizeof need / sizeof need[0]; i++) {
        CHECK(strstr(out, need[i]) != NULL, "the demo did not print \"%s\". Its output is "
              "in %s", need[i], outp);
    }
    free(out);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "demo_lab2 exited with status %d, not 0",
          WIFEXITED(st) ? WEXITSTATUS(st) : -1);

    char *log = get_file(logp);
    CHECK(log != NULL, "demo_lab2 wrote no mission log at %s", logp);
    char *dead = strstr(log, "died on signal 9");
    char *done = strstr(log, "Part 2 complete:");
    CHECK(dead != NULL, "the mission log has no ALERT for the killed AUR process");
    CHECK(done != NULL && dead < done, "the ALERT for AUR comes after the end of the run. "
          "sensor_reap must collect it while the centre runs");
    free(log);
    CHECK(access(FIFO_DEMO, F_OK) != 0, "%s still exists after the demo: prio_close must "
          "remove it", FIFO_DEMO);
}

/* ================================================================ runner */

struct check {
    const char *group;
    const char *name;
    void      (*fn)(void);
};

static const struct check kChecks[] = {
    { "sensor.c", "sensor_spawn starts contactfeed, and its lines arrive on its channel", spawn_runs_contactfeed },
    { "sensor.c", "sensor_spawn passes the exact arguments, with --rate or --burst", spawn_exact_argv },
    { "sensor.c", "each sensor holds its own pipe as fd 1, no other channel, and not the log", spawn_child_descriptors },
    { "sensor.c", "the channel of a sensor that exits reaches end of file", spawn_channel_ends },
    { "sensor.c", "sensor_spawn writes the SYSTEM line \"sensor NAME started ...\"", spawn_log_line },
    { "sensor.c", "sensor_spawn gives EINVAL, EEXIST and EBUSY", spawn_errors },
    { "sensor.c", "a failed exec: the child exits 127, and the log says so", spawn_exec_fails },
    { "sensor.c", "a failed exec does not write the text of the centre again", spawn_no_double_output },
    { "sensor.c", "a sensor may use each processor when the centre is pinned to one", spawn_gives_processors_back },
    { "sensor.c", "sensor_stop stops, collects and logs a sensor, and keeps its channel", stop_collects },
    { "sensor.c", "sensor_reap collects a killed sensor, and logs an ALERT", reap_killed },
    { "sensor.c", "sensor_reap collects a sensor that finished, and logs status 0", reap_finished },
    { "sensor.c", "sensor_reap does not wait, and does not take another child", reap_does_not_wait },
    { "prio_channel.c", "prio_open returns with no writer, and makes a FIFO of mode 0600", prio_open_private_fifo },
    { "prio_channel.c", "prio_open: an empty FIFO is empty, not end of file", prio_open_empty_is_not_eof },
    { "prio_channel.c", "prio_open replaces an old entry, and prio_close removes the FIFO", prio_close_removes },
    { "prio_channel.c", "prio_open gives EINVAL, ENAMETOOLONG and ENOENT, and stays closed", prio_open_errors },
    { "prio_channel.c", "prio_send writes the line and one '\\n' in one write()", prio_send_one_write },
    { "prio_channel.c", "prio_send refuses a long line with EMSGSIZE, and NULL with EINVAL", prio_send_limits },
    { "sched.c", "FCFS gives the order of arrival", sched_fcfs_order },
    { "sched.c", "priority gives HIGH, then P, then routine, the oldest first in each", sched_priority_order },
    { "sched.c", "round robin gives NWS, AUR, AIS, SAT in turn", sched_rr_order },
    { "sched.c", "a full scheduler gives ENOSPC, and still works", sched_capacity },
    { "sched.c", "sched_create, sched_push and sched_next give EINVAL and ENOMEM", sched_errors },
    { "libsyscall", "sc_open_read and sc_open_write_mode open with O_CLOEXEC", libsyscall_cloexec },
    { "demo_lab2", "the demo kills AUR, continues, and each requirement line says yes", demo_end_to_end },
};

#define NCHECKS (sizeof kChecks / sizeof kChecks[0])

static void print_indented(const char *text, int max_lines)
{
    int lines = 0;
    while (*text != '\0' && lines < max_lines) {
        size_t n = strcspn(text, "\n");
        printf("        %.*s\n", (int)n, text);
        text += n;
        if (*text == '\n') {
            text++;
        }
        lines++;
    }
}

static int run_check(const struct check *c)
{
    char msg_path[128], out_path[128];
    path_of(msg_path, sizeof msg_path, "check.msg");
    path_of(out_path, sizeof out_path, "check.out");
    int msgfd = open(msg_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (msgfd < 0) {
        printf("  FAIL  %s\n        the test cannot create %s\n", c->name, msg_path);
        return 0;
    }

    /* The check runs in a child process, in its own process group. alarm()
       ends it after TIMEOUT_S seconds, and waitpid() collects its status.
       Then kill() with a negative pid stops each process that it started. */
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(msgfd);
        printf("  FAIL  %s\n        the test cannot fork\n", c->name);
        return 0;
    }
    if (pid == 0) {
        setpgid(0, 0);
        g_msgfd = msgfd;
        int out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out >= 0) {
            dup2(out, STDOUT_FILENO);
            dup2(out, STDERR_FILENO);
            close(out);
        }
        umask(022);
        alarm(TIMEOUT_S);
        c->fn();
        fflush(NULL);
        _exit(0);
    }
    close(msgfd);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    kill(-pid, SIGKILL);

    char *msg = get_file(msg_path);
    char *out = get_file(out_path);
    int   ok  = WIFEXITED(status) && WEXITSTATUS(status) == 0;

    if (ok) {
        printf("  PASS  %s\n", c->name);
    } else {
        printf("  FAIL  %s\n", c->name);
        if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM) {
            printf("        no result after %d s: a call did not return\n", TIMEOUT_S);
        } else if (WIFSIGNALED(status)) {
            printf("        your code crashed: signal %d (%s)\n", WTERMSIG(status),
                   strsignal(WTERMSIG(status)));
        } else if (msg != NULL && msg[0] != '\0') {
            print_indented(msg, 40);
        } else {
            printf("        the check stopped with exit status %d: your code called exit()\n",
                   WEXITSTATUS(status));
        }
        if (out != NULL && out[0] != '\0') {
            printf("        the check printed:\n");
            print_indented(out, 3);
        }
    }
    free(msg);
    free(out);
    return ok;
}

static void remove_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    char           path[512];
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            unlink(path);
        }
    }
    closedir(d);
    rmdir(dir);
}

/* The given contactfeed of the student repository, for this processor. */
static void default_feedtool(void)
{
    struct utsname u;
    const char    *arch = "unknown";
    if (uname(&u) == 0) {
        arch = u.machine;
        if (strcmp(arch, "arm64") == 0) {
            arch = "aarch64";
        } else if (strcmp(arch, "amd64") == 0) {
            arch = "x86_64";
        }
    }
    snprintf(g_feedtool, sizeof g_feedtool, "tools/bin/%s/contactfeed", arch);
    if (access(g_feedtool, X_OK) != 0 && access("build/contactfeed", X_OK) == 0) {
        snprintf(g_feedtool, sizeof g_feedtool, "build/contactfeed");
    }
}

/* A whole-number percentage, rounded half up, with integer arithmetic only. */
static int percent(size_t part, size_t whole)
{
    if (whole == 0) {
        return 0;
    }
    return (int)((part * 200 + whole) / (whole * 2));
}

#define GROUPS_MAX 8

int main(int argc, char **argv)
{
    default_feedtool();
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0 && i + 1 < argc) {
            g_demo = argv[++i];
        } else if (strcmp(argv[i], "--contactfeed") == 0 && i + 1 < argc) {
            snprintf(g_feedtool, sizeof g_feedtool, "%s", argv[++i]);
        } else {
            fprintf(stderr, "usage: %s [--demo PATH] [--contactfeed PATH]\n", argv[0]);
            return 2;
        }
    }

    snprintf(g_dir, sizeof g_dir, "/tmp/sentinel_accept2_XXXXXX");
    if (mkdtemp(g_dir) == NULL) {
        fprintf(stderr, "cannot create a directory in /tmp: %s\n", strerror(errno));
        return 2;
    }

    /* A sensor whose reader has gone must not end the test with SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);

    printf("SENTINEL Lab 2 acceptance\n");

    const char *group    = "";
    size_t      passed   = 0;
    size_t      grp_pass = 0, grp_total = 0;
    char        grp_name[GROUPS_MAX][32];
    size_t      grp_p[GROUPS_MAX], grp_t[GROUPS_MAX];
    size_t      ngroups  = 0;

    for (size_t i = 0; i < NCHECKS; i++) {
        if (strcmp(group, kChecks[i].group) != 0) {
            if (ngroups > 0) {
                grp_p[ngroups - 1] = grp_pass;
                grp_t[ngroups - 1] = grp_total;
            }
            group = kChecks[i].group;
            printf("\n%s\n", group);
            if (ngroups == GROUPS_MAX) {
                fprintf(stderr, "accept_lab2: more than %d check groups. "
                                "Raise GROUPS_MAX.\n", GROUPS_MAX);
                return 2;
            }
            snprintf(grp_name[ngroups], sizeof grp_name[0], "%s", group);
            ngroups++;
            grp_pass = grp_total = 0;
        }
        int ok = run_check(&kChecks[i]);
        passed    += (size_t)ok;
        grp_pass  += (size_t)ok;
        grp_total += 1;
    }
    if (ngroups > 0) {
        grp_p[ngroups - 1] = grp_pass;
        grp_t[ngroups - 1] = grp_total;
    }
    remove_dir(g_dir);

    printf("\n");
    for (size_t g = 0; g < ngroups; g++) {
        printf("  %-15s %2zu of %-2zu  %3d%%\n", grp_name[g], grp_p[g], grp_t[g],
               percent(grp_p[g], grp_t[g]));
    }

    printf("\n%zu of %zu checks pass (%d%%).\n", passed, NCHECKS, percent(passed, NCHECKS));
    printf("These checks are a part of the contract. The course tests check more of it:\n"
           "Section 7 of docs/lab2/lab2_brief.pdf.\n");
    return passed == NCHECKS ? 0 : 1;
}
