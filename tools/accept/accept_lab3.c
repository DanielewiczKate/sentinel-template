/* accept_lab3 — the acceptance test of Lab 3.
 *
 *   accept_lab3 [--demo PATH] [--contactfeed PATH]
 *
 * `make accept3` compiles this file with your build/libipc.a, your
 * build/libproc.a, your build/libsyscall.a and build/libcommon.a. So each
 * call to a queue_* or pool_* function below runs your code, or the given
 * code of src/libipc/given/. Then the test runs build/demo_lab3 as a
 * program. It prints PASS or FAIL for each of 14 checks, then the share that
 * passed for each file.
 *
 * Read a check to see what it does. Do not change this file. The marks come
 * from the course copy of it, and Section 7 of docs/lab3/lab3_brief.pdf
 * gives them.
 *
 * How the test works:
 *
 *   - Each check runs in its own child process, in its own process group,
 *     with a time limit. A crash, or a call that does not return, stops
 *     that check only.
 *
 *   - This file defines pthread_create(). Your library is linked to this
 *     definition. It holds each new thread at a gate until the check opens
 *     it, so a check can fill the queue or set the stop flag while no
 *     worker runs. It can also make one call fail. In each other case it
 *     starts the thread with the pthread_create() of the C library.
 *
 *   - Each check that needs an exact total uses ONE worker, or fills the
 *     queue while no worker runs. With no lock, many workers do not give an
 *     exact total, and no check asks for one.
 *
 *   - The test uses no assert(). A failed check prints a sentence about the
 *     contract, not a line number in this file.
 */

#include "sentinel/ipc.h"
#include "contact.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
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

#define TIMEOUT_S 30

static char        g_dir[64];          /* the temporary directory of this run */
static const char *g_demo = "build/demo_lab3";
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

/* ---- small helpers, plain libc only ------------------------------------ */

static void path_of(char *buf, size_t n, const char *name)
{
    snprintf(buf, n, "%s/%s", g_dir, name);
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

/* ---- pthread_create, with a gate and a fault --------------------------- */

/* This file defines pthread_create. Your library is linked to this
   definition, so a check can do two things that it cannot do from outside:

     - hold each new worker at a GATE before its first step, until the check
       opens the gate. A check then fills the queue, or sets the stop flag,
       while no worker runs, and the result does not depend on timing;
     - make one call fail, as the kernel does when it has no thread left.

   In each other case it starts the thread with the pthread_create of the C
   library. */

static volatile int g_gate_open = 1;   /* 0: new threads wait */
static volatile int g_fail_at   = 0;   /* fail call number g_fail_at, from 1 */
static volatile int g_calls     = 0;

struct tramp {
    void *(*fn)(void *);
    void  *arg;
};

static void *trampoline(void *arg)
{
    struct tramp t = *(struct tramp *)arg;
    free(arg);
    while (!g_gate_open) {
        usleep(1000);
    }
    return t.fn(t.arg);
}

int pthread_create(pthread_t *tid, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg)
{
    typedef int (*real_t)(pthread_t *, const pthread_attr_t *,
                          void *(*)(void *), void *);
    static real_t real;
    if (real == NULL) {
        real = (real_t)dlsym(RTLD_NEXT, "pthread_create");
        if (real == NULL) {
            return ENOSYS;
        }
    }
    g_calls++;
    if (g_fail_at > 0 && g_calls == g_fail_at) {
        return EAGAIN;
    }
    struct tramp *t = malloc(sizeof *t);
    if (t == NULL) {
        return ENOMEM;
    }
    t->fn  = fn;
    t->arg = arg;
    int rc = real(tid, attr, trampoline, t);
    if (rc != 0) {
        free(t);
    }
    return rc;
}

/* ---- helpers for the checks -------------------------------------------- */

/* The number of threads of this process: the entries of /proc/self/task. */
static int thread_count(void)
{
    DIR *d = opendir("/proc/self/task");
    CHECK(d != NULL, "the test cannot read /proc/self/task");
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            n++;
        }
    }
    closedir(d);
    return n;
}

