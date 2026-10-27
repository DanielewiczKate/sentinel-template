/* proc.h — libproc, the sensor network. Lab 2.
 *
 * Lab 1 read one feed. Lab 2 runs four sensors as four processes, each with
 * its own pipe to the centre. It adds a named FIFO that brings urgent
 * contacts from any program on the machine, and a scheduler that chooses
 * the next report that the centre handles.
 *
 * WHO WRITES WHAT. You write the functions marked YOURS, in the file that
 * each section names: src/sensor.c, src/prio_channel.c and src/sched.c.
 * Each of the three files comes with a stub for each of these functions,
 * and its steps in a comment. The functions marked GIVEN are in
 * src/libproc/given/. You read them, and you
 * do not change them. Do not change a declaration in this file: the tests
 * that mark your lab call each function as it is declared here.
 *
 * Each function writes to the mission log through libsyscall (Lab 1). A
 * function that has no log (net->log == NULL) writes nothing.
 */

#ifndef SENTINEL_PROC_H
#define SENTINEL_PROC_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#include "contact.h"
#include "linereader.h"
#include "traffic.h"
#include "sentinel/syscall.h"

/* ================================================================ sensors */

/* One sensor: a child process that runs contactfeed. Its standard output is
 * the write end of a pipe of its own. The pipe goes one way, from the
 * sensor to the centre, and the centre holds the read end: the CHANNEL.
 *
 * WHY ONE PIPE FOR EACH SENSOR. The channel names the sender: a line that
 * arrives on the NWS channel came from the NWS process, whatever its text
 * says. End of file on one channel says which sensor stopped. A fast sensor
 * fills only its own pipe, so it cannot make the other sensors wait. */
typedef struct {
    sensor_id_t   id;           /* set by sensor_net_init, never changed     */
    pid_t         pid;          /* the child; 0 when no process runs         */
    int           fd;           /* the channel: the read end of the pipe, or
                                   -1 when it is closed                      */
    linereader_t *lr;           /* the line reader of fd; it owns fd         */
    int           taken;        /* GIVEN code only: lines in this pass       */
    int           more;         /* GIVEN code only: the last pass stopped at
                                   the limit                                  */
    int           rate;         /* reports each second, or the count         */
    int           burst;        /* non-zero: send `rate` reports, then exit 0 */
    long          restarts;     /* not used in Lab 2                          */
    int64_t       started_ns;   /* sc_now_ns() when the process started      */
    int64_t       restart_at_ns;/* not used in Lab 2                          */
} sensor_t;

/* The whole sensor network. */
typedef struct {
    sensor_t    sensors[SENSOR_COUNT];  /* index: sensor_id_t              */
    int         n_sensors;              /* how many processes run           */
    int         cursor;                 /* GIVEN code only                  */
    const char *feed_path;              /* the program that each sensor runs */
    log_t      *log;                    /* the mission log, or NULL          */
} sensor_net_t;

/* The most lines that the centre takes from one channel in one pass. With
 * no limit, a pass reads a fast channel until it is empty, and SAT at 2000
 * reports each second can keep it full: the other channels then wait. */
#define SENSOR_LINES_PER_PASS 64

/* ------------------------------------------------- YOURS, in src/sensor.c */

/* Start one sensor as a child process, with a new pipe.
 *
 * Input:
 *   net    a network from sensor_net_init. net->feed_path is the program to
 *          run, and net->log is the mission log.
 *   id     the sensor: SENSOR_NWS, SENSOR_AUR, SENSOR_AIS or SENSOR_SAT
 *          (common/contact.h). sensor_name(id) gives its three letters.
 *   rate   reports each second, from sensor_rate(id) in sensor_spawn_all.
 *          With burst != 0, rate is the number of reports to send.
 *   burst  0 for a sensor that sends at a rate until it is stopped; not 0
 *          for a sensor that sends `rate` reports and then exits 0.
 *
 * What it does, in this order:
 *   1. Make a pipe with pipe2() and the flag O_CLOEXEC.
 *   2. Give the read end to sensor_channel_open (GIVEN). The read end is
 *      then the channel of the sensor.
 *   3. Flush stdout and stderr, then fork().
  *   4. In the child: put the write end on standard output with dup2();
 *      ignore SIGPIPE; call cpu_unpin() (common/cpu.h; a failure of it is
 *      not fatal, and the child continues); then run the program
 *      with execvp() and exactly these arguments:
 *          net->feed_path  "--sensor"  NAME  "--rate"  N
 *      or, with burst != 0:
 *          net->feed_path  "--sensor"  NAME  "--burst" N
  *      where NAME is sensor_name(id) and N is rate as a decimal number. The
 *      child calls _exit(127) if execvp returns, and _exit(126) if dup2, or
 *      the fcntl below, fails. The child never returns from sensor_spawn.
 *   5. In the parent: close the write end. Set pid, rate, burst and
 *      started_ns (sc_now_ns()) in net->sensors[id], add 1 to
 *      net->n_sensors, and write this line to the mission log at level
 *      SYSTEM:
 *          sensor NAME started pid=PID rate=N fd=FD
 *      with "burst=N" in place of "rate=N" when burst != 0. FD is the
 *      channel.
 *
 * When the write end is descriptor 1 already, dup2() does nothing, and it
 * does not clear O_CLOEXEC either. Then clear the flag with
 * fcntl(1, F_SETFD, 0), or exec closes standard output.
 *
 * Output: 0. The sensor runs, and its lines arrive on its channel.
 */
