/* libsyscall — the operating system interface of SENTINEL. Lab 1.
 *
 * This header is given. Do not change the declarations: other code, and the
 * tests that mark your lab, call these functions exactly as declared here.
 * You write every function in src/.
 *
 * Each function has two comments. The first comment states what the function
 * does, what it takes, and what it returns when it succeeds. The second
 * comment, "Errors:", states every failure. When a function fails, it returns
 * -1 (or NULL) and sets errno, unless its "Errors:" comment says something
 * else.
 */

#ifndef SENTINEL_SYSCALL_H
#define SENTINEL_SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ io.c */

/* Open path for reading. Returns a file descriptor. */
/* Errors:
 * open() is retried after EINTR. Any other failure gives -1 with the errno
 * of open(), such as ENOENT or EACCES.
 */
int sc_open_read(const char *path);

/* Open path for writing. Returns a file descriptor.
 * Create the file if it does not exist, with the permission bits 0644. The
 * umask of the process masks them, as it does for open(): with umask 027 the
 * file gets 0640. Do not change the mode after the file opens.
 *   append != 0   every write goes to the end of the file, also when other
 *                 processes write the same file at the same time. The content
 *                 that the file holds already stays.
 *   append == 0   the existing content is removed when the file opens.
 */
/* Errors:
 * open() is retried after EINTR. Any other failure gives -1 with the errno
 * of open().
 */
int sc_open_write(const char *path, int append);

/* The same as sc_open_write, but a new file gets the permission bits in mode,
 * masked by the umask in the same way. Returns a file descriptor. */
/* Errors:
 * open() is retried after EINTR. Any other failure gives -1 with the errno
 * of open().
 */
int sc_open_write_mode(const char *path, int append, mode_t mode);

/* Close fd. Returns 0. */
/* Errors:
 * EINTR is not a failure: Linux has released fd already, so the function
 * returns 0. Any other failure gives -1 with the errno of close(), such as
 * EBADF.
 */
int sc_close(int fd);

/* Write all n bytes of buf to fd. Returns 0 only when every byte is written.
 * n == 0 writes nothing and returns 0, whatever fd is.
 */
/* Errors:
 * write() is called again after EINTR and after a partial write. write()
 * returning 0 for a non-zero count gives -1 with EIO, because a loop that
 * went on would never end. Any other failure gives -1 with the errno of
 * write(). Bytes written before a failure stay written.
 */
int sc_write_all(int fd, const void *buf, size_t n);

/* Read from fd into buf until n bytes arrive or the input ends. The input
 * ends when read() returns 0.
 * Returns the number of bytes read: n, or fewer only at the end of the input.
 * n == 0 reads nothing and returns 0, whatever fd is.
 */
/* Errors:
 * read() is called again after EINTR. Any other failure gives -1 with the
 * errno of read(), also when some bytes arrived first: they are in buf, but
 * not counted.
 */
ssize_t sc_read_full(int fd, void *buf, size_t n);

/* --------------------------------------------------------------- clock.c */

/* Bytes that a timestamp from sc_stamp needs, including the '\0'. */
#define SC_STAMP_MAX 32

/* Nanoseconds from a clock that never goes backwards. The count starts at an
 * arbitrary point, so the value is not a date. Use it to measure durations.
 */
/* Errors:
 * It cannot fail.
 */
int64_t sc_now_ns(void);

/* The current date and time: seconds since the epoch, and microseconds from
 * 0 to 999999. A NULL pointer is allowed, and nothing is written to it.
 */
/* Errors:
 * It cannot fail.
 */
void sc_now_wall(int64_t *sec, int32_t *usec);

/* Write the current date and time into buf as "<seconds>.<microseconds>",
 * with exactly 6 digits after the point: "1755534061.000042". buflen is the
 * size of buf, and must hold the stamp and its '\0': SC_STAMP_MAX is enough.
 * Returns the length of the stamp, not counting the '\0'.
 */
/* Errors:
 * buf == NULL gives -1 with EINVAL. A buffer that is too small gives -1 with
 * ERANGE. On a failure, buf is not changed.
 */
int sc_stamp(char *buf, size_t buflen);

/* ----------------------------------------------------------------- log.c */

/* The mission log. Every action of SENTINEL is timestamped and written to it
 * (requirement R6). The second column of the comment is the exact name that
 * log_level_name returns and that a log line shows. */

typedef enum {
    LOG_SYSTEM = 0,   /* "SYSTEM"    start, stop, configuration          */
    LOG_INFO,         /* "INFO"      a routine report was received       */
    LOG_PENDING,      /* "PENDING"   queued, waiting for a worker        */
    LOG_PRIORITY,     /* "PRIORITY"  a priority contact was promoted     */
    LOG_ALERT,        /* "ALERT"     a threat assessment was raised      */
    LOG_TRACK         /* "TRACK"     the track picture was updated       */
} log_level_t;

/* The handle of an open log. Callers see only a pointer to it. You define
 * struct log in log.c. It holds at least the file descriptor of the log, and
 * you choose its other fields. */
typedef struct log log_t;

/* Open the mission log at path. Returns a handle for log_writef and
 * log_close.
 * Create the file if it does not exist. The lines that the file holds
 * already stay: log_open never removes content. New lines always go to the
 * end of the file, and several processes may write the same log at the same
 * time.
 * Only the owner may read or write the file (mode 0600). This is also true
 * when the file existed before with other permissions. log_open sets this
 * mode with fchmod() after open(), for each file, new or old.
 */