/* Wait up to ms for the number of threads to become want. */
static int threads_become(int want, int ms)
{
    for (int waited = 0; waited < ms; waited += 5) {
        if (thread_count() == want) {
            return 1;
        }
        sleep_ms(5);
    }
    return thread_count() == want;
}

static pending_t pend(const char *sensor, const char *id, int p_flag, urgency_t u)
{
    pending_t p;
    memset(&p, 0, sizeof p);
    snprintf(p.c.sensor, sizeof p.c.sensor, "%s", sensor);
    snprintf(p.c.id, sizeof p.c.id, "%s", id);
    p.c.flag   = p_flag ? PRIORITY_PRIORITY : PRIORITY_ROUTINE;
    p.c.lat    = 60.0;
    p.c.lon    = -100.0;
    p.c.speed  = 300.0;
    p.urgency  = u;
    p.arrived_ns = sc_now_ns();
    return p;
}

/* A routine report with the id ID-<n>. */
static pending_t pend_n(long n)
{
    char id[32];
    snprintf(id, sizeof id, "ID-%ld", n);
    return pend("NWS", id, 0, URGENCY_ROUTINE);
}

/* Put n reports into q, with n_distinct ids. */
static void fill(queue_t *q, long n, long n_distinct)
{
    for (long i = 0; i < n; i++) {
        pending_t p = pend_n(i % n_distinct);
        CHECK(queue_push(q, &p) == 0, "queue_push of report %ld of %ld failed: %s",
              i + 1, n, strerror(errno));
    }
}

/* Keep this process on one processor, as the centre runs. */
static void one_processor(void)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(sched_getcpu() < 0 ? 0 : sched_getcpu(), &set);
    sched_setaffinity(0, sizeof set, &set);
}

static log_t *test_log(char *logp, size_t n)
{
    path_of(logp, n, "pool.log");
    unlink(logp);
    log_t *lg = log_open(logp);
    CHECK(lg != NULL, "log_open(%s) failed: %s", logp, strerror(errno));
    return lg;
}

/* =============================================================== queue.c */

static void q_init_empty(void)
{
    queue_t q;
    memset(&q, 0xA5, sizeof q);
    CHECK(queue_init(&q, 16) == 0, "queue_init(&q, 16) failed: %s", strerror(errno));
    CHECK(q.slots != NULL, "after queue_init, q.slots is NULL");
    CHECK(q.capacity == 16, "after queue_init(&q, 16), capacity is %zu", q.capacity);
    CHECK(q.count == 0 && q.head == 0 && q.tail == 0, "after queue_init, count, head and "
          "tail are %zu, %zu and %zu, not 0", q.count, q.head, q.tail);
    CHECK(q.pushed == 0 && q.popped == 0 && q.rejected == 0 && q.peak == 0,
          "after queue_init, each statistic must be 0. queue_init must set each field to 0");
    CHECK(queue_count(&q) == 0, "queue_count of an empty queue is %zu", queue_count(&q));
    pending_t out;
    CHECK(queue_pop(&q, &out) == 0, "queue_pop of an empty queue must give 0");
    queue_destroy(&q);
    CHECK(q.slots == NULL && q.capacity == 0 && q.count == 0, "after queue_destroy, slots "
          "must be NULL, and capacity and count 0");
    queue_destroy(NULL);
}

