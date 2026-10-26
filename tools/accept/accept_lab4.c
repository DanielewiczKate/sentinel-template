/* accept_lab4 — the acceptance test of Lab 4.
 *
 *   accept_lab4 [--sentinel PATH] [--contactfeed PATH]
 *
 * `make accept4` compiles this file with your build/libsync.a and the
 * libraries below it. So each call to a safe_track_*, sem_queue_* or
 * resources_* function below runs your code, or the given code of
 * src/libsync/given/. Then the test runs build/sentinel, the final program.
 * It prints PASS or FAIL for each of 16 checks, then the share that passed
 * for each file.
 *
 * Read a check to see what it does. Do not change this file. The marks come
 * from the course copy of it, and Section 7 of docs/lab4/lab4_brief.pdf
 * gives them.
 *
 * How the test works:
 *
 *   - Each check runs in its own child process, in its own process group,
 *     with a time limit. A crash, or a call that does not return (a
 *     deadlock), stops that check only.
 *
 *   - Most checks need no luck. To see that a function takes a lock, the
 *     main thread HOLDS that lock first, and a second thread calls the
 *     function: with the lock, the second thread waits until the main thread
 *     releases it. To see that a pop waits, the queue is empty; to see that
 *     a push waits, the queue is full. A wait of 300 ms with no result is
 *     the proof.
 *
 *   - The checks with many threads need two processors or more to find a
 *     missing lock. On one processor they pass more often than they should;
 *     the checks with a held lock find it on each machine.
 *
 *   - The test uses no assert(). A failed check prints a sentence about the
 *     contract, not a line number in this file.
 */

#include "sentinel/sync.h"
#include "contact.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
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
#define WAIT_MS   300          /* how long a thread must wait to prove a wait */

static char        g_dir[64];          /* the temporary directory of this run */
static const char *g_sentinel = "build/sentinel";
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

static void sleep_ms(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {
    }
}

static contact_t contact(const char *id)
{
    contact_t c;
    memset(&c, 0, sizeof c);
    snprintf(c.id, sizeof c.id, "%s", id);
    snprintf(c.sensor, sizeof c.sensor, "SAT");
    return c;
}

static pending_t pend(const char *id)
{
    pending_t p;
    memset(&p, 0, sizeof p);
    p.c = contact(id);
    return p;
}

/* ---- a second thread that calls one function --------------------------- */

/* The main thread starts a helper, and reads `done` to see if the call of
   the helper returned. */
typedef struct {
    int           what;      /* which call: see helper() */
    safe_track_t *st;
    sem_queue_t  *sq;
    resources_t  *res;
    pending_t     item;
    int           rc;
    atomic_int    done;
} helper_t;

enum { H_UPDATE, H_PUSH, H_POP, H_ACQUIRE_10 };

static void *helper(void *arg)
{
    helper_t *h = arg;
    switch (h->what) {
    case H_UPDATE: {
        contact_t c = contact("WAIT-1");
        h->rc = safe_track_update(h->st, &c);
        break;
    }
    case H_PUSH:
        h->rc = sem_queue_push(h->sq, &h->item);
        break;
    case H_POP:
        h->rc = sem_queue_pop(h->sq, &h->item);
        break;
    case H_ACQUIRE_10:
        h->rc = resources_acquire_both(h->res, RESOURCE_HQ_DATALINK,
                                       RESOURCE_TRACKING_SENSOR);
        break;
    }
    atomic_store(&h->done, 1);
    return NULL;
}

static pthread_t start_helper(helper_t *h)
{
    pthread_t t;
    atomic_init(&h->done, 0);
    CHECK(pthread_create(&t, NULL, helper, h) == 0, "the test cannot start a thread");
    return t;
}

/* 1 when the helper finished within ms milliseconds. */
static int finished_within(helper_t *h, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        if (atomic_load(&h->done)) {
            return 1;
        }
        sleep_ms(10);
    }
    return atomic_load(&h->done);
}

/* ========================================================== safetrack.c */

static void s_init(void)
{
    safe_track_t st;
    CHECK(safe_track_init(&st) == 0, "safe_track_init gave -1: %s", strerror(errno));
    CHECK(safe_track_total_reports(&st) == 0 && safe_track_total_tracks(&st) == 0,
          "a new picture must have 0 reports and 0 tracks");
    CHECK(pthread_mutex_trylock(&st.lock) == 0, "after safe_track_init, the mutex is not "
          "free: call pthread_mutex_init(&st->lock, NULL)");
    pthread_mutex_unlock(&st.lock);
    safe_track_destroy(&st);
    safe_track_destroy(NULL);
}