/* Errors:
 * path == NULL gives NULL with EINVAL. open() is retried after EINTR, as in
 * sc_open_write. When the mode cannot be set to 0600, the log is closed,
 * and the result is NULL with the errno of fchmod(), such as EPERM. Any
 * other failure gives NULL with the errno of the call that failed.
 */
log_t *log_open(const char *path);

/* Write one line to the log, with a message formatted like printf. Returns 0.
 *
 *     <timestamp> <LEVEL> <message>\n
 *     1755534061.482913 INFO     contact ACCF-4587 from NWS
 *
 * The timestamp is the sc_stamp format. The level name is left-aligned in a
 * field of 8 characters, and one space follows the field.
 * A line is at most 512 bytes, including its '\n'. A longer message is cut at
 * its end, and the line still ends with '\n'.
 * A line never mixes with a line from another writer, and no line is lost.
 * To make this true, format the whole line into one buffer first, then write
 * the buffer with sc_write_all on the log, which log_open opened with
 * O_APPEND. On a regular file, Linux writes a buffer this short in one
 * write() call, so no other line can come between two parts of it.
 */
/* Errors:
 * lg == NULL, fmt == NULL or an unknown level gives -1 with EINVAL, and
 * writes nothing. A format that vsnprintf() cannot convert gives -1 with
 * EOVERFLOW, and writes nothing. A failed write gives -1 with the errno of
 * write(). Each '\n' in the message becomes a space, also a '\n' at its end,
 * so one call always writes one line.
 */
int log_writef(log_t *lg, log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Close the log and free the handle. Returns 0. */
/* Errors:
 * lg == NULL returns 0. EINTR from close() is not a failure, as in sc_close.
 * Any other failed close gives -1 with the errno of close().
 * The handle is freed in all cases, so do not use it again.
 */
int log_close(log_t *lg);

/* The name of a level, as the comment of log_level_t gives it: "SYSTEM",
 * "INFO", "PENDING", "PRIORITY", "ALERT" or "TRACK". */
/* Errors:
 * A value that is not a log_level_t gives NULL. errno does not change.
 */
const char *log_level_name(log_level_t level);

/* ---------------------------------------------------------------- proc.c */

/* What the kernel knows about a running process, read from /proc.
 *
 * The directory /proc/<pid> describes the process with that pid. For the
 * calling process, use /proc/self. Most fields come from the text file
 * /proc/<pid>/status, which has one field on each line:
 *
 *     <Key>:<spaces or tabs><value>
 *     VmRSS:	    6736 kB
 *
 * A memory value ends with " kB". Find a field by its whole key, up to and
 * including the ':'. A search for "voluntary_ctxt_switches" at any point in
 * a line also finds "nonvoluntary_ctxt_switches", and gives the wrong number.
 *
 *   field        where the value comes from
 *   pid          the pid argument, or getpid() when the argument is 0
 *   ppid         status, key "PPid"
 *   name         status, key "Name"
 *   vm_rss_kb    status, key "VmRSS", the number without " kB"
 *   vm_size_kb   status, key "VmSize", the number without " kB"
 *   threads      status, key "Threads"
 *   open_fds     the number of entries in the directory /proc/<pid>/fd,
 *                not counting "." and ".."
 *   vol_ctxt     status, key "voluntary_ctxt_switches"
 *   invol_ctxt   status, key "nonvoluntary_ctxt_switches"
 */
typedef struct {
    pid_t pid;
    pid_t ppid;
    char  name[64];      /* the executable name, with no '\n' */
    long  vm_rss_kb;     /* resident memory, in kilobytes     */
    long  vm_size_kb;    /* total virtual memory, in kilobytes */
    int   threads;       /* the number of threads             */
    int   open_fds;      /* the number of open descriptors    */

    /* How often this process stopped running, and who decided.
     *
     * vol_ctxt   it gave the processor up itself, because it had to wait for
     *            something: a read, a lock, a sleep.
     * invol_ctxt the operating system TOOK the processor away, because
     *            another task was owed a turn.
     *
     * A rising involuntary count is the operating system sharing one
     * processor between several tasks. */
    long  vol_ctxt;
    long  invol_ctxt;
} proc_info_t;

/* Fill out for process pid. pid 0 means the calling process. Returns 0.
 * The comment of proc_info_t gives the source of each field.
 * For the calling process (pid 0, or its own pid), open_fds does not count
 * the descriptor that proc_report itself opens to read /proc/<pid>/fd.
 */
/* Errors:
 * out == NULL gives EINVAL. A pid with no process gives ENOENT. A process of
 * another user gives EACCES, because its fd directory cannot be read. A
 * field that /proc does not list, such as VmSize for a kernel thread, is 0.
 * On a failure, the content of out is not defined.
 */
int proc_report(pid_t pid, proc_info_t *out);

/* Write info to fd as text, in exactly this layout (%d, %s and %ld as in
 * printf). Returns 0 when the whole report is written.
 *
 *     process report
 *       pid        %d
 *       ppid       %d
 *       name       %s
 *       vm_size    %ld kB
 *       vm_rss     %ld kB
 *       threads    %d
 *       open_fds   %d
 *       ctxt_vol   %ld
 *       ctxt_invol %ld
 *
 * Each line after the first has two spaces, then the label (such as "pid")
 * left-aligned in a field of 10 characters, then one space, then the value.
 * Every line ends with '\n'. The order of the lines is not the order of the
 * fields in proc_info_t: vm_size comes before vm_rss.
 */
/* Errors:
 * info == NULL gives -1 with EINVAL. A failed write gives -1 with the errno
 * of write().
 */
int proc_report_write(int fd, const proc_info_t *info);

#endif /* SENTINEL_SYSCALL_H */