static void q_fifo_order(void)
{
    queue_t q;
    CHECK(queue_init(&q, 8) == 0, "queue_init failed: %s", strerror(errno));
    const char *ids[] = { "A-1", "B-2", "C-3", "D-4", "E-5" };
    for (int i = 0; i < 5; i++) {
        pending_t p = pend("SAT", ids[i], i == 2, URGENCY_ROUTINE);
        p.arrived_ns = 1000 + i;
        CHECK(queue_push(&q, &p) == 0, "queue_push %d failed: %s", i + 1, strerror(errno));
        CHECK(queue_count(&q) == (size_t)(i + 1), "after %d pushes, queue_count is %zu",
              i + 1, queue_count(&q));
    }
    for (int i = 0; i < 5; i++) {
        pending_t out;
        memset(&out, 0, sizeof out);
        CHECK(queue_pop(&q, &out) == 1, "pop %d of 5 did not give 1", i + 1);
        CHECK(strcmp(out.c.id, ids[i]) == 0, "pop %d gave %s, not %s: the queue must give "
              "the order of arrival", i + 1, out.c.id, ids[i]);
        CHECK(out.arrived_ns == 1000 + i && (i != 2 || out.c.flag == PRIORITY_PRIORITY),
              "pop %d did not give a whole copy of the report", i + 1);
        CHECK(queue_count(&q) == (size_t)(4 - i), "after pop %d, queue_count is %zu, not %d. "
              "queue_pop must take 1 from count", i + 1, queue_count(&q), 4 - i);
    }
    pending_t out;
    CHECK(queue_pop(&q, &out) == 0, "after 5 pushes and 5 pops, queue_pop must give 0");
    queue_destroy(&q);
}

static void q_wraps(void)
{
    queue_t q;
    CHECK(queue_init(&q, 4) == 0, "queue_init failed: %s", strerror(errno));
    long next_in = 0, next_out = 0;
    for (int round = 0; round < 25; round++) {
        for (int k = 0; k < 3; k++) {
            pending_t p = pend_n(next_in++);
            CHECK(queue_push(&q, &p) == 0, "in round %d, push %d of 3 into a queue of 4 "
                  "slots failed: %s. head and tail must go back to slot 0 after slot 3",
                  round + 1, k + 1, strerror(errno));
        }
        for (int k = 0; k < 3; k++) {
            pending_t out;
            CHECK(queue_pop(&q, &out) == 1, "in round %d, pop %d of 3 gave no report",
                  round + 1, k + 1);
            char want[32];
            snprintf(want, sizeof want, "ID-%ld", next_out++);
            CHECK(strcmp(out.c.id, want) == 0, "in round %d, a queue of 4 slots gave %s, not "
                  "%s. head and tail must use (index + 1) %% capacity", round + 1,
                  out.c.id, want);
        }
    }
    CHECK(q.head < 4 && q.tail < 4, "head %zu or tail %zu is outside the 4 slots", q.head,
          q.tail);
    queue_destroy(&q);
}

static void q_full(void)
{
    queue_t q;
    CHECK(queue_init(&q, 3) == 0, "queue_init failed: %s", strerror(errno));
    fill(&q, 3, 3);
    pending_t extra = pend("SAT", "EXTRA", 1, URGENCY_HIGH);
    errno = 0;
    CHECK(queue_push(&q, &extra) == -1 && errno == ENOSPC, "a push into a full queue must "
          "give -1 with ENOSPC");
    CHECK(queue_push(&q, &extra) == -1, "a second push into a full queue must give -1");
    CHECK(q.rejected == 2, "after 2 refused pushes, rejected is %ld, not 2", q.rejected);
    CHECK(queue_count(&q) == 3, "a refused push changed the count to %zu", queue_count(&q));
    for (int i = 0; i < 3; i++) {
        pending_t out;
        CHECK(queue_pop(&q, &out) == 1, "pop %d of a full queue gave no report", i + 1);
        char want[32];
        snprintf(want, sizeof want, "ID-%d", i);
        CHECK(strcmp(out.c.id, want) == 0, "pop %d gave %s, not %s: a refused push must not "
              "write a slot", i + 1, out.c.id, want);
    }
    pending_t p = pend_n(9);
    CHECK(queue_push(&q, &p) == 0, "after the pops, a push must succeed again: %s",
          strerror(errno));
    queue_destroy(&q);
}