/* Errors:
  * net == NULL, or id out of range: -1 with EINVAL.
 * The sensor runs (its pid is not 0): -1 with EEXIST. This test comes
 * before the next one: a sensor that runs also has its channel open.
 * Its old channel is still open (fd >= 0): -1 with EBUSY. The old channel
 * can hold lines from the last process; the centre reads it to its end, and
 * that closes it.
 * pipe2(), sensor_channel_open() or fork() fails: -1 with the errno of that
 * call. No descriptor of the new pipe stays open, and no child runs.
 * A failed execvp() is not an error of sensor_spawn: it has returned 0
 * already. The child exits 127, and sensor_reap logs "exited with code 127".
 */
int sensor_spawn(sensor_net_t *net, sensor_id_t id, int rate, int burst);

/* Stop one sensor and collect it.
 *
 * Input: net from sensor_net_init, and the sensor id.
 * What it does: if the sensor runs, send it SIGTERM with kill(), and wait
 * for it with waitpid(pid, ..., 0). Wait again when waitpid fails with
 * EINTR. Set its pid to 0, take 1 from net->n_sensors, and write this line
 * at level SYSTEM:
 *     sensor NAME (pid PID) stopped
 * The channel stays open, so that the centre can read the lines in it.
 *
 * Output: 0. A sensor that does not run gives 0, and nothing changes.
 */
/* Errors:
 * net == NULL, or id out of range: -1 with EINVAL.
 */
int sensor_stop(sensor_net_t *net, sensor_id_t id);

/* Collect each sensor process that has stopped. Do not wait for a sensor
 * that still runs.
 *
 * Input: net from sensor_net_init.
 * What it does: for each sensor whose pid is not 0, call
 *     waitpid(pid, &status, WNOHANG)
 * Call it again when it fails with EINTR. When it returns 0, the sensor
 * still runs. When it returns the pid, the sensor has stopped: write one
 * line at the level below, set its pid to 0, and take 1 from
 * net->n_sensors.
 *     ALERT   sensor NAME (pid PID) died on signal SIG (TEXT)
 *             when WIFSIGNALED(status); TEXT is strsignal(SIG)
 *     SYSTEM  sensor NAME (pid PID) finished with status 0
 *             when it exited with status 0
 *     ALERT   sensor NAME (pid PID) exited with code CODE
 *             when it exited with another status
 * Do not use waitpid(-1, ...): that also collects a child that is not a
 * sensor, and the code that started that child then cannot collect it.
 * The given centre_run calls sensor_reap in each pass.
 *
 * Output: the number of sensors collected in this call, 0 or more.
 */
/* Errors:
 * net == NULL: -1 with EINVAL.
 * A waitpid() error other than EINTR leaves that sensor as it is.
 */
int sensor_reap(sensor_net_t *net);

/* ------------------------------------------- GIVEN, in given/channels.c */

/* GIVEN. Prepare the network: clear the table of sensors. No pipe is made
 * here: each sensor_spawn makes its own. feed_path is the program that each
 * sensor runs, and log is the mission log or NULL.
 * Returns 0, or -1 with EINVAL when net or feed_path is NULL. */
int sensor_net_init(sensor_net_t *net, const char *feed_path, log_t *log);

/* GIVEN. Make fd the channel of s: set O_NONBLOCK on fd, and give it a line
 * reader. Your sensor_spawn calls it before fork(). The channel then owns
 * fd: close it with sensor_channel_close, not with close().
 * Returns 0, or -1 with errno set: EINVAL for a NULL s or fd < 0, EBUSY when
 * s has a channel already, or the errno of fcntl() or of the reader. After
 * -1, fd stays open, and the caller closes it. */
