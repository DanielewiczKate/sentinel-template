/* ipc.h — libipc, the operations centre. Lab 3.
 *
 * Lab 2 gave each report to one worker, and one worker cannot keep up with
 * RADARSAT, which sends 2000 reports each second. Lab 3 adds a pool of
 * worker threads that share one queue, and the track picture: the shared
 * record of what is out there. The main thread of the centre puts each
 * report into the queue, and the workers take the reports out.
 *
 * THIS LIBRARY HAS NO PROTECTION, ON PURPOSE.
 *
 * Each worker uses the queue and the track picture, and no lock protects
 * them. The structures are correct for one thread, and the tests prove it
 * with one thread. They become wrong only when many threads use them at the
 * same time. So the data structure has no fault; the SHARING has the fault.
 * Lab 3 measures what that costs, and Lab 4 adds the protection.
 *
 * WHO WRITES WHAT. You write the functions marked YOURS, in the file that
 * each section names: src/queue.c, src/track.c and src/pool.c. The
 * functions marked GIVEN are in src/libipc/given/. You read them, and you
 * do not change them. Do not change a declaration in this file: the tests
 * that mark your lab call each function as it is declared here.
 *
 * Each function that has a log writes to the mission log through
 * libsyscall (Lab 1). A function whose log is NULL writes nothing.
 */

#ifndef SENTINEL_IPC_H
#define SENTINEL_IPC_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>        /* pid_t, for the thread id of a worker */
#include <pthread.h>
#include <signal.h>           /* sig_atomic_t */

#include "contact.h"
#include "sentinel/syscall.h"
#include "sentinel/proc.h"    /* pending_t: the queue holds what Lab 2 made */

/* ================================================================== queue */

/* A ring buffer of reports that WAIT FOR A WORKER.
 *
 * It holds pending_t of Lab 2, and not contact_t, because a worker needs
 * one fact that the line of the sensor cannot give: WHEN THE CENTRE
 * ACCEPTED THE REPORT. pending_t holds it in arrived_ns, on the monotonic
 * clock. The report deadline and the wait in the queue both use it.
 *
 * head and tail move around a fixed array, `capacity` slots. Nothing moves
 * when a report leaves, so a push and a pop each take a fixed time. In a
 * ring, head == tail means "empty" and also "full", so the queue keeps a
 * count to tell the two apart.
 */
typedef struct {
    pending_t *slots;       /* capacity slots */
    size_t     capacity;
    size_t     head;        /* the slot of the next report to take out */
    size_t     tail;        /* the next free slot */
    size_t     count;       /* reports that wait now: 0 to capacity */

    /* The statistics. queue_push and queue_pop keep them up to date, and
       the given functions below read them. */
    long       pushed;      /* pushes that succeeded */
    long       popped;      /* pops that gave a report */
    long       rejected;    /* pushes refused because the queue was full */
    size_t     peak;        /* the highest count, since queue_init */
} queue_t;

/* -------------------------------------------------- YOURS, in src/queue.c */

/* Prepare an empty queue.
 *
 * Input: q, and capacity, the number of slots.
 * What it does: set each field of *q to 0. Allocate capacity slots of
 * pending_t for q->slots, and set q->capacity.
 *
 * Output: 0.
 */
/* Errors:
 * q == NULL, or capacity == 0: -1 with EINVAL.
 * The allocation fails, or capacity times the size of pending_t does not
 * fit in a size_t: -1 with ENOMEM. calloc() checks both.
 */
int    queue_init(queue_t *q, size_t capacity);

/* Free the slots. Set slots to NULL, and capacity and count to 0. head,
 * tail and the statistics stay. q == NULL does nothing. */
void   queue_destroy(queue_t *q);

/* Put a copy of *p at the tail.
 *
 * Input: q from queue_init, and p. The caller sets p->arrived_ns:
 * sched_next of Lab 2 does that already.
 * What it does:
 *     1. When count == capacity, add 1 to rejected, and give ENOSPC.
 *     2. Copy *p into slots[tail].
 *     3. tail = (tail + 1) % capacity. Add 1 to count and to pushed.
 *     4. When count > peak, set peak to count.
 *
 * Output: 0.
 */
/* Errors:
 * q == NULL, or p == NULL: -1 with EINVAL.
 * The queue is full: -1 with ENOSPC. Nothing changes except rejected.
 */
int    queue_push(queue_t *q, const pending_t *p);