static void q_stats(void)
{
    queue_t q;
    CHECK(queue_init(&q, 10) == 0, "queue_init failed: %s", strerror(errno));
    fill(&q, 6, 6);
    pending_t out;
    for (int i = 0; i < 4; i++) {
        CHECK(queue_pop(&q, &out) == 1, "pop %d gave no report", i + 1);
    }
    fill(&q, 3, 3);
    CHECK(queue_pushed(&q) == 9, "pushed is %ld after 9 pushes", queue_pushed(&q));
    CHECK(queue_popped(&q) == 4, "popped is %ld after 4 pops", queue_popped(&q));
    CHECK(queue_peak(&q) == 6, "peak is %zu: the count was 6 at most", queue_peak(&q));
    CHECK(queue_count(&q) == 5, "count is %zu, not 5", queue_count(&q));
    CHECK(queue_dropped(&q) == 0, "rejected is %ld with no refused push", queue_dropped(&q));
    CHECK(queue_pop(&q, &out) == 1 && queue_popped(&q) == 5, "a pop must add 1 to popped");
    while (queue_pop(&q, &out) == 1) {
    }
    CHECK(queue_popped(&q) == 9, "popped is %ld after 9 pops", queue_popped(&q));
    CHECK(queue_pop(&q, &out) == 0 && queue_popped(&q) == 9, "a pop from an empty queue "
          "must not change popped");
    queue_destroy(&q);
}

static void q_errors(void)
{
    queue_t   q;
    pending_t p = pend_n(1), out;
    errno = 0;
    CHECK(queue_init(NULL, 4) == -1 && errno == EINVAL, "queue_init(NULL, 4) must give -1 "
          "with EINVAL");
    errno = 0;
    CHECK(queue_init(&q, 0) == -1 && errno == EINVAL, "queue_init(&q, 0) must give -1 with "
          "EINVAL");
    CHECK(queue_init(&q, 4) == 0, "queue_init failed: %s", strerror(errno));
    errno = 0;
    CHECK(queue_push(NULL, &p) == -1 && errno == EINVAL, "queue_push(NULL, p) must give -1 "
          "with EINVAL");
    errno = 0;
    CHECK(queue_push(&q, NULL) == -1 && errno == EINVAL, "queue_push(q, NULL) must give -1 "
          "with EINVAL");
    errno = 0;
    CHECK(queue_pop(NULL, &out) == -1 && errno == EINVAL, "queue_pop(NULL, out) must give "
          "-1 with EINVAL");
    errno = 0;
    CHECK(queue_pop(&q, NULL) == -1 && errno == EINVAL, "queue_pop(q, NULL) must give -1 "
          "with EINVAL");
    CHECK(queue_count(NULL) == 0, "queue_count(NULL) must give 0");
    CHECK(q.pushed == 0 && q.rejected == 0 && q.count == 0, "a call that gave EINVAL "
          "changed the queue");
    queue_destroy(&q);
}

/* ================================================================ pool.c */

static void p_threads(void)
{
    queue_t q;
    track_t t;
    CHECK(queue_init(&q, 64) == 0 && track_init(&t) == 0, "the test cannot start");
    int base = thread_count();
    for (int n = 1; n <= 4; n += 3) {
        pool_t *p = pool_create_deadline(n, &q, &t, 0, 0, NULL);
        CHECK(p != NULL, "pool_create_deadline(%d, ...) failed: %s", n, strerror(errno));
        CHECK(pool_worker_count(p) == n, "the pool has %d workers, not %d",
              pool_worker_count(p), n);
        CHECK(threads_become(base + n, 2000), "with a pool of %d workers, the process has %d "
              "threads, not %d", n, thread_count(), base + n);
        pool_stop(p);
        CHECK(threads_become(base, 2000), "after pool_stop, %d threads run, not %d",
              thread_count(), base);
        pool_destroy(p);
    }
    queue_destroy(&q);
    track_destroy(&t);
}