int sensor_channel_open(sensor_t *s, int fd);

/* GIVEN. Close the channel of s and its descriptor. Safe when it is closed. */
void sensor_channel_close(sensor_t *s);

/* GIVEN. Start each sensor at sensor_rate(id), with burst 0.
 * Returns the number started. */
int sensor_spawn_all(sensor_net_t *net);

/* GIVEN. Stop each sensor with sensor_stop, and close each channel. */
void sensor_net_shutdown(sensor_net_t *net);

/* GIVEN. The sensor whose process has this pid, or NULL. */
sensor_t *sensor_by_pid(sensor_net_t *net, pid_t pid);

/* GIVEN. How many sensors have a running process. */
int sensor_running_count(const sensor_net_t *net);

struct pollfd;

/* GIVEN. Put the channels in fds[0] to fds[SENSOR_COUNT - 1], in the order
 * of sensor_id_t, with events POLLIN. A closed channel gets fd -1, which
 * poll() does not use. Call it before each poll(): a new sensor_spawn
 * changes the descriptor of a channel. */
void sensor_net_pollfds(const sensor_net_t *net, struct pollfd *fds);

/* GIVEN. After poll(), give the next whole line from the channels, in turn.
 * Returns 1 and sets *from, *line and *len; the line has no '\n'. Returns 0
 * when this pass is done. Returns -1 with EMSGSIZE when one line was too
 * long; that line is lost, and the channel is still good. A channel at end
 * of file closes here, and the mission log says "sensor NAME channel
 * closed". */
int sensor_net_next_line(sensor_net_t *net, const struct pollfd *fds,
                         sensor_id_t *from, const char **line, size_t *len);

/* GIVEN. Non-zero when a channel stopped at the limit in the last pass. */
int sensor_net_more(const sensor_net_t *net);

/* GIVEN. How many channels are open. */
int sensor_net_open_count(const sensor_net_t *net);

/* ======================================================= priority channel */

/* The priority channel: a named FIFO, apart from the sensor pipes.
 *
 * A pipe connects only a process and its children. A FIFO has a name in the
 * file system, so any program on the machine can open it and send the
 * centre an urgent contact. The centre reads it with the sensor channels. */
typedef struct {
    char path[256];   /* the name of the FIFO, or "" when it is closed    */
    int  fd;          /* the read end, held by the centre, or -1           */
    int  fd_hold;     /* a write end that the centre holds itself, or -1   */
} prio_channel_t;

/* ------------------------------------------- YOURS, in src/prio_channel.c */

/* Create the FIFO, and open it for the centre.
 *
 * Input: ch, a prio_channel_t that the caller owns; path, the name of the
 * FIFO. demo_lab2 uses /tmp/sentinel_priority.fifo.
 * What it does, in this order:
 *   1. Set ch->fd and ch->fd_hold to -1, and copy path into ch->path.
 *   2. Remove an entry that exists at path with unlink(). Its error is not
 *      checked: there is usually no entry.
 *   3. Create the FIFO with mkfifo(path, 0600). Then set the mode with
 *      chmod(path, 0600): only the owner can read it or write to it.
 *   4. Open the read end into ch->fd with O_RDONLY | O_NONBLOCK | O_CLOEXEC.
 *   5. Open a write end into ch->fd_hold with O_WRONLY | O_NONBLOCK |
 *      O_CLOEXEC, and keep it open until prio_close.
 *
 * Why O_NONBLOCK in step 4: open() of a FIFO for reading waits until a
 * writer opens it. With no writer, the centre would wait for ever.
 * Why step 5: when no process holds a write end, a read of the FIFO gives
 * end of file, and poll() marks it as readable at once. The centre would
 * then use a full processor and do nothing. With its own write end open, an
 * empty FIFO is only empty.
 *
 * Output: 0. The FIFO exists at path with mode 0600, and ch->fd and
 * ch->fd_hold are open.
 */
/* Errors:
 * ch == NULL or path == NULL: -1 with EINVAL.
 * path has 256 or more characters: -1 with ENAMETOOLONG. The file system
 * does not change.
  * mkfifo() fails: -1 with its errno, and nothing is removed.
 * chmod() or open() fails: -1 with the errno of that call. The FIFO is
 * removed, and no descriptor stays open.
 * After each -1, ch is closed: fd and fd_hold are -1, and path is "". So a
 * prio_close of it does nothing.
 */
int prio_open(prio_channel_t *ch, const char *path);