static void s_one_thread(void)
{
    safe_track_t st;
    CHECK(safe_track_init(&st) == 0, "safe_track_init failed");
    contact_t a = contact("A-1");
    contact_t b = contact("B-2");
    CHECK(safe_track_update(&st, &a) == 1, "a new id must give 1");
    CHECK(safe_track_update(&st, &a) == 0, "the same id again must give 0");
    CHECK(safe_track_update(&st, &b) == 1, "a second new id must give 1");
    CHECK(safe_track_total_reports(&st) == 3, "3 reports, but total_reports is %ld",
          safe_track_total_reports(&st));
    CHECK(safe_track_total_tracks(&st) == 2, "2 ids, but total_tracks is %ld",
          safe_track_total_tracks(&st));
    safe_track_destroy(&st);
}

static void s_takes_lock(void)
{
    safe_track_t st;
    CHECK(safe_track_init(&st) == 0, "safe_track_init failed");

    pthread_mutex_lock(&st.lock);
    helper_t  h = { .what = H_UPDATE, .st = &st };
    pthread_t t = start_helper(&h);
    int early = finished_within(&h, WAIT_MS);
    pthread_mutex_unlock(&st.lock);
    pthread_join(t, NULL);

    CHECK(!early, "safe_track_update returned while another thread held st->lock. Take the "
          "lock with pthread_mutex_lock before track_update");
    CHECK(h.rc == 1, "after the lock was free, safe_track_update gave %d, not 1", h.rc);
    safe_track_destroy(&st);
}

static void s_releases_lock(void)
{
    safe_track_t st;
    CHECK(safe_track_init(&st) == 0, "safe_track_init failed");
    contact_t a = contact("A-1");
    CHECK(safe_track_update(&st, &a) == 1, "a new id must give 1");
    CHECK(pthread_mutex_trylock(&st.lock) == 0, "after safe_track_update returned, st->lock is "
          "still held: call pthread_mutex_unlock after track_update");
    pthread_mutex_unlock(&st.lock);

    errno = 0;
    CHECK(safe_track_update(&st, NULL) == -1 && errno == EINVAL,
          "safe_track_update(st, NULL) must give -1 with EINVAL");
    CHECK(pthread_mutex_trylock(&st.lock) == 0, "after safe_track_update gave EINVAL, st->lock "
          "is held: check the arguments before the lock");
    pthread_mutex_unlock(&st.lock);
    safe_track_destroy(&st);
}

#define S_THREADS 8
#define S_EACH    20000
#define S_IDS     37

static void *s_worker(void *arg)
{
    safe_track_t *st = arg;
    char id[16];
    for (int i = 0; i < S_EACH; i++) {
        snprintf(id, sizeof id, "T-%d", i % S_IDS);
        contact_t c = contact(id);
        safe_track_update(st, &c);
    }
    return NULL;
}

static void s_threads_exact(void)
{
    safe_track_t st;
    CHECK(safe_track_init(&st) == 0, "safe_track_init failed");
    pthread_t t[S_THREADS];
    for (int i = 0; i < S_THREADS; i++) {
        CHECK(pthread_create(&t[i], NULL, s_worker, &st) == 0, "the test cannot start a thread");
    }
    for (int i = 0; i < S_THREADS; i++) {
        pthread_join(t[i], NULL);
    }
    long want = (long)S_THREADS * S_EACH;
    CHECK(safe_track_total_reports(&st) == want, "%d threads put %ld reports in, and "
          "total_reports is %ld: updates were lost. The lock must cover the whole call of "
          "track_update", S_THREADS, want, safe_track_total_reports(&st));
    CHECK(safe_track_total_tracks(&st) == S_IDS && safe_track_count_entries(&st) == S_IDS,
          "%d ids, but total_tracks is %ld and the walk finds %ld", S_IDS,
          safe_track_total_tracks(&st), safe_track_count_entries(&st));
    safe_track_destroy(&st);
}

static void s_errors(void)
{
    errno = 0;
    CHECK(safe_track_init(NULL) == -1 && errno == EINVAL,
          "safe_track_init(NULL) must give -1 with EINVAL");
    safe_track_t st;
    CHECK(safe_track_init(&st) == 0, "safe_track_init failed");
    contact_t a = contact("A-1");
    errno = 0;
    CHECK(safe_track_update(NULL, &a) == -1 && errno == EINVAL,
          "safe_track_update(NULL, c) must give -1 with EINVAL");
    safe_track_destroy(&st);
}

