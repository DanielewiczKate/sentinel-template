/* sync.h — libsync, the protection of the shared data. Lab 4.
 *
 * Lab 3 showed the problem: with no lock, many workers lose updates of the
 * track picture, take one report two times, and drive the count of the
 * queue below 0, so the stop never ends. Lab 4 solves it with the three
 * tools of the lectures:
 *
 *   a mutex          the update of the track picture is one step (R4)
 *   two semaphores   the queue waits with no use of the processor, and no
 *                    longer loses its count
 *   a lock order     two workers that need two locks cannot freeze (R5)
 *
 * libsync does not rewrite Lab 3. It puts the same queue and the same track
 * picture of Lab 3 inside the protection, so the run that disagreed in Lab 3
 * agrees in Lab 4, with the same code inside.
 *
 * WHO WRITES WHAT. You write the functions marked YOURS, in the file that
 * each section names: src/safetrack.c, src/sem_queue.c and src/resources.c.
 * Each one arrives as a stub, with its steps in a comment. The functions
 * marked GIVEN are in src/libsync/given/, or in the GIVEN part of your
 * file. You read them, and you do not change them. Do not change a
 * declaration in this file: the tests that mark your lab call each function
 * as it is declared here.
 *
 * A pthread function RETURNS its error number (0 is success), and does not
 * set errno. A sem_* function returns -1 and sets errno. Each function of
 * this file gives its errors in errno.
 *
 * CALLS THAT NEED NO CHECK in your three files, and why. They need no
 * branch and no comment:
 *   - pthread_mutex_lock and pthread_mutex_unlock of a mutex that
 *     pthread_mutex_init made, by the thread that holds it;
 *   - sem_post of empty or full: neither one can pass the capacity;
 *   - pthread_mutex_destroy, sem_destroy, track_destroy and queue_destroy in
 *     a destroy function: the caller joins each thread first.
 */

#ifndef SENTINEL_SYNC_H
#define SENTINEL_SYNC_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

#include "contact.h"
#include "sentinel/syscall.h"
#include "sentinel/ipc.h"

/* ============================================================ track picture */

/* The track picture of Lab 3, with one mutex around it (R4).
 *
 * One lock for the whole picture, not one for each bucket. The shared totals
 * cover each bucket, so a lock for each bucket needs one more lock for the
 * totals: a second lock, taken while the first is held, and two locks need a
 * fixed order (resources, below). One lock is correct and simple.
 */
typedef struct {
    track_t         track;     /* the track picture of Lab 3 */
    pthread_mutex_t lock;      /* held for each read and write of track */
    long            refused;   /* reports refused as impossible (GIVEN code) */
} safe_track_t;

/* ----------------------------------------------- YOURS, in src/safetrack.c */

/* Prepare an empty, protected picture.
 *
 * What it does:
 *     1. Set each field of *st to 0.
 *     2. track_init(&st->track) (GIVEN, Lab 3).
 *     3. pthread_mutex_init(&st->lock, NULL).
 * Output: 0.
 */
/* Errors:
 * st == NULL: -1 with EINVAL.
 * track_init fails: -1, with its errno.
 * pthread_mutex_init gives rc != 0: track_destroy(&st->track), then -1 with
 * errno set to rc.
 */
int  safe_track_init(safe_track_t *st);

/* Free the picture, and destroy the mutex. Call it after each worker is
 * joined: the destroy of a mutex that a thread holds is undefined behaviour.
 * st == NULL does nothing. */
void safe_track_destroy(safe_track_t *st);

/* Put one report into the picture, as ONE step for each other thread.
 *
 * What it does:
 *     1. pthread_mutex_lock(&st->lock).
 *     2. rc = track_update(&st->track, c) (GIVEN, Lab 3).
 *     3. pthread_mutex_unlock(&st->lock).
 *     4. Give rc.
 * The lock covers the whole call of track_update, and nothing more.
 *
 * Output: 1 when a new track was created, 0 when a track was updated.
 */