static void p_own_block(void)
{
    queue_t q;
    track_t t;
    CHECK(queue_init(&q, 64) == 0 && track_init(&t) == 0, "the test cannot start");
    pool_t *p = pool_create_deadline(4, &q, &t, 0, 0, NULL);
    CHECK(p != NULL, "pool_create_deadline(4, ...) failed: %s", strerror(errno));
    pid_t tids[4];
    for (int wait = 0; wait < 200; wait++) {
        int all = 1;
        for (int i = 0; i < 4; i++) {
            tids[i] = pool_worker_tid(p, i);
            all = all && tids[i] > 0;
        }
        if (all) {
            break;
        }
        sleep_ms(5);
    }
    for (int i = 0; i < 4; i++) {
        CHECK(p->workers[i].pool == p && p->workers[i].index == i, "workers[%d] has pool %p "
              "and index %d. Set them before pthread_create", i, (void *)p->workers[i].pool,
              p->workers[i].index);
        CHECK(tids[i] > 0, "worker %d never ran pool_worker_start on its own block. Give "
              "thread i the address &workers[i], not the address of a variable that changes",
              i);
        for (int j = 0; j < i; j++) {
            CHECK(tids[i] != tids[j], "workers %d and %d have the same thread id: two "
                  "threads got the same block", j, i);
        }
    }
    pool_stop(p);
    pool_destroy(p);
    queue_destroy(&q);
    track_destroy(&t);
}

static void p_one_worker_exact(void)
{
    queue_t q;
    track_t t;
    CHECK(queue_init(&q, 5000) == 0 && track_init(&t) == 0, "the test cannot start");
    fill(&q, 3000, 37);
    pool_t *p = pool_create_deadline(1, &q, &t, 0, 0, NULL);
    CHECK(p != NULL, "pool_create_deadline(1, ...) failed: %s", strerror(errno));
    for (int wait = 0; wait < 400 && queue_count(&q) > 0; wait++) {
        sleep_ms(5);
    }
    pool_stop(p);
    const worker_stats_t *s = pool_worker_stats(p, 0);
    CHECK(pool_total_processed(p) == 3000, "one worker processed %ld of 3000 reports",
          pool_total_processed(p));
    CHECK(s->new_tracks == 37 && s->updates == 3000 - 37, "one worker counted %ld new tracks "
          "and %ld updates, not 37 and %d", s->new_tracks, s->updates, 3000 - 37);
    CHECK(t.total_reports == 3000 && t.total_tracks == 37 && track_count_entries(&t) == 37,
          "with ONE worker the picture must be exact: it holds %ld reports and %ld tracks "
          "(%ld walked), not 3000 and 37", t.total_reports, t.total_tracks,
          track_count_entries(&t));
    CHECK(pool_total_late(p) == 0, "with no deadline, %ld reports were late",
          pool_total_late(p));
    pool_destroy(p);
    queue_destroy(&q);
    track_destroy(&t);
}

static void p_stop_joins(void)
{
    one_processor();
    queue_t q;
    track_t t;
    CHECK(queue_init(&q, 1000) == 0 && track_init(&t) == 0, "the test cannot start");
    fill(&q, 600, 600);
    int base = thread_count();
    g_gate_open = 0;
    pool_t *p = pool_create_deadline(1, &q, &t, 1000, 0, NULL);
    CHECK(p != NULL, "pool_create_deadline(1, ..., 1000 us) failed: %s", strerror(errno));
    p->stop = 1;
    g_gate_open = 1;
    pool_stop(p);          /* the drain is 600 ms of work */
    long done = pool_total_processed(p);
    CHECK(done == 600, "pool_stop returned when %ld of 600 reports were processed. It must "
          "call pthread_join for each thread, and the worker returns after the drain", done);
    /* A joined thread can stay in /proc for a moment after pthread_join. */
    CHECK(threads_become(base, 100), "100 ms after pool_stop, %d worker threads still ran. "
          "pool_stop must call pthread_join for each thread", thread_count() - base);
    pool_destroy(p);
    queue_destroy(&q);
    track_destroy(&t);
}