/* ========================================================== sem_queue.c */

static void q_init_values(void)
{
    sem_queue_t sq;
    CHECK(sem_queue_init(&sq, 5) == 0, "sem_queue_init gave -1: %s", strerror(errno));
    int empty = -1, full = -1;
    sem_getvalue(&sq.empty, &empty);
    sem_getvalue(&sq.full, &full);
    CHECK(empty == 5, "after sem_queue_init(sq, 5), empty is %d, not 5: each slot is free",
          empty);
    CHECK(full == 0, "after sem_queue_init, full is %d, not 0: no report waits", full);
    CHECK(sq.q.capacity == 5 && queue_count(&sq.q) == 0, "the ring of the queue must have 5 "
          "slots and no report: call queue_init(&sq->q, capacity)");
    CHECK(pthread_mutex_trylock(&sq.lock) == 0, "after sem_queue_init, the mutex is not free");
    pthread_mutex_unlock(&sq.lock);
    sem_queue_destroy(&sq);
    sem_queue_destroy(NULL);
}

static void q_order(void)
{
    sem_queue_t sq;
    CHECK(sem_queue_init(&sq, 4) == 0, "sem_queue_init failed");
    const char *ids[] = { "A", "B", "C" };
    for (int i = 0; i < 3; i++) {
        pending_t p = pend(ids[i]);
        CHECK(sem_queue_push(&sq, &p) == 0, "sem_queue_push gave -1 on a queue with room");
    }
    CHECK(sem_queue_count(&sq) == 3, "3 pushes, but the count is %zu", sem_queue_count(&sq));
    for (int i = 0; i < 3; i++) {
        pending_t out;
        memset(&out, 0, sizeof out);
        CHECK(sem_queue_pop(&sq, &out) == 1, "sem_queue_pop must give 1 while reports wait");
        CHECK(strcmp(out.c.id, ids[i]) == 0, "pop %d gave %s, not %s: the order of arrival",
              i + 1, out.c.id, ids[i]);
    }
    int empty = -1, full = -1;
    sem_getvalue(&sq.empty, &empty);
    sem_getvalue(&sq.full, &full);
    CHECK(empty == 4 && full == 0, "after 3 pushes and 3 pops, empty is %d and full is %d, not "
          "4 and 0: a push posts full, and a pop posts empty", empty, full);
    sem_queue_destroy(&sq);
}

static void q_pop_waits(void)
{
    sem_queue_t sq;
    CHECK(sem_queue_init(&sq, 4) == 0, "sem_queue_init failed");

    helper_t  h = { .what = H_POP, .sq = &sq };
    pthread_t t = start_helper(&h);
    int early = finished_within(&h, WAIT_MS);
    CHECK(!early, "sem_queue_pop returned %d on an empty queue. It must wait in "
          "sem_wait(&sq->full) until a report arrives", h.rc);

    pending_t p = pend("LATE");
    CHECK(sem_queue_push(&sq, &p) == 0, "sem_queue_push failed");
    int done = finished_within(&h, 5000);
    CHECK(done, "a report was pushed, and the waiting sem_queue_pop did not wake: "
          "sem_queue_push must call sem_post(&sq->full)");
    pthread_join(t, NULL);
    CHECK(h.rc == 1 && strcmp(h.item.c.id, "LATE") == 0,
          "the waiting pop must give 1 and the report LATE");
    sem_queue_destroy(&sq);
}

static void q_push_waits(void)
{
    sem_queue_t sq;
    CHECK(sem_queue_init(&sq, 2) == 0, "sem_queue_init failed");
    pending_t a = pend("A");
    pending_t b = pend("B");
    CHECK(sem_queue_push(&sq, &a) == 0 && sem_queue_push(&sq, &b) == 0,
          "two pushes into a queue of 2 slots must give 0");

    helper_t  h = { .what = H_PUSH, .sq = &sq, .item = pend("C") };
    pthread_t t = start_helper(&h);
    int early = finished_within(&h, WAIT_MS);
    CHECK(!early, "a push into a full queue returned %d. It must wait for a free slot "
          "(empty is 0): call sem_init(&sq->empty, 0, capacity)", h.rc);

    pending_t out;
    CHECK(sem_queue_pop(&sq, &out) == 1, "sem_queue_pop failed on a full queue");
    int done = finished_within(&h, 5000);
    CHECK(done, "a report was popped, and the waiting push did not wake: sem_queue_pop must "
          "call sem_post(&sq->empty)");
    pthread_join(t, NULL);
    CHECK(h.rc == 0, "the waiting push must give 0");
    CHECK(sem_queue_count(&sq) == 2, "2 reports must wait, and the count is %zu",
          sem_queue_count(&sq));
    sem_queue_destroy(&sq);
}