/* Take the report at the head, in the order of arrival.
 *
 * Input: q from queue_init, and out.
 * What it does:
 *     1. When count == 0, give 0, and do not change *out.
 *     2. Copy slots[head] into *out.
 *     3. head = (head + 1) % capacity. Take 1 from count, and add 1 to
 *        popped.
 *
 * Output: 1 when *out holds a report, 0 when the queue is empty.
 */
/* Errors:
 * q == NULL, or out == NULL: -1 with EINVAL.
 */
int    queue_pop(queue_t *q, pending_t *out);

/* The number of reports that wait now, q->count. q == NULL gives 0. */
size_t queue_count(const queue_t *q);

/* --------------------------------------------- GIVEN, in given/queue_stats.c */

/* GIVEN. Each one reads a field that your functions keep. q == NULL gives 0
 * (queue_is_empty gives 1). */
int    queue_is_empty(const queue_t *q);
int    queue_is_full(const queue_t *q);
long   queue_pushed(const queue_t *q);
long   queue_popped(const queue_t *q);
long   queue_dropped(const queue_t *q);   /* rejected: the queue was full */
size_t queue_peak(const queue_t *q);      /* the highest count */

/* ========================================================== track picture */

/* The track picture: what the centre believes is out there.
 *
 * One record for each contact identity, found through a hash table. A
 * report about a contact that the picture knows UPDATES its record. It
 * does not add a second record, so the table is a picture and not a log.
 *
 * That update is the read, change and write that Lab 4 must protect.
 */

#define TRACK_BUCKETS 1024

/* The most entries that the track picture holds at one time.
 *
 * The queue has a limit and refuses a report when it is full. With no
 * limit here, a flood of contacts with NEW ids passes through the queue and
 * fills the memory: each track is 96 bytes, so 1 million new ids cost 94 MB.
 * 8192 keeps the picture below about 800 kB. The default feed `world` holds
 * about 36 objects, so a normal run never removes a track. */
#define TRACK_MAX_ENTRIES 8192

typedef struct track_entry {
    char    id[CONTACT_ID_MAX + 1];
    char    sensor[CONTACT_SENSOR_LEN + 1];  /* the sensor of the last report */
    double  lat, lon, speed;                 /* from the last report */
    long    reports;          /* how many reports name this contact */
    int     priority;         /* 1 when any report of it had the flag P */

    /* The flag of the LAST report, 'P' or 'R'. `priority` answers "was this
     * target ever urgent", and stays 1. last_flag answers "is it urgent
     * now". A page that draws `priority` keeps a target red for the whole
     * run because of one report long ago. */
    char    last_flag;

    int64_t first_seen_ns;    /* sc_now_ns() of the first report */
    int64_t last_seen_ns;     /* sc_now_ns() of the last report */
    struct track_entry *next; /* the next entry in the same bucket */
} track_entry_t;

typedef struct {
    track_entry_t *buckets[TRACK_BUCKETS];   /* the head of each chain */

    /* The shared totals. Each worker adds to them, and an addition is not
     * atomic. Lab 3 measures that, and Lab 4 corrects it. */
    long total_reports;       /* reports applied */
    long total_tracks;        /* tracks held NOW: an eviction lowers it */
    long priority_tracks;     /* tracks held now with priority == 1 */

    /* Tracks removed to stay inside TRACK_MAX_ENTRIES. A number above 0 is
     * not an error: the picture was full, and the centre chose what to
     * forget. An operator must see that number. */
    long total_evicted;
} track_t;

/* -------------------------------------------------- YOURS, in src/track.c */

/* Apply one report: update the track of its contact, or create the track.
 *
 * Input: t from track_init, and c. c->id is the identity of the contact.
 * What it does:
 *     1. Find the entry whose id is c->id, in the chain that starts at
 *        t->buckets[track_bucket(c->id)].
 *     2. now = sc_now_ns(). The report is urgent when c->flag is
 *        PRIORITY_PRIORITY.
 *     3. The entry exists: copy lat, lon, speed and sensor from c. Add 1 to
 *        reports. Set last_seen_ns to now, and last_flag to 'P' (urgent) or
 *        'R'. When the report is urgent and priority is 0, set priority to 1
 *        and add 1 to t->priority_tracks. Add 1 to t->total_reports.
 *        Give 0.
 *     4. No entry: when t->total_tracks >= TRACK_MAX_ENTRIES, call
 *        track_evict_one(t, track_bucket(c->id)) first. Allocate a new entry set to 0.
 *        Copy id, sensor, lat, lon and speed from c. Set reports to 1,
 *        priority to 1 (urgent) or 0, last_flag to 'P' or 'R', and
 *        first_seen_ns and last_seen_ns to now. Put it at the HEAD of its
 *        chain. Add 1 to t->total_tracks and t->total_reports, and to
 *        t->priority_tracks when it is urgent. Give 1.
 *
 * Output: 1 when a new track was created, 0 when a track was updated.
 */