/* Errors:
 * st == NULL, or c == NULL: -1 with EINVAL, and no lock is taken.
 * track_update fails: -1, with its errno, and the lock is released.
 */
int  safe_track_update(safe_track_t *st, const contact_t *c);

/* ------------------------------------- GIVEN, in given/safetrack_check.c */

/* GIVEN. THE CHECKED PATH of the final program: validate the report, find
 * the track that it belongs to, and update that track, in ONE critical
 * section. Validation and association READ the picture to decide what the
 * update writes, so three short critical sections would decide on a picture
 * that another worker changed after the check.
 *
 * Gives 1 for a new track, 0 for an update, -1 for an error, and
 * -(2 + valid_err_t) for a report refused as impossible: a value below -1
 * means a refused report, and refused has 1 more. assoc_id gets the id of
 * the track that the report updated. */
int  safe_track_submit(safe_track_t *st, const contact_t *c,
                       const assoc_gate_t *gate, const valid_rules_t *rules,
                       char assoc_id[CONTACT_ID_MAX + 1]);

/* GIVEN. How many reports were refused as impossible. */
long safe_track_refused(safe_track_t *st);

/* GIVEN. Write the picture to path as JSON, for the page: one entry for each
 * track. It writes a temporary file and renames it, so the page reads the old
 * file or the new one, never a part. Gives 0, or -1 with errno set. */
int  safe_track_write_json(safe_track_t *st, const char *path);

/* GIVEN. The totals, each read under the lock: a read of a shared long while
 * another thread writes it is also a race. */
long safe_track_total_reports(safe_track_t *st);
long safe_track_total_tracks(safe_track_t *st);
long safe_track_count_entries(safe_track_t *st);

/* =================================================================== queue */

/* The queue of Lab 3, with a mutex and two counting semaphores: the bounded
 * buffer of Lecture 3.3 (prodCons.c), around the FIFO ring of Lab 3.
 *
 *   empty   the free slots. A producer waits on it; a consumer posts it.
 *   full    the reports that wait. A consumer waits on it; a producer
 *           posts it.
 *   lock    a mutex around each call of queue_push and queue_pop.
 *
 * A semaphore is not a mutex. It holds a NUMBER, and sem_wait waits while it
 * is 0, with no use of the processor: no usleep loop. The mutex is still
 * necessary, because two threads that pass the semaphores must not be in the
 * ring at the same time.
 *
 * THE ORDER IS FIXED: sem_wait first, then the lock. A thread that takes the
 * lock and then waits on a semaphore sleeps with the lock, and no other
 * thread can post the semaphore.
 *
 * A full queue of Lab 4 does not refuse a push, as Lab 3 did: the producer
 * WAITS for a free slot. Nothing is dropped, and the wait is counted in waits
 * and wait_ns: in SENTINEL the producer is the centre, and each wait is time
 * that it does not read its sensors.
 */
typedef struct {
    queue_t         q;         /* the ring of Lab 3: your queue.c */
    pthread_mutex_t lock;
    sem_t           empty;
    sem_t           full;

    /* GIVEN code only. _Atomic, not volatile: volatile stops the compiler
     * from keeping a value in a register, and that is all; two threads that
     * write and read it are still a race. */
    atomic_int      shutdown;   /* 1 after sem_queue_shutdown */
    atomic_long     waits;      /* pushes that had to wait for a free slot */
    atomic_long     wait_ns;    /* how long they waited, in total          */
} sem_queue_t;

/* ----------------------------------------------- YOURS, in src/sem_queue.c */

/* Prepare an empty queue of capacity slots.
 *
 * What it does:
 *     1. Set each field of *sq to 0.
 *     2. queue_init(&sq->q, capacity): your function of Lab 3.
 *     3. pthread_mutex_init(&sq->lock, NULL).
 *     4. sem_init(&sq->empty, 0, capacity): each slot is free.
 *     5. sem_init(&sq->full, 0, 0): no report waits.
 *        The 0 in the middle: the threads of ONE process share it.
 * Output: 0.
 */