#define Q_PRODUCERS 4
#define Q_CONSUMERS 4
#define Q_EACH      5000

typedef struct {
    sem_queue_t *sq;
    int          index;
    int         *seen;      /* one counter for each report, for the consumers */
} q_arg_t;

static void *q_producer(void *arg)
{
    q_arg_t *a = arg;
    char id[16];
    for (int i = 0; i < Q_EACH; i++) {
        snprintf(id, sizeof id, "%d", a->index * Q_EACH + i);
        pending_t p = pend(id);
        sem_queue_push(a->sq, &p);
    }
    return NULL;
}

static void *q_consumer(void *arg)
{
    q_arg_t *a = arg;
    for (int i = 0; i < Q_EACH; i++) {
        pending_t out;
        if (sem_queue_pop(a->sq, &out) != 1) {
            break;
        }
        int n = atoi(out.c.id);
        if (n >= 0 && n < Q_PRODUCERS * Q_EACH) {
            __atomic_fetch_add(&a->seen[n], 1, __ATOMIC_RELAXED);
        }
    }
    return NULL;
}

static void q_threads_exact(void)
{
    sem_queue_t sq;
    CHECK(sem_queue_init(&sq, 64) == 0, "sem_queue_init failed");
    int *seen = calloc(Q_PRODUCERS * Q_EACH, sizeof *seen);
    CHECK(seen != NULL, "the test is out of memory");

    pthread_t t[Q_PRODUCERS + Q_CONSUMERS];
    q_arg_t   a[Q_PRODUCERS + Q_CONSUMERS];
    for (int i = 0; i < Q_PRODUCERS + Q_CONSUMERS; i++) {
        a[i] = (q_arg_t){ &sq, i, seen };
        void *(*fn)(void *) = i < Q_PRODUCERS ? q_producer : q_consumer;
        CHECK(pthread_create(&t[i], NULL, fn, &a[i]) == 0, "the test cannot start a thread");
    }
    for (int i = 0; i < Q_PRODUCERS + Q_CONSUMERS; i++) {
        pthread_join(t[i], NULL);
    }

    int missing = 0, twice = 0;
    for (int n = 0; n < Q_PRODUCERS * Q_EACH; n++) {
        missing += (seen[n] == 0);
        twice   += (seen[n] > 1);
    }
    CHECK(missing == 0 && twice == 0, "%d producers and %d consumers moved %d reports: %d were "
          "lost and %d were taken two times. The lock must cover each call of queue_push and "
          "queue_pop", Q_PRODUCERS, Q_CONSUMERS, Q_PRODUCERS * Q_EACH, missing, twice);
    CHECK(sem_queue_count(&sq) == 0, "after all the pops, %zu reports still wait",
          sem_queue_count(&sq));
    free(seen);
    sem_queue_destroy(&sq);
}

static void q_errors(void)
{
    errno = 0;
    CHECK(sem_queue_init(NULL, 4) == -1 && errno == EINVAL,
          "sem_queue_init(NULL, 4) must give -1 with EINVAL");
    sem_queue_t sq;
    errno = 0;
    CHECK(sem_queue_init(&sq, 0) == -1 && errno == EINVAL,
          "sem_queue_init(sq, 0) must give -1 with EINVAL");
    CHECK(sem_queue_init(&sq, 4) == 0, "sem_queue_init failed");
    pending_t p = pend("A");
    errno = 0;
    CHECK(sem_queue_push(NULL, &p) == -1 && errno == EINVAL,
          "sem_queue_push(NULL, p) must give -1 with EINVAL");
    errno = 0;
    CHECK(sem_queue_push(&sq, NULL) == -1 && errno == EINVAL,
          "sem_queue_push(sq, NULL) must give -1 with EINVAL");
    errno = 0;
    CHECK(sem_queue_pop(&sq, NULL) == -1 && errno == EINVAL,
          "sem_queue_pop(sq, NULL) must give -1 with EINVAL");
    int empty = -1;
    sem_getvalue(&sq.empty, &empty);
    CHECK(empty == 4, "a call that gave EINVAL changed empty to %d: check the arguments "
          "before sem_wait", empty);
    sem_queue_destroy(&sq);
}