/* Errors:
 * t == NULL, or c == NULL: -1 with EINVAL.
 * The allocation fails: -1 with ENOMEM. No total changes; an eviction of
 * step 4 is not undone.
 */
int  track_update(track_t *t, const contact_t *c);

/* --------------------------------------------- GIVEN, in given/track_table.c */

/* GIVEN. Prepare an empty picture: each bucket NULL, each total 0.
 * Returns 0, or -1 with EINVAL when t is NULL. */
int  track_init(track_t *t);

/* GIVEN. Free each entry, and set each total to 0. */
void track_destroy(track_t *t);

/* GIVEN. The bucket of an identity: a number from 0 to TRACK_BUCKETS - 1.
 * track_update and track_find use the same one. */
size_t track_bucket(const char *id);

/* GIVEN. Remove the track that was seen least recently, from a sample of
 * buckets that starts at start_bucket. It takes 1 from total_tracks (and
 * from priority_tracks for a track with priority 1), and adds 1 to
 * total_evicted. */
void track_evict_one(track_t *t, size_t start_bucket);

/* GIVEN. The track of a contact identity, or NULL. */
const track_entry_t *track_find(const track_t *t, const char *id);

/* GIVEN. The totals. t == NULL gives 0. */
long track_total_reports(const track_t *t);
long track_total_tracks(const track_t *t);
long track_total_evicted(const track_t *t);

/* GIVEN. Count the entries: walk each bucket. This is the INDEPENDENT
 * count. total_tracks and the walk are kept apart, and they must agree; a
 * difference proves that an update was lost. Call it when no worker runs:
 * a worker that adds an entry can leave a pointer half-written. */
long track_count_entries(const track_t *t);

/* ------------------------------------------- GIVEN, in given/associate.c */

/* ASSOCIATION: the track that a new report belongs to. Lab 4 uses it.
 *
 * With no association, the picture uses the id that the SENSOR gave. Two
 * sensors that see one aircraft use different ids, so one aircraft is two
 * tracks. A radar gives a POSITION, not a name, so the centre must assign
 * the identity.
 *
 * The rule is a gate. A report joins a track when all three are true: it is
 * near enough, the track is recent enough, and the speed of the move is
 * possible. The nearest such track wins. */
typedef struct {
    double  max_km;         /* the most distance between two reports of one target */
    int64_t max_age_ns;     /* a track not seen for longer is not a candidate */
    double  max_speed_kn;   /* the highest speed that the move may give */
} assoc_gate_t;

/* GIVEN. 25 km, 30 s, 700 knots. */
extern const assoc_gate_t ASSOC_DEFAULT;

/* GIVEN. The distance on the surface of the earth, in kilometres. */
double assoc_distance_km(double lat1, double lon1, double lat2, double lon2);

/* GIVEN. The nearest track that passes the three gates, or NULL when the
 * report is a new object. */
const track_entry_t *track_associate(const track_t *t, const contact_t *c,
                                     const assoc_gate_t *gate);

/* GIVEN. Associate, then track_update with the identity that the centre
 * chose. Returns what track_update returns. When assoc_id is not NULL, it
 * gets that identity: the id of the sensor for a new track, or the id of
 * the track that matched. */
int track_update_associated(track_t *t, const contact_t *c,
                            const assoc_gate_t *gate,
                            char assoc_id[CONTACT_ID_MAX + 1]);

/* -------------------------------------------- GIVEN, in given/validate.c */

/* VALIDATION: is the report physically possible? Lab 4 uses it.
 *
 * contact_parse decides if a line is WELL FORMED. It does not decide if a
 * report is TRUE, and each forged report of the attack tool was well
 * formed. Two of the checks need the history of the track, which is shared
 * state, so Lab 4 calls the check with the lock held. */
typedef enum {
    VALID_OK = 0,
    VALID_ERR_SPEED,        /* faster than any object in this scenario */
    VALID_ERR_TELEPORT,     /* moved more than possible since the last report */
    VALID_ERR_STALE,        /* the time is far in the past: a replay */
    VALID_ERR_FUTURE        /* the time is after the clock of the centre */
} valid_err_t;