/* Errors:
 * sq == NULL, or capacity == 0: -1 with EINVAL.
 * queue_init fails: -1, with its errno.
 * pthread_mutex_init gives rc != 0: queue_destroy, then -1 with errno set to
 * rc.
 * A sem_init fails: destroy what the steps before made (sem_destroy of empty
 * if it was made, pthread_mutex_destroy, queue_destroy), then -1 with the
 * errno of sem_init: save errno before the destroys, and put it back after.
 */
int  sem_queue_init(sem_queue_t *sq, size_t capacity);

/* queue_destroy, pthread_mutex_destroy, and sem_destroy of empty and full.
 * Call it after each thread is joined. sq == NULL does nothing. */
void sem_queue_destroy(sem_queue_t *sq);

/* Put a copy of *p in, and wait for a free slot when the queue is full.
 *
 * What it does:
 *     1. Wait for a free slot: wait_for_slot(sq) (GIVEN in src/sem_queue.c).
 *        It calls sem_wait(&sq->empty), and it counts the wait. When it
 *        gives -1, give -1.
 *     2. pthread_mutex_lock(&sq->lock).
 *     3. rc = queue_push(&sq->q, p): your function of Lab 3.
 *     4. pthread_mutex_unlock(&sq->lock).
 *     5. sem_post(&sq->full): one more report waits.
 *     6. Give rc.
 * queue_push cannot fail in step 3: step 1 gave this thread a free slot.
 *
 * Output: 0.
 */
/* Errors:
 * sq == NULL, or p == NULL: -1 with EINVAL.
 * The queue was shut down: -1.
 */
int  sem_queue_push(sem_queue_t *sq, const pending_t *p);

/* Take the report at the head, and wait while no report waits.
 *
 * What it does:
 *     1. sem_wait(&sq->full). When it gives -1 with EINTR, call it again (a
 *        signal is not an error, as read() in Lab 1). Another error: give -1.
 *     2. If is_shut_down(sq) (GIVEN in src/sem_queue.c), give
 *        pop_after_shutdown(sq, out) (GIVEN).
 *     3. pthread_mutex_lock(&sq->lock).
 *     4. got = queue_pop(&sq->q, out): your function of Lab 3.
 *     5. pthread_mutex_unlock(&sq->lock).
 *     6. sem_post(&sq->empty): one more slot is free.
 *     7. Give got.
 *
 * Output: 1 and *out, or 0 after a shutdown when the queue is empty.
 */
/* Errors:
 * sq == NULL, or out == NULL: -1 with EINVAL.
 */
int  sem_queue_pop(sem_queue_t *sq, pending_t *out);

/* -------------------------------------- GIVEN, in given/sem_queue_admin.c */

/* GIVEN. Wake each thread that waits, so that each one can end. Without it, a
 * worker that sleeps in sem_wait never returns, and the program cannot end. */
void sem_queue_shutdown(sem_queue_t *sq);

/* GIVEN. The reports that wait, read under the lock. */
size_t sem_queue_count(sem_queue_t *sq);

/* GIVEN. How often a push had to wait for a free slot, and for how long in
 * total. This is what Lab 4 pays in place of a dropped report. */
long   sem_queue_waits(const sem_queue_t *sq);
int64_t sem_queue_wait_ns(const sem_queue_t *sq);

/* =============================================================== resources */

/* The two resources that workers compete for, besides the track picture: the
 * tracking sensor and the data link to headquarters.
 *
 * A worker that needs both is where a deadlock comes from. Two workers that
 * take them in OPPOSITE orders can each hold one and wait for ever for the
 * other. The centre then freezes, while each thread is alive and the process
 * looks healthy (R5).
 *
 * The fix is ONE FIXED ORDER: each thread takes them by rank, the lower rank
 * first, whatever it needs them for. That removes the circular wait, the
 * fourth condition of a deadlock (Lecture 4.8).
 */