/* ========================================================== resources.c */

static void r_order(void)
{
    resources_t r;
    CHECK(resources_init(&r) == 0, "resources_init failed");

    /* Hold rank 0. A helper asks for (1, 0). With the order, it takes rank 0
       first, so it waits on rank 0 and never touches rank 1. With no order,
       it takes rank 1, then waits on rank 0: rank 1 is then held. */
    pthread_mutex_lock(&r.locks[RESOURCE_TRACKING_SENSOR]);
    helper_t  h = { .what = H_ACQUIRE_10, .res = &r };
    pthread_t t = start_helper(&h);
    int early = finished_within(&h, WAIT_MS);
    int rank1_free = pthread_mutex_trylock(&r.locks[RESOURCE_HQ_DATALINK]) == 0;
    if (rank1_free) {
        pthread_mutex_unlock(&r.locks[RESOURCE_HQ_DATALINK]);
    }
    pthread_mutex_unlock(&r.locks[RESOURCE_TRACKING_SENSOR]);

    CHECK(!early, "resources_acquire_both returned while another thread held the tracking "
          "sensor: it must lock both");
    CHECK(rank1_free, "the caller named (HQ data link, tracking sensor), and "
          "resources_acquire_both took the HQ data link FIRST. Take the lower rank first, "
          "whatever order the caller names: the tracking sensor (0), then the HQ data link (1)");
    int done = finished_within(&h, 5000);
    CHECK(done, "after the tracking sensor was free, resources_acquire_both did not return");
    pthread_join(t, NULL);
    CHECK(h.rc == 0, "resources_acquire_both must give 0");
    resources_release_both(&r, RESOURCE_HQ_DATALINK, RESOURCE_TRACKING_SENSOR);
    resources_destroy(&r);
}

static void r_release(void)
{
    resources_t r;
    CHECK(resources_init(&r) == 0, "resources_init failed");
    CHECK(resources_acquire_both(&r, RESOURCE_TRACKING_SENSOR, RESOURCE_HQ_DATALINK) == 0,
          "resources_acquire_both must give 0");
    CHECK(r.acquisitions[0] == 1 && r.acquisitions[1] == 1, "after one acquire, each "
          "acquisitions count must be 1, not %ld and %ld", r.acquisitions[0], r.acquisitions[1]);
    resources_release_both(&r, RESOURCE_TRACKING_SENSOR, RESOURCE_HQ_DATALINK);
    for (int i = 0; i < RESOURCE_COUNT; i++) {
        CHECK(pthread_mutex_trylock(&r.locks[i]) == 0, "after resources_release_both, the %s "
              "is still held", resource_name((resource_id_t)i));
        pthread_mutex_unlock(&r.locks[i]);
    }
    resources_destroy(&r);
}

static void r_errors(void)
{
    resources_t r;
    CHECK(resources_init(&r) == 0, "resources_init failed");
    errno = 0;
    CHECK(resources_acquire_both(NULL, RESOURCE_TRACKING_SENSOR, RESOURCE_HQ_DATALINK) == -1 &&
          errno == EINVAL, "resources_acquire_both(NULL, ...) must give -1 with EINVAL");
    errno = 0;
    CHECK(resources_acquire_both(&r, RESOURCE_HQ_DATALINK, RESOURCE_HQ_DATALINK) == -1 &&
          errno == EINVAL, "resources_acquire_both(r, a, a) must give -1 with EINVAL");
    errno = 0;
    CHECK(resources_acquire_both(&r, RESOURCE_TRACKING_SENSOR, (resource_id_t)RESOURCE_COUNT)
          == -1 && errno == EINVAL, "a resource out of range must give -1 with EINVAL");
    for (int i = 0; i < RESOURCE_COUNT; i++) {
        CHECK(pthread_mutex_trylock(&r.locks[i]) == 0, "a call that gave EINVAL left the %s "
              "held", resource_name((resource_id_t)i));
        pthread_mutex_unlock(&r.locks[i]);
    }
    resources_release_both(NULL, RESOURCE_TRACKING_SENSOR, RESOURCE_HQ_DATALINK);
    resources_destroy(&r);
}