typedef struct {
    double  max_speed_kn;
    int64_t max_age_ns;     /* how old a report can be before it is a replay */
    int64_t max_future_ns;  /* how far two clocks can disagree */
} valid_rules_t;

extern const valid_rules_t VALID_DEFAULT;

/* GIVEN. The text of a result. */
const char *valid_strerror(valid_err_t e);

/* GIVEN. Read only. t can be NULL: then only the checks that need no
 * history run. */
valid_err_t track_validate(const track_t *t, const contact_t *c,
                           const valid_rules_t *r);

/* ------------------------------------------------ GIVEN, in given/work.c */

/* The cost of one report to a worker.
 *
 * There is no radar here, so a worker must SPEND something for each
 * report. It spends PROCESSOR TIME. A worker that waits for a time on the
 * wall clock costs nothing on a busy processor: the clock runs while the
 * thread is off the processor, so eight workers finish in the time of one,
 * and one processor seems free. Processor time cannot give that false
 * result, because the time off the processor does not count. */

/* GIVEN. How many passes of work_do cost one microsecond of processor time
 * on this machine. It measures one time, at the first call, in about 1 s.
 * Returns -1 when the three tries do not agree. */
long work_calibrate(void);

/* GIVEN. How much the three tries of the calibration disagreed, in percent
 * of the middle one, or -1 before work_calibrate. */
int work_calibration_spread_pct(void);

/* GIVEN. Spend about service_us microseconds of processor time on c. The
 * caller MUST store the result: if not, the compiler can remove the loop
 * (a test of this project lost its work that way). */
double work_do(const contact_t *c, int service_us, long passes_per_us);

/* =================================================================== pool */

/* A pool of worker threads that share one queue. */

typedef struct pool pool_t;

/* The statistics of one worker. Only that worker writes them, so their sum
 * is exact when the shared totals are wrong. */
typedef struct {
    long processed;      /* reports taken from the queue, not late, and given
                            to track_update (also when it gave -1) */
    long new_tracks;     /* track_update gave 1 */
    long updates;        /* track_update gave 0 */
    long late;           /* taken from the queue, then dropped as late */
} worker_stats_t;

/* ONE REPORT, AS THE WORKER SAW IT. GIVEN code writes it.
 *
 *   start_ns, end_ns   CLOCK_MONOTONIC: how long the report took.
 *   cpu_ns             CLOCK_THREAD_CPUTIME_ID: how much processor it got.
 *
 * On an idle machine the two agree. On ONE processor with eight workers,
 * a report of 200 us of processor time can take milliseconds, because the
 * worker waits for its turn. The spans of different workers then OVERLAP,
 * on a machine that runs only one of them at a time. That overlap is
 * concurrency, and the page draws it. */
typedef struct {
    int64_t start_ns;
    int64_t end_ns;
    int64_t cpu_ns;
    char    flag;        /* 'H', 'P' or 'R': urgency_letter() */
} work_span_t;

#define WORKER_SPANS 32

/* One worker: its argument block. pthread_create gives the thread the
 * address of this struct, and each worker has its own. Only that worker
 * writes it. The fields after `stats` are the instruments, and only the
 * given functions of given/pool_watch.c write them. */
typedef struct {
    pool_t        *pool;
    int            index;          /* 0 to n_workers - 1 */
    worker_stats_t stats;

    pid_t          tid;            /* gettid(), for cpu_thread_ctxt */
    int64_t        cpu_ns;         /* processor time, summed over reports */
    int64_t        busy_ns;        /* elapsed time with a report */
    work_span_t    spans[WORKER_SPANS];
    int            span_next;      /* where the next span goes */
    long           span_total;     /* how many spans were ever written */
    double         work_sink;      /* the result of work_do: see work.c */
} pool_worker_t;

/* The pool. Your pool.c and the given pool_watch.c both read it, so it is
 * stated here. */
struct pool {
    pthread_t     *tids;           /* n_workers thread ids */
    pool_worker_t *workers;        /* n_workers argument blocks */
    int            n_workers;

    queue_t       *queue;
    track_t       *track;
    int            service_us;     /* the work of one report, 0 for none */
    int            late_ms;        /* the report deadline, 0 for none */
    long           passes_per_us;  /* set by pool_calibrate */
    log_t         *log;            /* the mission log, or NULL */