static void p_log(void)
{
    char   logp[128];
    log_t *lg = test_log(logp, sizeof logp);
    queue_t q;
    track_t t;
    CHECK(queue_init(&q, 64) == 0 && track_init(&t) == 0, "the test cannot start");
    fill(&q, 20, 5);
    g_gate_open = 0;
    pool_t *p = pool_create_deadline(1, &q, &t, 50, 3000, lg);
    CHECK(p != NULL, "pool_create_deadline failed: %s", strerror(errno));
    pool_set_log_steps(p, 1);
    p->stop = 1;
    g_gate_open = 1;
    pool_stop(p);
    pool_destroy(p);
    log_close(lg);
    CHECK(file_has_line(logp, "SYSTEM", "work calibrated:"), "the mission log has no line "
          "\"work calibrated\": call pool_calibrate in pool_create_deadline");
    CHECK(file_has_line(logp, "SYSTEM", "worker pool started: 1 workers, 50 us, deadline "
          "3000 ms"), "the mission log has no SYSTEM line \"worker pool started: 1 workers, "
          "50 us, deadline 3000 ms\"");
    CHECK(file_has_line(logp, "SYSTEM", "worker pool stopped: 20 processed"), "the mission log "
          "has no SYSTEM line \"worker pool stopped: 20 processed\"");
    char *log = get_file(logp);
    CHECK(log != NULL, "the test cannot read %s", logp);
    int started = count_of(log, "(draining)");
    int created = count_of(log, " created from ");
    int updated = count_of(log, " updated from ");
    free(log);
    CHECK(started == 20, "the log has %d lines \"worker 0 started ... (draining)\", not 20. "
          "Call pool_report_begin for each report, with draining 1 after the stop", started);
    CHECK(created == 5 && updated == 15, "the log has %d \"created\" and %d \"updated\" "
          "lines, not 5 and 15. Call pool_report_end with the result of track_update",
          created, updated);
    queue_destroy(&q);
    track_destroy(&t);
}

static void p_errors(void)
{
    queue_t q;
    track_t t;
    CHECK(queue_init(&q, 8) == 0 && track_init(&t) == 0, "the test cannot start");
    int base = thread_count();
    errno = 0;
    CHECK(pool_create_deadline(0, &q, &t, 0, 0, NULL) == NULL && errno == EINVAL,
          "pool_create_deadline(0 workers) must give NULL with EINVAL");
    errno = 0;
    CHECK(pool_create_deadline(2, NULL, &t, 0, 0, NULL) == NULL && errno == EINVAL,
          "pool_create_deadline with q NULL must give NULL with EINVAL");
    errno = 0;
    CHECK(pool_create_deadline(2, &q, NULL, 0, 0, NULL) == NULL && errno == EINVAL,
          "pool_create_deadline with t NULL must give NULL with EINVAL");
    CHECK(thread_count() == base, "a call that gave EINVAL started a thread");
    pool_stop(NULL);
    pool_destroy(NULL);
    CHECK(pool_total_processed(NULL) == 0 && pool_total_late(NULL) == 0,
          "pool_total_processed(NULL) and pool_total_late(NULL) must give 0");
    queue_destroy(&q);
    track_destroy(&t);
}

static void p_create_fails(void)
{
    queue_t q;
    track_t t;
    CHECK(queue_init(&q, 8) == 0 && track_init(&t) == 0, "the test cannot start");
    int base = thread_count();
    g_calls   = 0;
    g_fail_at = 3;
    errno = 0;
    pool_t *p = pool_create_deadline(5, &q, &t, 0, 0, NULL);
    int     e = errno;
    g_fail_at = 0;
    CHECK(p == NULL, "the third pthread_create failed with EAGAIN, and pool_create_deadline "
          "did not give NULL");
    CHECK(e == EAGAIN, "after a failed pthread_create, errno is %s, not the value that "
          "pthread_create returned (EAGAIN)", strerror(e));
    CHECK(threads_become(base, 2000), "after the failed pool_create_deadline, %d threads of "
          "the pool still run. Set stop to 1 and join the threads that started",
          thread_count() - base);
    queue_destroy(&q);
    track_destroy(&t);
}