/* ============================================================= sentinel */

static void sentinel_end_to_end(void)
{
    char logp[128], outp[128];
    path_of(logp, sizeof logp, "sentinel.log");
    path_of(outp, sizeof outp, "sentinel.out");
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
        execl(g_sentinel, g_sentinel, "--workers", "8", "--cpus", "all", "--ms", "3000",
              "--log", logp, "--feed", g_feedtool, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    CHECK(!(WIFEXITED(st) && WEXITSTATUS(st) == 127), "%s does not run. Run make first",
          g_sentinel);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "sentinel exited with status %d, not 0. "
          "Its output is in %s", WIFEXITED(st) ? WEXITSTATUS(st) : -1, outp);

    char *out = get_file(outp);
    CHECK(out != NULL, "sentinel wrote no output");
    CHECK(strstr(out, "BALANCED") != NULL, "with 8 workers, the loss ledger does not balance. "
          "The output is in %s", outp);
    const char *lines[] = { "R4 shared data stays exact", "R5 never freezes" };
    for (int i = 0; i < 2; i++) {
        const char *at = strstr(out, lines[i]);
        CHECK(at != NULL, "the output has no line \"%s\"", lines[i]);
        const char *end = strchr(at, '\n');
        size_t n = end ? (size_t)(end - at) : strlen(at);
        CHECK(memmem(at, n, "PASS", 4) != NULL, "with 8 workers and all processors, the line "
              "\"%.*s\" is not PASS", (int)n, at);
    }
    free(out);
}

/* ================================================================ runner */

struct check {
    const char *group;
    const char *name;
    void      (*fn)(void);
};

static const struct check kChecks[] = {
    { "safetrack.c", "safe_track_init gives an empty picture and a free mutex", s_init },
    { "safetrack.c", "one thread: a new id gives 1, the same id gives 0", s_one_thread },
    { "safetrack.c", "safe_track_update waits while another thread holds the lock", s_takes_lock },
    { "safetrack.c", "safe_track_update releases the lock, also after EINVAL", s_releases_lock },
    { "safetrack.c", "8 threads: no update is lost", s_threads_exact },
    { "safetrack.c", "safe_track_init and safe_track_update give EINVAL", s_errors },
    { "sem_queue.c", "sem_queue_init: empty is capacity, full is 0", q_init_values },
    { "sem_queue.c", "the reports come out in the order of arrival", q_order },
    { "sem_queue.c", "a pop on an empty queue waits until a push", q_pop_waits },
    { "sem_queue.c", "a push on a full queue waits until a pop", q_push_waits },
    { "sem_queue.c", "4 producers and 4 consumers: no report lost or taken two times", q_threads_exact },
    { "sem_queue.c", "sem_queue_init, push and pop give EINVAL", q_errors },
    { "resources.c", "resources_acquire_both takes the lower rank first", r_order },
    { "resources.c", "resources_release_both releases both", r_release },
    { "resources.c", "resources_acquire_both gives EINVAL", r_errors },
    { "sentinel", "8 workers with all processors: R4 and R5 pass", sentinel_end_to_end },
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
            printf("        no result after %d s: a call did not return (a deadlock, or a "
                   "lock that was never released)\n", TIMEOUT_S);
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
        if (strcmp(argv[i], "--sentinel") == 0 && i + 1 < argc) {
            g_sentinel = argv[++i];
        } else if (strcmp(argv[i], "--contactfeed") == 0 && i + 1 < argc) {
            snprintf(g_feedtool, sizeof g_feedtool, "%s", argv[++i]);
        } else {
            fprintf(stderr, "usage: %s [--sentinel PATH] [--contactfeed PATH]\n", argv[0]);
            return 2;
        }
    }

    snprintf(g_dir, sizeof g_dir, "/tmp/sentinel_accept4_XXXXXX");
    if (mkdtemp(g_dir) == NULL) {
        fprintf(stderr, "cannot create a directory in /tmp: %s\n", strerror(errno));
        return 2;
    }

    /* A sensor whose reader has gone must not end the test with SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);

    printf("SENTINEL Lab 4 acceptance\n");

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
                fprintf(stderr, "accept_lab4: more than %d check groups. "
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
           "Section 7 of docs/lab4/lab4_brief.pdf.\n");
    return passed == NCHECKS ? 0 : 1;
}