/* Close the FIFO and remove it from the file system.
 *
 * What it does: close ch->fd_hold and ch->fd, remove ch->path with
 * unlink(), and set the descriptors to -1 and ch->path to "". A FIFO stays
 * in the file system after each process closes it, so it must be removed.
 * It is safe to call it two times, and with ch == NULL.
 */
void prio_close(prio_channel_t *ch);

/* Send one report into a FIFO that exists. Another program calls it, not
 * the centre.
 *
 * Input: path, the name of the FIFO; line, one contact report, with or
 * without its '\n'.
 * What it does: put the line and its '\n' in one buffer. Open path with
 * O_WRONLY | O_CLOEXEC (this open waits until the centre has the read end
 * open). Write the buffer with ONE call of write(), or of sc_write_all.
 * Close the descriptor.
 *
 * Why one write: the FIFO has many writers. Linux writes PIPE_BUF (4096)
 * bytes or fewer to a FIFO in one atomic step, so a line from another
 * writer cannot come between its bytes. This is true only when the line is
 * one write. A second write() for the '\n' lets another line in between.
 *
 * Output: 0. The line and one '\n' are in the FIFO.
 */
/* Errors:
 * path == NULL or line == NULL: -1 with EINVAL.
 * The line with its '\n' is longer than CONTACT_LINE_MAX (128): -1 with
 * EMSGSIZE, and nothing is opened or written.
 * open() or the write fails: -1 with the errno of that call. The descriptor
 * is closed.
 */
int prio_send(const char *path, const char *line);

/* ============================================================== scheduler */

/* Which report the centre handles next (requirement R2).
 *
 * The names are POLICY_*, not SCHED_*. <sched.h> defines SCHED_RR and
 * SCHED_FIFO as macros already, for the Linux scheduler of processes. This
 * is a different thing: a rule in the centre that chooses among reports. */
typedef enum {
    POLICY_FCFS = 0,   /* first come, first served: the oldest report       */
    POLICY_PRIORITY,   /* the most urgent class first: see urgency_t        */
    POLICY_RR          /* round robin: one report from each sensor in turn  */
} sched_policy_t;

/* THE TWO KINDS OF PRIORITY.
 *
 * The priority of the sensor is the P flag in the line. It records what the
 * sensor observed. A sensor cannot know if a contact is filed: it does not
 * hold the list.
 *
 * The priority of the centre is HIGH: a contact that is not filed AND is in
 * a region that the centre monitors. The centre decides both halves, from
 * common/traffic.txt.
 *
 * The centre handles the classes in this order, the highest value first. */
typedef enum {
    URGENCY_ROUTINE = 0,   /* neither of the two below                    */
    URGENCY_SENSOR,        /* the sensor marked it P                      */
    URGENCY_HIGH           /* not filed, and in a monitored region        */
} urgency_t;

/* One report that waits for the centre. */
typedef struct {
    contact_t   c;                 /* as the sensor sent it; c.flag never
                                      changes                              */
    int64_t     arrived_ns;        /* sc_now_ns() when it left its channel */
    int         from_prio_channel; /* 1 when it came through the FIFO      */
    urgency_t   urgency;           /* the class from centre_assess         */
    const char *region;            /* the region of a HIGH report, or NULL */
} pending_t;

typedef struct sched sched_t;   /* struct sched is in src/sched.c */

/* ------------------------------------------------ in src/sched.c
 *
 * src/sched.c comes from the course with struct sched and sched_next
 * written: they hold the three policies. Do not change them. YOU write
 * sched_create, sched_destroy, sched_push and sched_pending in the same
 * file, with the fields of struct sched.
 */

/* Make an empty scheduler.
 *
 * Input: policy, one of the three in sched_policy_t; capacity, the most
 * reports that can wait at one time, or 0 for 4096. demo_lab2 uses 65536.
 * Output: a new scheduler from malloc(), or NULL. Free it with
 * sched_destroy.
 */
/* Errors:
 * policy is not one of the three: NULL with EINVAL.
 * malloc() fails, or capacity times the size of pending_t does not fit in
 * size_t: NULL with ENOMEM.
 */
sched_t *sched_create(sched_policy_t policy, size_t capacity);

/* Free s and each report in it. Safe with s == NULL. */
void sched_destroy(sched_t *s);

/* Add a copy of the report *p at the end.
 *
 * Output: 0. The report waits in s.
 */
/* Errors:
 * s == NULL or p == NULL: -1 with EINVAL.
 * capacity reports wait already: -1 with ENOSPC, and s does not change.
 */