    /* volatile: the compiler must read the flag again in each turn of the
     * worker loop, and must not keep it in a register. */
    volatile sig_atomic_t stop;      /* 1: each worker drains, then returns */
    volatile sig_atomic_t observed;  /* GIVEN code only */
    volatile sig_atomic_t log_steps; /* GIVEN code only */
};

/* What pool_report_begin gives to pool_report_end for one report. */
typedef struct {
    int     steps;
    int     observed;
    int64_t began_ns;
    int64_t began_cpu;
} pool_report_t;

/* THE REPORT DEADLINE, in milliseconds. A routine report that waited longer
 * than this in the queue is dropped, and is not applied.
 *
 * Why 5000. The association gate (ASSOC_DEFAULT) says that two reports are
 * one object when they are 25 km apart or less, and a track not seen for
 * 30 s is not a candidate. The fastest object that the gate accepts flies at
 * 700 knots, 0.36 km/s, and it flies a TENTH of the 25 km gate in 6.9 s. So
 * a report that waited more than about 5 s moves the picture by a tenth of
 * the tolerance of the gate.
 *
 * 0 switches the deadline off. An URGENT report is never dropped for its
 * age: see R2. */
#define POOL_LATE_DEFAULT_MS 5000

/* --------------------------------------------------- YOURS, in src/pool.c */

/* Start a pool of n_workers threads. They take reports from q and apply
 * them to t.
 *
 * Input: n_workers (1 or more), q from queue_init, t from track_init,
 * service_us (the work of one report, 0 or less for none), late_ms (the
 * report deadline, 0 or less for none), and log (or NULL).
 * What it does:
 *     1. Allocate the pool, n_workers thread ids and n_workers argument
 *        blocks, each set to 0. Set n_workers, queue, track, service_us,
 *        late_ms and log.
 *     2. Call pool_calibrate(p), BEFORE the first thread starts.
 *     3. For each i from 0: set workers[i].pool to p and workers[i].index to
 *        i. Start the thread with pthread_create(&tids[i], NULL, worker,
 *        &workers[i]). Each thread gets the address of ITS OWN block.
 *     4. Write this line at level SYSTEM:
 *            worker pool started: N workers, US us, deadline MS ms
 *
 * THE WORKER is a static function of pool.c. Its argument is its block,
 * w, and it returns NULL. Its steps:
 *     1. Call pool_worker_start(w).
 *     2. While p->stop is 0: take a report with queue_pop. When the queue
 *        is empty, call usleep(200) and start the turn again. Do not spin
 *        with no wait: on one processor, a spin takes the processor from
 *        the thread that fills the queue.
 *     3. After the stop, take each report that is still in the queue, until
 *        queue_pop gives 0. This is the DRAIN. A stop does not discard a
 *        report that waits.
 *     4. For each report of step 2 or step 3:
 *          - when pool_report_late(p, &item) gives 1, add 1 to
 *            w->stats.late, and do not apply it;
 *          - if not, call pool_report_begin(w, &item, draining, &r), where
 *            draining is 1 in step 3 and 0 in step 2; then
 *            created = track_update(p->track, &item.c); then
 *            pool_report_end(w, &item, created, &r). Add 1 to
 *            w->stats.processed. Add 1 to new_tracks when created is 1,
 *            and to updates when created is 0.
 *
 * Output: the pool.
 */
/* Errors:
 * n_workers <= 0, q == NULL or t == NULL: NULL with EINVAL.
 * An allocation fails: NULL with ENOMEM. Free what was allocated.
 * pool_calibrate fails: NULL with EAGAIN. Free each allocation.
 * pthread_create fails for thread i: set p->stop to 1, join threads 0 to
 * i - 1, free each allocation, and give NULL with errno set to the value
 * that pthread_create returned. No thread of the pool runs after it. The
 * threads that started drain the queue before they return: the reports
 * that waited are applied, not lost.
 *
 * CALLS THAT NEED NO CHECK in pool.c, each with a comment that says why
 * (the rule of Lab 1): pthread_join() of a thread of this pool that no
 * other code joins; log_writef(), because a lost log line is not an
 * error of the pool; and usleep(200), because the loop tries the queue
 * again after it returns, also after EINTR.
 */
pool_t *pool_create_deadline(int n_workers, queue_t *q, track_t *t,
                             int service_us, int late_ms, log_t *log);

/* Tell each worker to stop, and wait for each one.
 *
 * Input: p from pool_create_deadline.
 * What it does: set p->stop to 1. Call pthread_join for each thread: each
 * worker drains the queue, and then returns. Then write this line at level
 * SYSTEM, where N is pool_total_processed(p):
 *     worker pool stopped: N processed
 * Call it ONE time for a pool. p == NULL does nothing.
 */