typedef enum {
    RESOURCE_TRACKING_SENSOR = 0,   /* rank 0: always taken first  */
    RESOURCE_HQ_DATALINK     = 1,   /* rank 1: always taken second */
    RESOURCE_COUNT
} resource_id_t;

typedef struct {
    pthread_mutex_t locks[RESOURCE_COUNT];
    long            acquisitions[RESOURCE_COUNT];
} resources_t;

/* ----------------------------------------------- YOURS, in src/resources.c */

/* Take both resources, in the fixed order, whatever order a and b name.
 *
 * What it does:
 *     1. first = the lower of a and b; second = the higher.
 *     2. pthread_mutex_lock(&r->locks[first]), then
 *        pthread_mutex_lock(&r->locks[second]).
 *     3. Add 1 to r->acquisitions[first] and r->acquisitions[second].
 * Output: 0, with both locks held.
 */
/* Errors:
 * r == NULL, a == b, or a or b is not from 0 to RESOURCE_COUNT - 1:
 * -1 with EINVAL, and no lock is taken.
 */
int  resources_acquire_both(resources_t *r, resource_id_t a, resource_id_t b);

/* Release both: pthread_mutex_unlock of the higher rank, then of the lower.
 * r == NULL or a == b does nothing. */
void resources_release_both(resources_t *r, resource_id_t a, resource_id_t b);

/* -------------------------------------- GIVEN, in given/resources_setup.c */

/* GIVEN. Make and destroy the two mutexes. resources_init gives 0, or -1 with
 * errno (EINVAL for r == NULL). */
int  resources_init(resources_t *r);
void resources_destroy(resources_t *r);

/* GIVEN. "tracking sensor" or "HQ data link", or NULL. */
const char *resource_name(resource_id_t id);

/* GIVEN. Take them in EXACTLY the order a, b, with no reorder. This is the
 * unsafe version: two threads with opposite orders can freeze. The course
 * tests use it to show the freeze that your order removes. */
int  resources_acquire_naive(resources_t *r, resource_id_t a, resource_id_t b);

/* ============================================ GIVEN, in given/celltrack.c */

/* GIVEN. THE PICTURE DIVIDED BY POSITION, WITH ONE LOCK FOR EACH CELL.
 *
 * With one lock, association holds the lock for a scan of each track, and 8
 * workers do less than 1. The map is divided into 8 x 8 cells, and workers on
 * opposite sides of the map no longer wait for each other.
 *
 * Here the deadlock of Lab 4 is real. A plot near the border of two cells
 * can belong to a track on either side, so it holds BOTH cells while it
 * decides. Two plots near one border, from opposite sides, ask for the same
 * two locks in opposite orders. The fix is the order of resources.c: take
 * the cells by rank, the lowest index first.
 */
#define CELL_ROWS  8
#define CELL_COLS  8
#define CELL_COUNT (CELL_ROWS * CELL_COLS)

typedef struct {
    track_t         track;
    pthread_mutex_t lock;
    long            contended;
} cell_t;

typedef struct {
    cell_t      cells[CELL_COUNT];

    /* Atomics, not a mutex: one global lock in the same path would make the
     * 64 cells wait for each other again. */
    atomic_long refused;
    atomic_long two_cell_plots;       /* plots that needed more than one cell */
} cell_track_t;

int  cell_track_init(cell_track_t *ct);
void cell_track_destroy(cell_track_t *ct);

/* Validate, associate and update, with each cell that the gate can reach
 * held. The return values of safe_track_submit. */
int  cell_track_submit(cell_track_t *ct, const contact_t *c,
                       const assoc_gate_t *gate, const valid_rules_t *rules,
                       char assoc_id[CONTACT_ID_MAX + 1]);

/* The same, but it takes the cells in the order that it finds them, and not
 * by rank: two plots near one border from opposite sides can freeze here. */
int  cell_track_submit_naive(cell_track_t *ct, const contact_t *c,
                             const assoc_gate_t *gate,
                             const valid_rules_t *rules,
                             char assoc_id[CONTACT_ID_MAX + 1]);