int sched_push(sched_t *s, const pending_t *p);

/* GIVEN in src/sched.c. Take the next report, as the policy chooses, out
 * of s, and copy it to *out.
 *
 *   POLICY_FCFS      the report that was pushed first.
 *   POLICY_PRIORITY  the report with the highest pending_urgency (GIVEN):
 *                    HIGH, then sensor P, then routine. In one class, the
 *                    report that was pushed first.
 *   POLICY_RR        the sensors in the order NWS, AUR, AIS, SAT, then
 *                    NWS again. Start with the sensor after the last one
 *                    served (with NWS the first time). Take the first
 *                    sensor in that order that has a report waiting, by
 *                    the sensor field c.sensor, and take its report that
 *                    was pushed first. The next turn starts after that
  *                    sensor. When no report has one of the four names,
 *                    take the report that was pushed first, and keep the
 *                    turn where it is.
 *
 * The reports that stay keep their order.
 *
 * Output: 1 and *out, or 0 when no report waits.
 */
/* Errors:
 * s == NULL or out == NULL: -1 with EINVAL.
 */
int sched_next(sched_t *s, pending_t *out);

/* How many reports wait in s. 0 when s == NULL. */
size_t sched_pending(const sched_t *s);

/* ------------------------------------- GIVEN, in given/policy.c, assess.c */

/* GIVEN. The name of a policy: "fcfs", "priority" or "rr"; or NULL. */
const char *sched_policy_name(sched_policy_t p);

/* GIVEN. Read a policy name. Returns 0 and sets *out, or -1. */
int sched_policy_parse(const char *name, sched_policy_t *out);

/* GIVEN. The class of one report. expected is the traffic that the centre
 * expects, with its monitored regions, or NULL: then only the flag of the
 * sensor counts. *region (if region is not NULL) gets the name of the
 * region of a HIGH report, or NULL. */
urgency_t centre_assess(const traffic_t *expected, const contact_t *c,
                        const char **region);

/* GIVEN. The class that decides the order of p: its urgency, but never
 * lower than URGENCY_SENSOR when the sensor marked it P. Use it in
 * sched_next. */
urgency_t pending_urgency(const pending_t *p);

/* GIVEN. The name of a class: "routine", "sensor P" or "HIGH". */
const char *urgency_name(urgency_t u);

/* GIVEN. One letter for a class: 'R', 'P' or 'H'. */
char urgency_letter(urgency_t u);

/* ================================================== the centre, GIVEN */

/* What the centre observed during a run. */
typedef struct {
    long total;           /* lines read from all channels                   */
    long accepted;
    long rejected;
    long priority;        /* accepted reports with the P flag of the sensor */
    long high;            /* accepted reports that the centre made HIGH     */
    long per_sensor[SENSOR_COUNT];  /* counted by channel, not by the text  */
    long from_prio_channel;
    long wrong_channel;   /* refused: the sensor field did not match the
                             channel                                        */

    /* The wait: from the time a report left its channel to the time the
     * scheduler gave it back. Monotonic nanoseconds. */
    int64_t lat_sum_ns;
    int64_t lat_max_ns;
    long    lat_count;

    /* The same, for the reports of class URGENCY_SENSOR or higher. */
    int64_t prio_lat_sum_ns;
    int64_t prio_lat_max_ns;
    long    prio_lat_count;
} centre_stats_t;

/* GIVEN, in given/centre.c. Run the centre until each sensor channel is
 * closed, or until run_ms have passed (0: no limit).
 *
 * It waits with poll() on the four sensor channels and the priority
 * channel, and takes at most SENSOR_LINES_PER_PASS lines from a channel in
 * one pass. It parses each line, gives it its class with centre_assess, and
 * gives it to sched_push. Then it serves each report that sched_next gives,
 * and calls sensor_reap. A line whose sensor field does not match its
 * channel is refused and counted in wrong_channel.
 *
 * expected is the traffic that the centre expects, or NULL. service_us is
 * the work that each report costs, in microseconds. With 0, no queue forms,
 * and the policy has nothing to choose between.
 *
 * Returns 0, or -1 with errno set. */
int centre_run(sensor_net_t *net, prio_channel_t *prio, sched_t *sched,
               const traffic_t *expected, int64_t run_ms, int service_us,
               centre_stats_t *stats);

/* GIVEN, in given/centre.c. Write the statistics to fd. */
int centre_stats_write(int fd, const centre_stats_t *st,
                       sched_policy_t policy, int64_t elapsed_ns);

#endif /* SENTINEL_PROC_H */