/* ============================================================= demo_lab3 */

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
        execl(g_demo, g_demo, "--workers", "1", "--ms", "2000", "--log", logp,
              "--feed", g_feedtool, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    CHECK(!(WIFEXITED(st) && WEXITSTATUS(st) == 127), "%s does not run. Run make first",
          g_demo);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "demo_lab3 exited with status %d, not 0. "
          "Its output is in %s", WIFEXITED(st) ? WEXITSTATUS(st) : -1, outp);

    char *out = get_file(outp);
    CHECK(out != NULL, "demo_lab3 wrote no output");
    CHECK(strstr(out, "sensors started 4 of 4") != NULL, "the demo did not start 4 sensors");
    CHECK(strstr(out, "BALANCED") != NULL, "with 1 worker, the loss ledger does not balance. "
          "The output is in %s", outp);
    CHECK(strstr(out, "shared data agrees") != NULL, "with 1 worker, the shared data must "
          "agree. The output is in %s", outp);
    free(out);

    char *log = get_file(logp);
    CHECK(log != NULL, "demo_lab3 wrote no mission log at %s", logp);
    CHECK(strstr(log, "worker pool stopped:") != NULL, "the mission log has no line "
          "\"worker pool stopped\"");
    CHECK(strstr(log, "Part 3 complete:") != NULL, "the mission log has no line "
          "\"SENTINEL Part 3 complete\"");
    free(log);
}

/* ================================================================ runner */

struct check {
    const char *group;
    const char *name;
    void      (*fn)(void);
};

static const struct check kChecks[] = {
    { "queue.c", "queue_init gives an empty queue, and queue_destroy frees it", q_init_empty },
    { "queue.c", "queue_pop gives the reports in the order of arrival, and count follows", q_fifo_order },
    { "queue.c", "head and tail go back to slot 0: the ring wraps", q_wraps },
    { "queue.c", "a full queue gives ENOSPC, counts it, and keeps its reports", q_full },
    { "queue.c", "pushed, popped and peak", q_stats },
    { "queue.c", "queue_init, queue_push and queue_pop give EINVAL", q_errors },
    { "pool.c", "pool_create_deadline starts N threads, and pool_stop ends them", p_threads },
    { "pool.c", "each worker gets its own argument block", p_own_block },
    { "pool.c", "one worker applies each report: the picture is exact", p_one_worker_exact },
    { "pool.c", "pool_stop returns after each worker returned", p_stop_joins },
    { "pool.c", "the SYSTEM lines, and a step line for each report", p_log },
    { "pool.c", "pool_create_deadline gives EINVAL, and NULL is safe", p_errors },
    { "pool.c", "a failed pthread_create: NULL, its errno, and no thread left", p_create_fails },
    { "demo_lab3", "the demo with 1 worker runs, and its ledger balances", demo_end_to_end },
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

    snprintf(g_dir, sizeof g_dir, "/tmp/sentinel_accept3_XXXXXX");
    if (mkdtemp(g_dir) == NULL) {
        fprintf(stderr, "cannot create a directory in /tmp: %s\n", strerror(errno));
        return 2;
    }

    /* A sensor whose reader has gone must not end the test with SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);

    printf("SENTINEL Lab 3 acceptance\n");

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
                fprintf(stderr, "accept_lab3: more than %d check groups. "
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
           "Section 7 of docs/lab3/lab3_brief.pdf.\n");
    return passed == NCHECKS ? 0 : 1;
}