long cell_track_total_reports(cell_track_t *ct);
long cell_track_total_tracks(cell_track_t *ct);
long cell_track_count_entries(cell_track_t *ct);
long cell_track_refused(cell_track_t *ct);
long cell_track_two_cell_plots(cell_track_t *ct);

/* ================================================= GIVEN, in given/pool.c */

/* GIVEN. A pool of worker threads that protects each thing that it shares.
 * Each worker takes a report with sem_queue_pop, and puts it into the picture
 * with safe_track_update (or safe_track_submit in a checked pool). A report
 * with the flag P, or the class HIGH, also takes both resources with
 * resources_acquire_both, and names them in the order HQ data link,
 * tracking sensor: the opposite of the ranks. Your resources_acquire_both
 * must reorder them. Each worker names the same order, so a missing reorder
 * gives no freeze in this pool; the acceptance test finds it. */
typedef struct safe_pool safe_pool_t;

safe_pool_t *safe_pool_create(int n_workers, sem_queue_t *q, safe_track_t *t,
                              resources_t *res, int service_us, log_t *log);

/* GIVEN. The same pool, with each report CHECKED: its workers call
 * safe_track_submit (validate, associate, update) and not safe_track_update.
 * The final program uses it. gate and rules must not be NULL. */
safe_pool_t *safe_pool_create_checked(int n_workers, sem_queue_t *q, safe_track_t *t,
                                      resources_t *res, int service_us, log_t *log,
                                      const assoc_gate_t *gate,
                                      const valid_rules_t *rules,
                                      int late_ms);

/* GIVEN. A mission log line for each STEP of a worker, as pool_set_log_steps
 * of Lab 3: "worker ... started" at LOG_PENDING, and "track ... updated" at
 * LOG_TRACK. Off by default. */
void safe_pool_set_log_steps(safe_pool_t *p, int on);

/* GIVEN. Stop the pool and join each worker; then free it. */
void safe_pool_stop(safe_pool_t *p);
void safe_pool_destroy(safe_pool_t *p);

/* GIVEN. The totals of the workers, exact after safe_pool_stop. A refused
 * report is counted in processed and never enters the picture, so processed
 * = reports in the picture + refused. A late report waited past the deadline
 * and was dropped; an urgent report is never late. */
long safe_pool_total_processed(const safe_pool_t *p);
long safe_pool_total_refused(const safe_pool_t *p);
long safe_pool_total_late(const safe_pool_t *p);
long safe_pool_refused_for(const safe_pool_t *p, valid_err_t why);

/* GIVEN. What each worker does, for the page, read WHILE the pool runs. Each
 * worker writes its view under its own small mutex, so a reader never sees a
 * view half written. */
typedef struct {
    int  index;
    int  busy;                          /* 1 between the pop and the decision */
    long processed;
    char last_id[CONTACT_ID_MAX + 1];   /* "" before the first report         */
    char last_flag;                     /* 'H', 'P', 'R', or 0 before the first */

    /* The processor time of the worker, over its finished reports
     * (CLOCK_THREAD_CPUTIME_ID), and its thread id. On one processor the
     * shares of all the workers cannot be more than 1.00 in total. */
    int64_t cpu_ns;
    pid_t   tid;

    /* The elapsed time that the worker held a report, over its finished
     * reports: always at least cpu_ns. The difference is the time that it
     * waited for the processor. */
    int64_t busy_ns;
} safe_worker_view_t;

/* GIVEN. Copy at most max rows into out. Gives the number of workers, or -1
 * with errno EINVAL. The first call starts the views. */
int safe_pool_snapshot(safe_pool_t *p, safe_worker_view_t *out, int max);

/* GIVEN. Copy at most max of the newest spans of worker i into out, the
 * newest last. Gives how many it copied, or -1 with errno EINVAL. */
int safe_pool_worker_spans(safe_pool_t *p, int i, work_span_t *out, int max);

#endif /* SENTINEL_SYNC_H */