void pool_stop(pool_t *p);

/* Free the thread ids, the argument blocks and the pool. Call it after
 * pool_stop. p == NULL does nothing. */
void pool_destroy(pool_t *p);

/* The sum of stats.processed over each worker. p == NULL gives 0. */
long pool_total_processed(const pool_t *p);

/* The sum of stats.late over each worker: reports taken from the queue and
 * then dropped, because they waited past the deadline. p == NULL gives 0. */
long pool_total_late(const pool_t *p);

/* ---------------------------------------------- GIVEN, in given/pool_watch.c */

/* GIVEN. pool_create_deadline with late_ms POOL_LATE_DEFAULT_MS. */
pool_t *pool_create(int n_workers, queue_t *q, track_t *t, int service_us,
                    log_t *log);

/* GIVEN. When p->service_us > 0, measure the work with work_calibrate, set
 * p->passes_per_us, and write this line at level SYSTEM:
 *     work calibrated: PASSES passes = 1 us of CPU (spread S%)
 * Returns 0, or -1 when the measure does not settle. */
int  pool_calibrate(pool_t *p);

/* GIVEN. The first call of a worker thread: it records the thread id. */
void pool_worker_start(pool_worker_t *w);

/* GIVEN. 1 when the report is routine and waited longer than p->late_ms
 * since item->arrived_ns. A report with the sensor's P or the centre's HIGH
 * gives 0, and late_ms 0 gives 0. */
int  pool_report_late(const pool_t *p, const pending_t *item);

/* GIVEN. Call it before track_update. When the step lines are on, it
 * writes this line at level PENDING, where I is w->index, with
 * " (draining)" at the end when draining is 1:
 *     worker I started ID from SENSOR after MS ms in the queue
 * It starts the clocks when a reader asked for the spans, and it spends the
 * work of the report. */
void pool_report_begin(pool_worker_t *w, const pending_t *item, int draining,
                       pool_report_t *r);

/* GIVEN. Call it after track_update, with what track_update returned. When
 * the step lines are on and created >= 0, it writes this line at level
 * TRACK, with "created" when created is 1 and "updated" when it is 0:
 *     track ID created from SENSOR at LAT LON, SPEED kn
 * It records the span of the report. */
void pool_report_end(pool_worker_t *w, const pending_t *item, int created,
                     pool_report_t *r);

/* GIVEN. Write a mission log line for each STEP of a worker, with
 * pool_report_begin and pool_report_end. Story vi of the course
 * documentation names two that happen only inside a worker: "Worker
 * processing started" and "Track updated". Off by default. A worker reads
 * the flag one time for each report, so a change starts at the next
 * report. To have a line for EACH report, call it before the first report
 * can arrive. */
void pool_set_log_steps(pool_t *p, int on);

/* GIVEN. The number of workers. p == NULL gives 0. */
int  pool_worker_count(const pool_t *p);

/* GIVEN. The statistics of worker i, or NULL when i is out of range. */
const worker_stats_t *pool_worker_stats(const pool_t *p, int i);

/* GIVEN. What each worker spent, and its thread id.
 *
 * The FIRST call of one of these starts the recording. So a run with no
 * page pays nothing for it.
 *
 * cpu_ns is the processor time of worker i over its finished reports.
 * Divide its change by the elapsed time, and you have the share of a
 * processor that the worker got. ON ONE PROCESSOR THE SHARES CANNOT ADD UP
 * TO MORE THAN 1.00, for any number of workers. That number tells
 * concurrency from parallelism.
 *
 * busy_ns is the elapsed time that worker i held a report, so it is always
 * at least cpu_ns. The difference is the time that the worker waited for
 * the processor.
 *
 * NOT PROTECTED, as the rest of Lab 3: a worker writes these while a reader
 * reads them. Lab 4 copies the same numbers under a lock. */
int64_t pool_worker_cpu_ns(const pool_t *p, int i);
int64_t pool_worker_busy_ns(const pool_t *p, int i);
pid_t   pool_worker_tid(const pool_t *p, int i);

/* GIVEN. Copy at most max of the newest spans of worker i into out, the
 * newest last. Returns how many it copied, or -1 with EINVAL. */
int     pool_worker_spans(const pool_t *p, int i, work_span_t *out, int max);

#endif /* SENTINEL_IPC_H */
