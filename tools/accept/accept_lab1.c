/* accept_lab1 — the acceptance test of Lab 1.
 *
 *   accept_lab1 [--demo PATH] [--contactfeed PATH]
 *
 * `make accept1` compiles this file with your build/libsyscall.a, so each call
 * to an sc_*, log_* or proc_* function below runs your code. Then the test runs
 * your build/demo_lab1 as a program, and reads what it wrote. It prints PASS
 * or FAIL for each of 34 checks, then the share that passed for each file.
 *
 * Read a check to see what it does. Do not change this file. The marks come
 * from the course copy of it, and Section 6.1 of docs/lab1/lab1_brief.pdf
 * gives them.
 *
 * How the test works:
 *
 *   - Each check runs in its own child process, with a time limit. A crash,
 *     or a call that does not return, stops that check only. The test still
 *     reports each other check. fork() makes the child process; Lab 2 teaches
 *     it.
 *
 *   - The test does its own work with the C library only. It calls one of
 *     your functions only in the check of that function. So a fault in
 *     sc_read_full cannot make the test read the mission log of the demo
 *     wrongly.
 *
 *   - This file defines read() and write(). Your library is linked to these
 *     definitions, and a check can make them move only a part of the bytes,
 *     or fail with EINTR. On a regular file, the kernel almost always moves
 *     all the bytes in one call. Without these definitions, an sc_write_all
 *     with no loop would pass.
 *
 *   - The test uses no assert(). A failed check prints a sentence about the
 *     contract, not a line number in this file.
 */

#undef _FORTIFY_SOURCE      /* its inline read() would clash with the one below */

#include "sentinel/syscall.h"
#include "contact.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

#define TIMEOUT_S 10
#define PROGRAM_TIMEOUT_MS 5000   /* one run of demo_lab1 or contactfeed */
#define BIG       (1024 * 1024)

static char        g_dir[64];          /* the temporary directory of this run */
static const char *g_demo = "build/demo_lab1";
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
    if (g_msgfd >= 0 && write(g_msgfd, buf, (size_t)n) < 0) {
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

static void put_file(const char *path, const char *data, size_t len)
{
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0, "the test cannot create %s: %s", path, strerror(errno));
    size_t done = 0;
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
static char *get_file(const char *path, size_t *len)
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
    if (len != NULL) {
        *len = used;
    }
    return buf;
}

/* The permission bits of a file, from statx. */
static int mode_of(const char *path)
{
    struct statx st;
    CHECK(statx(AT_FDCWD, path, 0, STATX_MODE, &st) == 0, "%s does not exist", path);
    return (int)(st.stx_mode & 07777);
}

static int fd_is_open(int fd)
{
    return fcntl(fd, F_GETFD) != -1;
}

/* Entries in /proc/<pid>/fd. For our own process, the descriptor that
   opendir holds is in the list, so it is removed from the count. */
static int count_fds(pid_t pid)
{
    char path[64];
    if (pid == 0) {
        snprintf(path, sizeof path, "/proc/self/fd");
    } else {
        snprintf(path, sizeof path, "/proc/%d/fd", (int)pid);
    }
    DIR *d = opendir(path);
    CHECK(d != NULL, "the test cannot read %s: %s", path, strerror(errno));
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            n++;
        }
    }
    closedir(d);
    return pid == 0 ? n - 1 : n;
}

/* A decimal number at *p, with an optional '-'. Moves *p past it. */
static int take_long(const char **p, long *out)
{
    const char *s   = *p;
    int         neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    if (*s < '0' || *s > '9') {
        return -1;
    }
    long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    *out = neg ? -v : v;
    *p   = s;
    return 0;
}

/* The number after "<key>:" in /proc/self/status. */
static long status_field(const char *key)
{
    char *text = get_file("/proc/self/status", NULL);
    CHECK(text != NULL, "the test cannot read /proc/self/status");
    size_t klen = strlen(key);
    long   v    = -1;
    for (char *line = text; line != NULL && *line != '\0';) {
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            const char *p = line + klen + 1;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            take_long(&p, &v);
            break;
        }
        line = strchr(line, '\n');
        if (line != NULL) {
            line++;
        }
    }
    free(text);
    return v;
}

static void own_comm(char *buf, size_t n)
{
    char *text = get_file("/proc/self/comm", NULL);
    CHECK(text != NULL, "the test cannot read /proc/self/comm");
    text[strcspn(text, "\n")] = '\0';
    snprintf(buf, n, "%s", text);
    free(text);
}

/* Lines of text, split in place. A last line with no '\n' counts. */
static char **split_lines(char *text, size_t len, size_t *count)
{
    size_t cap = 64, n = 0;
    char **v   = malloc(cap * sizeof *v);
    CHECK(v != NULL, "the test is out of memory");
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len && i == start) {
            break;
        }
        if (i == len || text[i] == '\n') {
            if (n == cap) {
                cap *= 2;
                v = realloc(v, cap * sizeof *v);
                CHECK(v != NULL, "the test is out of memory");
            }
            text[i] = '\0';
            v[n++]  = text + start;
            start   = i + 1;
        }
    }
    *count = n;
    return v;
}

static void pause_us(long us)
{
    struct timespec t = { 0, us * 1000 };
    nanosleep(&t, NULL);
}

/* Run a program with its standard output and error in files. Returns its
   exit status, or 128 + the signal that stopped it. */
static int run_program(char *const argv[], const char *out_path, const char *err_path, pid_t *pid_out)
{
    /* fork() makes a copy of this process. The copy (pid 0) points its
       standard output and error at the two files with dup2(), and then
       execv() replaces it with the program. waitpid() below waits for the
       program to end. */
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0, "the test cannot fork: %s", strerror(errno));
    if (pid == 0) {
        int out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int err = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out < 0 || err < 0 || dup2(out, STDOUT_FILENO) < 0 || dup2(err, STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(out);
        close(err);
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid_out != NULL) {
        *pid_out = pid;
    }
    /* A program that never ends gets its own message, before the alarm of
       the check ends everything with a less exact one. */
    int status = 0;
    for (int waited_ms = 0;; waited_ms++) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            break;
        }
        CHECK(w == 0 || errno == EINTR, "the test cannot wait for %s: %s", argv[0], strerror(errno));
        if (waited_ms >= PROGRAM_TIMEOUT_MS) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            fail("%s did not end in %d s. A read or a loop does not stop at the end of the feed",
                 argv[0], PROGRAM_TIMEOUT_MS / 1000);
        }
        pause_us(1000);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return WEXITSTATUS(status);
}

/* NULL when line is "<seconds>.<6 digits> <LEVEL in 8 columns> <message>".
   Otherwise the rule that the line breaks. */
static const char *log_line_error(const char *line, char *level, size_t level_n, const char **msg)
{
    const char *p = line;
    if (*p < '0' || *p > '9') {
        return "it does not start with a timestamp";
    }
    while (*p >= '0' && *p <= '9') {
        p++;
    }
    if (*p != '.') {
        return "the timestamp has no '.'";
    }
    p++;
    for (int i = 0; i < 6; i++, p++) {
        if (*p < '0' || *p > '9') {
            return "the timestamp does not have 6 digits after the '.'";
        }
    }
    if (*p != ' ') {
        return "no single space follows the timestamp";
    }
    p++;
    size_t k = 0;
    while (k < 8 && p[k] >= 'A' && p[k] <= 'Z') {
        k++;
    }
    if (k == 0) {
        return "no level name follows the timestamp";
    }
    for (size_t i = k; i < 8; i++) {
        if (p[i] != ' ') {
            return "the level name is not left-aligned in 8 columns";
        }
    }
    if (p[8] != ' ') {
        return "one space does not follow the 8 columns of the level";
    }
    if (level != NULL) {
        snprintf(level, level_n, "%.*s", (int)k, p);
    }
    if (msg != NULL) {
        *msg = p + 9;
    }
    return NULL;
}

/* ---- io.c -------------------------------------------------------------- */

static void io_file_roundtrip(void)
{
    char path[128];
    path_of(path, sizeof path, "io.bin");

    char *out = malloc(BIG), *in = malloc(BIG);
    CHECK(out != NULL && in != NULL, "the test is out of memory");
    for (int i = 0; i < BIG; i++) {
        out[i] = (char)(i * 31 + 7);
    }

    int fd = sc_open_write(path, 0);
    CHECK(fd >= 0, "sc_open_write(\"%s\", 0) returned %d: %s", path, fd, strerror(errno));
    CHECK(sc_write_all(fd, out, BIG) == 0, "sc_write_all of 1 MB to a file did not return 0");
    CHECK(sc_close(fd) == 0, "sc_close did not return 0");

    size_t got = 0;
    char  *disk = get_file(path, &got);
    CHECK(disk != NULL && got == BIG, "the file holds %zu bytes after sc_write_all of %d", got, BIG);
    CHECK(memcmp(disk, out, BIG) == 0, "the file holds different bytes from the bytes written");
    free(disk);

    fd = sc_open_read(path);
    CHECK(fd >= 0, "sc_open_read returned %d for a file that exists", fd);
    ssize_t n = sc_read_full(fd, in, BIG);
    CHECK(n == BIG, "sc_read_full of a 1 MB file returned %zd, expected %d", n, BIG);
    CHECK(memcmp(in, out, BIG) == 0, "sc_read_full returned different bytes from the file");
    char tail[8];
    n = sc_read_full(fd, tail, sizeof tail);
    CHECK(n == 0, "sc_read_full at the end of the file returned %zd, expected 0", n);
    sc_close(fd);
}

/* ---- fault injection ---------------------------------------------------- */

static int g_inject;            /* 0: read() and write() behave normally */
static int g_injected_calls;

/* At most 1000 bytes per call, and every fourth call interrupted before any
   byte moves: what a signal does to a call that waits. The system call
   itself goes through syscall(), which does not come back here. */
static ssize_t injected(long number, int fd, void *buf, size_t n)
{
    if (g_inject && n > 0) {
        g_injected_calls++;
        if (g_injected_calls % 4 == 0) {
            errno = EINTR;
            return -1;
        }
        if (n > 1000) {
            n = 1000;
        }
    }
    return syscall(number, fd, buf, n);
}

ssize_t write(int fd, const void *buf, size_t n)
{
    return injected(SYS_write, fd, (void *)buf, n);
}

ssize_t read(int fd, void *buf, size_t n)
{
    return injected(SYS_read, fd, buf, n);
}

static void io_partial_write(void)
{
    char path[128];
    path_of(path, sizeof path, "partial.bin");
    char *out = malloc(BIG);
    CHECK(out != NULL, "the test is out of memory");
    for (int i = 0; i < BIG; i++) {
        out[i] = (char)(i % 251);
    }

    int fd = sc_open_write(path, 0);
    CHECK(fd >= 0, "sc_open_write returned %d", fd);
    errno              = 0;
    g_inject           = 1;
    g_injected_calls   = 0;
    int rc             = sc_write_all(fd, out, BIG);
    int e              = errno;
    int calls          = g_injected_calls;
    g_inject           = 0;
    close(fd);

    CHECK(calls > 0, "sc_write_all made no write() call that the test can see. Call write() directly");
    CHECK(!(rc == -1 && e == EINTR), "sc_write_all returned -1 with errno EINTR. "
          "A signal interrupted write(): that is not an error, so write again");
    size_t got  = 0;
    char  *disk = get_file(path, &got);
    CHECK(disk != NULL && got == BIG, "sc_write_all wrote %zu of %d bytes when write() moved at most 1000 bytes "
          "for each call. write() can move part of the buffer: go on until every byte is written", got, BIG);
    CHECK(memcmp(disk, out, BIG) == 0, "the file holds different bytes from the bytes written. "
          "After a partial write, the next write() starts where the last one stopped");
    CHECK(rc == 0, "sc_write_all wrote every byte but returned %d, not 0", rc);
}

static void io_partial_read(void)
{
    char path[128];
    path_of(path, sizeof path, "partial.bin");
    char *out = malloc(BIG), *in = malloc(BIG);
    CHECK(out != NULL && in != NULL, "the test is out of memory");
    for (int i = 0; i < BIG; i++) {
        out[i] = (char)(i * 7 + 3);
    }
    put_file(path, out, BIG);

    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0, "the test cannot open %s", path);
    errno            = 0;
    g_inject         = 1;
    g_injected_calls = 0;
    ssize_t n        = sc_read_full(fd, in, BIG);
    int     e        = errno;
    int     calls    = g_injected_calls;
    ssize_t end      = n == BIG ? sc_read_full(fd, in + BIG - 16, 16) : 0;
    g_inject         = 0;
    close(fd);

    CHECK(calls > 0, "sc_read_full made no read() call that the test can see. Call read() directly");
    CHECK(!(n == -1 && e == EINTR), "sc_read_full returned -1 with errno EINTR. "
          "A signal interrupted read(): that is not an error, so read again");
    CHECK(n == BIG, "sc_read_full returned %zd, expected %d, when read() returned at most 1000 bytes for each call. "
          "read() can return fewer bytes than asked before the end of the file", n, BIG);
    CHECK(memcmp(in, out, BIG) == 0, "sc_read_full put the bytes in the wrong place. "
          "After a short read, the next read() fills the buffer where the last one stopped");
    CHECK(end == 0, "sc_read_full at the end of the file returned %zd, expected 0", end);
}

static void io_append_truncate(void)
{
    char path[128];
    path_of(path, sizeof path, "append.txt");
    unlink(path);

    for (int i = 0; i < 3; i++) {
        int fd = sc_open_write(path, 1);
        CHECK(fd >= 0, "sc_open_write(path, 1) returned %d", fd);
        CHECK(sc_write_all(fd, "line\n", 5) == 0, "sc_write_all did not return 0");
        sc_close(fd);
    }
    size_t len  = 0;
    char  *text = get_file(path, &len);
    CHECK(text != NULL && len == 15, "3 opens with append=1, 5 bytes each, left %zu bytes, expected 15", len);
    free(text);

    int fd = sc_open_write(path, 0);
    CHECK(fd >= 0, "sc_open_write(path, 0) returned %d", fd);
    sc_close(fd);
    text = get_file(path, &len);
    CHECK(text != NULL && len == 0, "sc_open_write with append=0 left %zu old bytes, expected 0", len);
    free(text);
}

static void io_modes(void)
{
    char path[128];
    path_of(path, sizeof path, "mode.txt");

    umask(0);                      /* so the umask cannot change the result */
    unlink(path);
    int fd = sc_open_write(path, 0);
    CHECK(fd >= 0, "sc_open_write returned %d", fd);
    sc_close(fd);
    int m = mode_of(path);
    CHECK(m == 0644, "sc_open_write made a file with mode %04o, expected 0644", m);

    unlink(path);
    fd = sc_open_write_mode(path, 0, 0600);
    CHECK(fd >= 0, "sc_open_write_mode returned %d", fd);
    sc_close(fd);
    m = mode_of(path);
    CHECK(m == 0600, "sc_open_write_mode(path, 0, 0600) made a file with mode %04o", m);
}

static void io_errors(void)
{
    char b[4];
    errno = 0;
    int fd = sc_open_read("/nonexistent/sentinel/feed.txt");
    CHECK(fd == -1 && errno == ENOENT,
          "sc_open_read of a missing file returned %d with errno %d, expected -1 and ENOENT", fd, errno);

    errno = 0;
    int rc = sc_write_all(-1, b, sizeof b);
    CHECK(rc == -1 && errno == EBADF, "sc_write_all(-1, ...) returned %d with errno %d, expected -1 and EBADF", rc, errno);
    errno = 0;
    ssize_t n = sc_read_full(-1, b, sizeof b);
    CHECK(n == -1 && errno == EBADF, "sc_read_full(-1, ...) returned %zd with errno %d, expected -1 and EBADF", n, errno);
    CHECK(sc_write_all(-1, b, 0) == 0, "sc_write_all with n == 0 must return 0, whatever fd is");
    CHECK(sc_read_full(-1, b, 0) == 0, "sc_read_full with n == 0 must return 0, whatever fd is");

    /* The next call still works. */
    char path[128];
    path_of(path, sizeof path, "recover.txt");
    fd = sc_open_write(path, 0);
    CHECK(fd >= 0, "after an error, sc_open_write returned %d", fd);
    CHECK(sc_write_all(fd, "ok\n", 3) == 0, "after an error, sc_write_all did not return 0");
    sc_close(fd);
    fd = sc_open_read(path);
    CHECK(fd >= 0, "after an error, sc_open_read returned %d", fd);
    CHECK(sc_read_full(fd, b, 3) == 3 && memcmp(b, "ok\n", 3) == 0, "after an error, sc_read_full did not read the file");
    sc_close(fd);
}

static void io_close(void)
{
    char path[128];
    path_of(path, sizeof path, "close.txt");
    int fd = sc_open_write(path, 0);
    CHECK(fd >= 0, "sc_open_write returned %d", fd);
    CHECK(sc_close(fd) == 0, "sc_close of an open descriptor did not return 0");
    CHECK(!fd_is_open(fd), "descriptor %d is still open after sc_close", fd);

    errno = 0;
    int rc = sc_close(-1);
    CHECK(rc == -1 && errno == EBADF, "sc_close(-1) returned %d with errno %d, expected -1 and EBADF", rc, errno);
}

/* ---- clock.c ----------------------------------------------------------- */

static int64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void clock_monotonic(void)
{
    int64_t mine = mono_ns();
    int64_t prev = sc_now_ns();
    int64_t diff = prev - mine;
    CHECK(diff > -1000000000 && diff < 1000000000,
          "sc_now_ns is %lld s away from CLOCK_MONOTONIC. A duration must use the clock that never goes back",
          (long long)(diff / 1000000000));

    for (int i = 0; i < 10000; i++) {
        int64_t now = sc_now_ns();
        CHECK(now >= prev, "sc_now_ns went back by %lld ns", (long long)(prev - now));
        prev = now;
    }

    struct timespec req = { 0, 50 * 1000 * 1000 };
    int64_t t0 = sc_now_ns();
    nanosleep(&req, NULL);
    int64_t ms = (sc_now_ns() - t0) / 1000000;
    CHECK(ms >= 50 && ms < 1000, "a sleep of 50 ms measured %lld ms with sc_now_ns", (long long)ms);
}

static void clock_wall(void)
{
    struct timespec real;
    clock_gettime(CLOCK_REALTIME, &real);
    int64_t sec  = 0;
    int32_t usec = -1;
    sc_now_wall(&sec, &usec);
    int64_t diff = sec - (int64_t)real.tv_sec;
    CHECK(diff >= -2 && diff <= 2, "sc_now_wall gave %lld seconds; the date is %lld", (long long)sec, (long long)real.tv_sec);
    CHECK(usec >= 0 && usec <= 999999, "sc_now_wall gave %d microseconds, expected 0 to 999999", (int)usec);

    sc_now_wall(&sec, NULL);
    sc_now_wall(NULL, &usec);
    sc_now_wall(NULL, NULL);
}

static void clock_stamp(void)
{
    char buf[SC_STAMP_MAX];
    memset(buf, '#', sizeof buf);
    int n = sc_stamp(buf, sizeof buf);
    CHECK(n > 0 && (size_t)n < sizeof buf, "sc_stamp returned %d", n);
    CHECK((size_t)n == strlen(buf), "sc_stamp returned %d, but the string has %zu characters", n, strlen(buf));

    const char *p = buf;
    long        sec;
    CHECK(take_long(&p, &sec) == 0 && *p == '.', "sc_stamp wrote \"%s\", expected <seconds>.<6 digits>", buf);
    p++;
    for (int i = 0; i < 6; i++) {
        CHECK(p[i] >= '0' && p[i] <= '9', "sc_stamp wrote \"%s\": the part after the point must be 6 digits", buf);
    }
    CHECK(p[6] == '\0', "sc_stamp wrote \"%s\": the part after the point must be 6 digits", buf);
    long now = (long)time(NULL);
    CHECK(sec >= now - 2 && sec <= now + 2, "sc_stamp wrote %ld seconds; the date is %ld", sec, now);

    char tiny[4];
    CHECK(sc_stamp(tiny, sizeof tiny) == -1, "sc_stamp into a 4-byte buffer must return -1");
}

static void proc_ctxt(void)
{
    proc_info_t before;
    CHECK(proc_report(0, &before) == 0, "proc_report(0) did not return 0");
    CHECK(before.vol_ctxt >= 0 && before.invol_ctxt >= 0,
          "proc_report gave vol_ctxt %ld and invol_ctxt %ld", before.vol_ctxt, before.invol_ctxt);

    /* A sleep is a voluntary switch: the process asks to wait. Ten of them
       must move the counter. The involuntary count is not asserted, because
       whether the kernel preempts this process depends on what else runs. */
    for (int i = 0; i < 10; i++) {
        struct timespec req = { .tv_sec = 0, .tv_nsec = 2000000 };
        nanosleep(&req, NULL);
    }

    proc_info_t after;
    CHECK(proc_report(0, &after) == 0, "proc_report(0) did not return 0");
    CHECK(after.vol_ctxt > before.vol_ctxt,
          "after ten sleeps, vol_ctxt went from %ld to %ld. Read voluntary_ctxt_switches "
          "from /proc/<pid>/status", before.vol_ctxt, after.vol_ctxt);
    CHECK(after.invol_ctxt >= before.invol_ctxt,
          "invol_ctxt fell from %ld to %ld, which a counter cannot do. Check that "
          "nonvoluntary_ctxt_switches is not being read as voluntary_ctxt_switches",
          before.invol_ctxt, after.invol_ctxt);
}

/* ---- log.c ------------------------------------------------------------- */

static void log_format(void)
{
    char path[128];
    path_of(path, sizeof path, "format.log");
    unlink(path);

    log_t *lg = log_open(path);
    CHECK(lg != NULL, "log_open returned NULL: %s", strerror(errno));
    CHECK(log_writef(lg, LOG_SYSTEM, "SENTINEL starting") == 0, "log_writef did not return 0");
    CHECK(log_writef(lg, LOG_INFO, "contact %s from %s", "ACCF-4587", "NWS") == 0, "log_writef did not return 0");
    CHECK(log_writef(lg, LOG_PRIORITY, "speed %.1f", 420.0) == 0, "log_writef did not return 0");
    CHECK(log_close(lg) == 0, "log_close did not return 0");

    size_t len  = 0;
    char  *text = get_file(path, &len);
    CHECK(text != NULL, "log_open did not create %s", path);
    CHECK(len > 0 && text[len - 1] == '\n', "the log does not end with '\\n'");
    size_t n     = 0;
    char **lines = split_lines(text, len, &n);
    CHECK(n == 3, "3 calls to log_writef wrote %zu lines", n);

    static const char *const want_level[] = { "SYSTEM", "INFO", "PRIORITY" };
    static const char *const want_msg[]   = { "SENTINEL starting", "contact ACCF-4587 from NWS", "speed 420.0" };
    for (size_t i = 0; i < 3; i++) {
        char        level[16];
        const char *msg = NULL;
        const char *why = log_line_error(lines[i], level, sizeof level, &msg);
        CHECK(why == NULL, "log line %zu, \"%s\": %s", i + 1, lines[i], why);
        CHECK(strcmp(level, want_level[i]) == 0, "log line %zu has level %s, expected %s", i + 1, level, want_level[i]);
        CHECK(strcmp(msg, want_msg[i]) == 0, "log line %zu has message \"%s\", expected \"%s\"", i + 1, msg, want_msg[i]);
    }
}

static void log_levels(void)
{
    static const char *const names[] = { "SYSTEM", "INFO", "PENDING", "PRIORITY", "ALERT", "TRACK" };
    for (int i = 0; i < 6; i++) {
        const char *got = log_level_name((log_level_t)i);
        CHECK(got != NULL && strcmp(got, names[i]) == 0,
              "log_level_name(%d) gave \"%s\", expected \"%s\"", i, got ? got : "(NULL)", names[i]);
    }
    CHECK(log_level_name((log_level_t)99) == NULL, "log_level_name(99) must return NULL");
    CHECK(log_level_name((log_level_t)-1) == NULL, "log_level_name(-1) must return NULL");

    char path[128];
    path_of(path, sizeof path, "levels.log");
    log_t *lg = log_open(path);
    CHECK(lg != NULL, "log_open returned NULL");
    errno = 0;
    int rc = log_writef(lg, (log_level_t)99, "bad level");
    CHECK(rc == -1 && errno == EINVAL, "log_writef with level 99 returned %d with errno %d, expected -1 and EINVAL", rc, errno);
    CHECK(log_writef(lg, LOG_INFO, "good") == 0, "after a bad level, log_writef did not return 0");
    log_close(lg);
}

static void log_appends(void)
{
    char path[128];
    path_of(path, sizeof path, "append.log");
    unlink(path);

    for (int i = 0; i < 2; i++) {
        log_t *lg = log_open(path);
        CHECK(lg != NULL, "log_open returned NULL");
        CHECK(log_writef(lg, LOG_INFO, "run %d", i) == 0, "log_writef did not return 0");
        log_close(lg);
    }
    size_t len = 0, n = 0;
    char  *text = get_file(path, &len);
    CHECK(text != NULL, "log_open did not create the log");
    split_lines(text, len, &n);
    CHECK(n == 2, "two runs of log_open and log_writef left %zu lines, expected 2. "
          "A second open must not destroy the first run's lines", n);
}

static void log_private(void)
{
    char path[128];
    path_of(path, sizeof path, "private.log");
    umask(022);

    unlink(path);
    log_t *lg = log_open(path);
    CHECK(lg != NULL, "log_open returned NULL");
    log_close(lg);
    int m = mode_of(path);
    CHECK(m == 0600, "log_open made a new log with mode %04o, expected 0600", m);

    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0, "the test cannot create %s", path);
    close(fd);
    CHECK(chmod(path, 0644) == 0, "the test cannot chmod %s", path);
    lg = log_open(path);
    CHECK(lg != NULL, "log_open of an existing log returned NULL");
    log_close(lg);
    m = mode_of(path);
    CHECK(m == 0600, "an existing log with mode 0644 is %04o after log_open, expected 0600. "
          "The mode of open() applies only when it creates the file", m);
}

static void log_concurrent(void)
{
    enum { WRITERS = 4, LINES = 500 };
    char path[128];
    path_of(path, sizeof path, "concurrent.log");
    unlink(path);

    /* Four child processes, made with fork(), write to the same log at the
       same time. wait() collects each one and its exit status. */
    fflush(NULL);
    for (int w = 0; w < WRITERS; w++) {
        pid_t pid = fork();
        CHECK(pid >= 0, "the test cannot fork");
        if (pid == 0) {
            log_t *lg = log_open(path);
            if (lg == NULL) {
                _exit(2);
            }
            for (int i = 0; i < LINES; i++) {
                if (log_writef(lg, LOG_INFO, "writer=%d seq=%04d payload=%s", w, i,
                               "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX") != 0) {
                    _exit(3);
                }
            }
            _exit(log_close(lg) == 0 ? 0 : 4);
        }
    }
    for (int w = 0; w < WRITERS; w++) {
        int status = 0;
        CHECK(wait(&status) > 0, "the test cannot wait for a writer");
        CHECK(WIFEXITED(status), "a writer process crashed in log_writef");
        CHECK(WEXITSTATUS(status) == 0, "a writer process failed: %s",
              WEXITSTATUS(status) == 2 ? "log_open returned NULL" :
              WEXITSTATUS(status) == 3 ? "log_writef did not return 0" : "log_close did not return 0");
    }

    size_t len = 0, n = 0;
    char  *text  = get_file(path, &len);
    CHECK(text != NULL, "the writers did not create the log");
    char **lines = split_lines(text, len, &n);
    static char seen[WRITERS][LINES];
    long        whole = 0;
    for (size_t i = 0; i < n; i++) {
        const char *msg = NULL;
        const char *why = log_line_error(lines[i], NULL, 0, &msg);
        CHECK(why == NULL, "line %zu of the log is damaged (%s): \"%.80s\". Two writes mixed", i + 1, why, lines[i]);
        const char *p = msg;
        long        wr, seq;
        CHECK(strncmp(p, "writer=", 7) == 0, "line %zu of the log is damaged: \"%.80s\"", i + 1, lines[i]);
        p += 7;
        CHECK(take_long(&p, &wr) == 0 && strncmp(p, " seq=", 5) == 0, "line %zu of the log is damaged", i + 1);
        p += 5;
        CHECK(take_long(&p, &seq) == 0 && wr >= 0 && wr < WRITERS && seq >= 0 && seq < LINES,
              "line %zu of the log is damaged", i + 1);
        CHECK(strcmp(p, " payload=XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX") == 0, "line %zu of the log is damaged", i + 1);
        CHECK(!seen[wr][seq], "writer %ld line %ld is in the log two times", wr, seq);
        seen[wr][seq] = 1;
        whole++;
    }
    CHECK(whole == WRITERS * LINES, "%d processes wrote %d lines each, and the log holds %ld whole lines",
          WRITERS, LINES, whole);
}

static void log_long_message(void)
{
    char path[128];
    path_of(path, sizeof path, "long.log");
    unlink(path);

    char huge[4096];
    memset(huge, 'Z', sizeof huge - 1);
    huge[sizeof huge - 1] = '\0';

    log_t *lg = log_open(path);
    CHECK(lg != NULL, "log_open returned NULL");
    CHECK(log_writef(lg, LOG_INFO, "%s", huge) == 0, "log_writef of a 4095-byte message did not return 0: a long message is cut, not refused");
    CHECK(log_writef(lg, LOG_TRACK, "after a long line") == 0, "after a long message, log_writef did not return 0");
    log_close(lg);

    size_t len = 0, n = 0;
    char  *text = get_file(path, &len);
    CHECK(text != NULL, "log_open did not create the log");
    char *nl = memchr(text, '\n', len);
    CHECK(nl != NULL, "the log has no '\\n'");
    size_t first = (size_t)(nl - text) + 1;
    CHECK(first <= 512, "the long message gave a line of %zu bytes; a line is at most 512, with its '\\n'", first);
    char **lines = split_lines(text, len, &n);
    CHECK(n == 2, "a long message and a short one gave %zu lines, expected 2", n);
    const char *msg = NULL;
    const char *why = log_line_error(lines[0], NULL, 0, &msg);
    CHECK(why == NULL, "the cut line breaks the format: %s", why);
    why = log_line_error(lines[1], NULL, 0, &msg);
    CHECK(why == NULL && strcmp(msg, "after a long line") == 0, "the line after the long message is damaged");
}

static void log_errors(void)
{
    CHECK(log_close(NULL) == 0, "log_close(NULL) must return 0");
    errno = 0;
    int rc = log_writef(NULL, LOG_INFO, "x");
    CHECK(rc == -1 && errno == EINVAL, "log_writef(NULL, ...) returned %d with errno %d, expected -1 and EINVAL", rc, errno);

    char path[160];
    path_of(path, sizeof path, "no-such-dir/x.log");
    CHECK(log_open(path) == NULL, "log_open in a directory that does not exist must return NULL");

    path_of(path, sizeof path, "after.log");
    log_t *lg = log_open(path);
    CHECK(lg != NULL, "after a failed log_open, the next log_open returned NULL");
    CHECK(log_writef(lg, LOG_INFO, "still works") == 0, "after a failed log_open, log_writef did not return 0");
    log_close(lg);
}

/* ---- proc.c ------------------------------------------------------------ */

static void proc_self(void)
{
    proc_info_t info;
    memset(&info, 0, sizeof info);
    CHECK(proc_report(0, &info) == 0, "proc_report(0, &info) returned -1: %s", strerror(errno));
    long threads = status_field("Threads");
    char comm[64];
    own_comm(comm, sizeof comm);

    CHECK(info.pid == getpid(), "pid is %d, expected %d", (int)info.pid, (int)getpid());
    CHECK(info.ppid == getppid(), "ppid is %d, expected %d", (int)info.ppid, (int)getppid());
    CHECK(strcmp(info.name, comm) == 0, "name is \"%s\", expected \"%s\"", info.name, comm);
    CHECK(info.threads == threads, "threads is %d, expected %ld", info.threads, threads);
    CHECK(info.vm_rss_kb > 0, "vm_rss is %ld kB, expected more than 0", info.vm_rss_kb);
    CHECK(info.vm_size_kb >= info.vm_rss_kb, "vm_size (%ld kB) is less than vm_rss (%ld kB)", info.vm_size_kb, info.vm_rss_kb);

    proc_info_t byid;
    CHECK(proc_report(getpid(), &byid) == 0, "proc_report(getpid(), &info) returned -1");
    CHECK(byid.pid == info.pid && strcmp(byid.name, info.name) == 0,
          "proc_report(getpid()) and proc_report(0) give different processes");
}

static void proc_fds(void)
{
    proc_info_t before, during, after;
    CHECK(proc_report(0, &before) == 0, "proc_report returned -1");
    int real = count_fds(0);
    CHECK(before.open_fds == real, "open_fds is %d; /proc/self/fd holds %d, without the descriptor that proc_report opens",
          before.open_fds, real);

    int a = open("/proc/self/status", O_RDONLY);
    int b = open("/proc/self/status", O_RDONLY);
    CHECK(a >= 0 && b >= 0, "the test cannot open two files");
    CHECK(proc_report(0, &during) == 0, "proc_report returned -1");
    CHECK(during.open_fds == before.open_fds + 2, "after two opens, open_fds went from %d to %d", before.open_fds, during.open_fds);
    close(a);
    close(b);
    CHECK(proc_report(0, &after) == 0, "proc_report returned -1");
    CHECK(after.open_fds == before.open_fds, "after two closes, open_fds is %d, expected %d", after.open_fds, before.open_fds);
}

static void proc_other(void)
{
    /* A child process opens two files, and stops itself with SIGSTOP, so its
       descriptors stay open while this process counts them. waitpid() with
       WUNTRACED returns when the child has stopped. kill() ends the child
       after the count. */
    fflush(NULL);
    pid_t child = fork();
    CHECK(child >= 0, "the test cannot fork");
    if (child == 0) {
        int x = open("/proc/self/status", O_RDONLY);
        int y = open("/proc/self/status", O_RDONLY);
        if (x < 0 || y < 0) {
            _exit(1);
        }
        raise(SIGSTOP);             /* hold the descriptors while the parent counts */
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status), "the child process did not start");

    int         expect = count_fds(child);
    proc_info_t info;
    int         rc = proc_report(child, &info);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    CHECK(rc == 0, "proc_report(<child pid>) returned -1: %s", strerror(errno));
    CHECK(info.pid == child, "pid is %d, expected the child %d", (int)info.pid, (int)child);
    CHECK(info.ppid == getpid(), "ppid is %d, expected %d", (int)info.ppid, (int)getpid());
    CHECK(info.open_fds == expect, "open_fds of the child is %d; /proc/%d/fd holds %d. "
          "Only the calling process has a descriptor of proc_report to remove",
          info.open_fds, (int)child, expect);
}

static void proc_errors(void)
{
    proc_info_t info;
    errno = 0;
    int rc = proc_report(0, NULL);
    CHECK(rc == -1 && errno == EINVAL, "proc_report(0, NULL) returned %d with errno %d, expected -1 and EINVAL", rc, errno);
    rc = proc_report(4194304 + 1, &info);
    CHECK(rc == -1, "proc_report of a pid with no process returned %d, expected -1", rc);
    CHECK(proc_report(0, &info) == 0 && info.pid == getpid(), "after a failed report, proc_report(0) did not work");
}

static void proc_write(void)
{
    proc_info_t info;
    memset(&info, 0, sizeof info);
    info.pid        = 4321;
    info.ppid       = 1234;
    snprintf(info.name, sizeof info.name, "demo_lab1");
    info.vm_size_kb = 2600;
    info.vm_rss_kb  = 1100;
    info.threads    = 1;
    info.open_fds   = 4;
    info.vol_ctxt   = 87;
    info.invol_ctxt = 12;

    char path[128];
    path_of(path, sizeof path, "report.txt");
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0, "the test cannot create %s", path);
    CHECK(proc_report_write(fd, &info) == 0, "proc_report_write did not return 0");
    close(fd);

    static const char want[] =
        "process report\n"
        "  pid        4321\n"
        "  ppid       1234\n"
        "  name       demo_lab1\n"
        "  vm_size    2600 kB\n"
        "  vm_rss     1100 kB\n"
        "  threads    1\n"
        "  open_fds   4\n"
        "  ctxt_vol   87\n"
        "  ctxt_invol 12\n";
    char *got = get_file(path, NULL);
    CHECK(got != NULL && strcmp(got, want) == 0,
          "proc_report_write wrote:\n%s\n        expected the layout in syscall.h:\n%s", got ? got : "", want);

    errno = 0;
    int rc = proc_report_write(fd, NULL);
    CHECK(rc == -1 && errno == EINVAL, "proc_report_write(fd, NULL) returned %d with errno %d, expected -1 and EINVAL", rc, errno);
}

/* Append to a file that a reader already holds open. The demo's --follow
   check needs this: the point is that bytes arriving after the reader reached
   the end are still read. Plain libc, so a broken student function cannot
   make the appending itself fail. */
static void append_file(const char *path, const char *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0, "the test cannot open %s to append: %s", path, strerror(errno));
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, data + done, len - done);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        CHECK(w > 0, "the test cannot append to %s: %s", path, strerror(errno));
        done += (size_t)w;
    }
    close(fd);
}

/* ---- demo_lab1, as a black box ----------------------------------------- */

struct demo {
    int     status;
    pid_t   pid;
    char    feed[128];
    char    log[128];
    char   *out;
    char   *logtext;
    char  **lines;
    size_t  nlines;
};

static void need_demo(void)
{
    CHECK(access(g_demo, X_OK) == 0, "%s does not exist. Your src/libsyscall/Makefile builds it: see Section 4.1 of "
          "docs/lab1/lab1_brief.pdf", g_demo);
}

static void run_demo(struct demo *d, const char *feed, const char *log)
{
    need_demo();
    snprintf(d->feed, sizeof d->feed, "%s", feed);
    snprintf(d->log, sizeof d->log, "%s", log);
    unlink(log);

    char out[128], err[128];
    path_of(out, sizeof out, "demo.out");
    path_of(err, sizeof err, "demo.err");
    char *argv[] = { (char *)g_demo, d->feed, d->log, NULL };
    d->status = run_program(argv, out, err, &d->pid);
    CHECK(d->status < 128, "demo_lab1 crashed: signal %d (%s)", d->status - 128, strsignal(d->status - 128));
    d->out = get_file(out, NULL);

    size_t len = 0;
    d->logtext = get_file(log, &len);
    d->lines   = NULL;
    d->nlines  = 0;
    if (d->logtext != NULL) {
        CHECK(len == 0 || d->logtext[len - 1] == '\n', "the last line of the mission log has no '\\n'");
        d->lines = split_lines(d->logtext, len, &d->nlines);
    }
}

/* The message that Section 3.3 of the brief gives for each line of a feed. */
struct expect {
    char **msg;
    size_t n;
    long   read, good, bad, prio;
};

static void expect_log(const char *feed_text, size_t feed_len, struct expect *e)
{
    char  *copy = malloc(feed_len + 1);
    CHECK(copy != NULL, "the test is out of memory");
    memcpy(copy, feed_text, feed_len);
    copy[feed_len] = '\0';
    size_t n     = 0;
    char **lines = split_lines(copy, feed_len, &n);

    e->msg  = malloc((n + 1) * sizeof *e->msg);
    CHECK(e->msg != NULL, "the test is out of memory");
    e->n    = n;
    e->read = e->good = e->bad = e->prio = 0;
    for (size_t i = 0; i < n; i++) {
        char      buf[256];
        contact_t c;
        e->read++;
        contact_err_t err = contact_parse(lines[i], strlen(lines[i]), &c);
        if (err != CONTACT_OK) {
            e->bad++;
            snprintf(buf, sizeof buf, "INFO|rejected line %zu: %s", i + 1, contact_strerror(err));
        } else if (c.flag == PRIORITY_PRIORITY) {
            e->good++;
            e->prio++;
            snprintf(buf, sizeof buf, "PRIORITY|priority contact %s from %s at %.4f,%.4f speed %.1f",
                     c.id, c.sensor, c.lat, c.lon, c.speed);
        } else {
            e->good++;
            snprintf(buf, sizeof buf, "INFO|contact %s from %s", c.id, c.sensor);
        }
        e->msg[i] = strdup(buf);
        CHECK(e->msg[i] != NULL, "the test is out of memory");
    }
    free(lines);
    free(copy);
}

/* Compare log lines [3, 3 + e->n) with the expected messages. */
static void compare_body(const struct demo *d, const struct expect *e)
{
    CHECK(d->nlines >= 2, "the mission log has %zu lines, expected %zu", d->nlines, e->n + 3);
    for (size_t i = 0; i < e->n; i++) {
        const char *want = e->msg[i];
        const char *bar  = strchr(want, '|');
        CHECK(2 + i < d->nlines, "the mission log ends after %zu lines; feed line %zu has no log line. Expected: %s",
              d->nlines, i + 1, bar + 1);
        const char *got   = d->lines[2 + i];
        char        level[16];
        const char *msg   = NULL;
        const char *why   = log_line_error(got, level, sizeof level, &msg);
        CHECK(why == NULL, "log line %zu, \"%.80s\": %s", i + 3, got, why);
        int same_level = strlen(level) == (size_t)(bar - want) && strncmp(level, want, (size_t)(bar - want)) == 0;
        CHECK(same_level && strcmp(msg, bar + 1) == 0,
              "log line %zu is for feed line %zu.\n        got       %s %s\n        expected  %.*s %s",
              i + 3, i + 1, level, msg, (int)(bar - want), want, bar + 1);
    }
    CHECK(d->nlines == e->n + 3, "the mission log has %zu lines, expected %zu: the start line, the process line, "
          "one line for each of the %zu feed lines, and the last line", d->nlines, e->n + 3, e->n);
}

/* 9 lines: 6 accepted, 2 of them priority, 3 rejected. The 5000-byte line is
   "line too long" whatever the student's reader limit is: the reader refuses
   it, or contact_parse does. The last line has no '\n'. */
static char *prepared_feed(size_t *len)
{
    static const char head[] =
        "1755534061.482913 NWS ACCF-4587 68.7100 -105.7000 420.0 P\n"
        "1755534061.501244 AIS VESL-0912 48.3000 -123.4000 12.0 R\n"
        "1755534061.600000 XYZ ACCF-0001 60.0000 -100.0000 300.0 R\n"
        "1755534061.700000 AUR ACCF-0134 66.1049 -119.6736 226.7 R\n";
    static const char tail[] =
        "1755534061.800000 SAT ACCF-0294 66.1049 -119.6736 226.7 Q\n"
        "1755534061.900000 NWS ACCF-0777 70.0000 -110.0000 480.5 P\n"
        "1755534062.000000 AIS VESL-0001 49.0000 -124.0000 8.0 R\n"
        "1755534062.100000 SAT SHIP-0042 51.2500 -130.5000 15.5 R";
    size_t hn = sizeof head - 1, tn = sizeof tail - 1, big = 5000;
    char  *buf = malloc(hn + big + 1 + tn + 1);
    CHECK(buf != NULL, "the test is out of memory");
    memcpy(buf, head, hn);
    memset(buf + hn, 'X', big);
    buf[hn + big] = '\n';
    memcpy(buf + hn + big + 1, tail, tn);
    *len = hn + big + 1 + tn;
    buf[*len] = '\0';
    return buf;
}

static void run_prepared(struct demo *d, struct expect *e)
{
    size_t len  = 0;
    char  *feed = prepared_feed(&len);
    char   fpath[128], lpath[128];
    path_of(fpath, sizeof fpath, "feed.txt");
    path_of(lpath, sizeof lpath, "missions.log");
    put_file(fpath, feed, len);
    expect_log(feed, len, e);
    free(feed);
    run_demo(d, fpath, lpath);
    CHECK(d->logtext != NULL, "demo_lab1 wrote no mission log at %s", lpath);
}

static void demo_usage(void)
{
    need_demo();
    char out[128], err[128];
    path_of(out, sizeof out, "demo.out");
    path_of(err, sizeof err, "demo.err");
    char *argv[] = { (char *)g_demo, NULL };
    int   st     = run_program(argv, out, err, NULL);
    CHECK(st == 2, "demo_lab1 with no argument exited with status %d, expected 2", st);
}

static void demo_cannot_open(void)
{
    struct demo d;
    char        fpath[128], lpath[160];
    path_of(fpath, sizeof fpath, "no-such-feed.txt");
    path_of(lpath, sizeof lpath, "missions.log");
    run_demo(&d, fpath, lpath);
    CHECK(d.status == 1, "demo_lab1 with a feed that does not exist exited with status %d, expected 1", d.status);

    static const char one[] = "1755534061.482913 NWS ACCF-4587 68.7100 -105.7000 420.0 P\n";
    path_of(fpath, sizeof fpath, "feed.txt");
    put_file(fpath, one, sizeof one - 1);
    path_of(lpath, sizeof lpath, "no-such-dir/missions.log");
    run_demo(&d, fpath, lpath);
    CHECK(d.status == 1, "demo_lab1 with a mission log in a directory that does not exist exited with status %d, expected 1",
          d.status);
}

static void demo_status_and_mode(void)
{
    struct demo   d;
    struct expect e;
    run_prepared(&d, &e);
    CHECK(d.status == 0, "demo_lab1 on a feed with accepted lines exited with status %d, expected 0", d.status);
    int m = mode_of(d.log);
    CHECK(m == 0600, "the mission log of demo_lab1 has mode %04o, expected 0600", m);
}

static void demo_first_lines(void)
{
    struct demo   d;
    struct expect e;
    run_prepared(&d, &e);
    CHECK(d.nlines >= 2, "the mission log has %zu lines, expected the start line and the process line first", d.nlines);

    char        want[256];
    const char *msg = NULL;
    char        level[16];
    const char *why = log_line_error(d.lines[0], level, sizeof level, &msg);
    CHECK(why == NULL, "log line 1, \"%.80s\": %s", d.lines[0], why);
    snprintf(want, sizeof want, "SENTINEL Part 1 starting, feed=%s", d.feed);
    CHECK(strcmp(level, "SYSTEM") == 0 && strcmp(msg, want) == 0,
          "log line 1 is \"%s %s\", expected \"SYSTEM %s\"", level, msg, want);

    why = log_line_error(d.lines[1], level, sizeof level, &msg);
    CHECK(why == NULL, "log line 2, \"%.80s\": %s", d.lines[1], why);
    const char *base = strrchr(g_demo, '/');
    base = base ? base + 1 : g_demo;
    int n = snprintf(want, sizeof want, "pid=%d ppid=%d name=%.15s open_fds=", (int)d.pid, (int)getpid(), base);
    const char *p = msg + (strncmp(msg, want, (size_t)n) == 0 ? (size_t)n : 0);
    long        fds;
    CHECK(strcmp(level, "SYSTEM") == 0 && p != msg && take_long(&p, &fds) == 0 && *p == '\0' && fds >= 3,
          "log line 2 is \"%s %s\", expected \"SYSTEM %s<n>\", with the pid of the demo process, and n of 3 or more",
          level, msg, want);
}

static void demo_body(void)
{
    struct demo   d;
    struct expect e;
    run_prepared(&d, &e);
    CHECK(e.read == 9 && e.good == 6 && e.bad == 3 && e.prio == 2, "the test feed is wrong: the given contact_parse changed");
    compare_body(&d, &e);
}

static void check_last_line(const struct demo *d, const struct expect *e)
{
    CHECK(d->nlines > 0, "the mission log is empty");
    const char *last = d->lines[d->nlines - 1];
    const char *msg  = NULL;
    char        level[16];
    const char *why  = log_line_error(last, level, sizeof level, &msg);
    CHECK(why == NULL, "the last log line, \"%.80s\": %s", last, why);

    char want[160];
    int  n = snprintf(want, sizeof want, "Part 1 complete: %ld read, %ld accepted, %ld rejected, %ld priority, ",
                      e->read, e->good, e->bad, e->prio);
    int ok = strcmp(level, "SYSTEM") == 0 && strncmp(msg, want, (size_t)n) == 0;
    if (ok) {
        const char *p = msg + n;
        long        whole;
        ok = take_long(&p, &whole) == 0 && whole >= 0 && p[0] == '.';
        if (ok) {
            ok = p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9' &&
                 strcmp(p + 4, " ms") == 0;
        }
    }
    CHECK(ok, "the last log line is \"%s %s\", expected \"SYSTEM %s<ms> ms\", with 3 digits after the point",
          level, msg, want);
}

static void demo_last_line(void)
{
    struct demo   d;
    struct expect e;
    run_prepared(&d, &e);
    check_last_line(&d, &e);
}

static void demo_format(void)
{
    struct demo   d;
    struct expect e;
    run_prepared(&d, &e);
    CHECK(d.nlines > 0, "the mission log is empty");
    for (size_t i = 0; i < d.nlines; i++) {
        size_t len = strlen(d.lines[i]) + 1;
        CHECK(len <= 512, "log line %zu is %zu bytes with its '\\n'; a line is at most 512", i + 1, len);
        const char *why = log_line_error(d.lines[i], NULL, 0, NULL);
        CHECK(why == NULL, "log line %zu, \"%.80s\": %s", i + 1, d.lines[i], why);
    }
}

static void demo_balanced(void)
{
    struct demo   d;
    struct expect e;
    run_prepared(&d, &e);
    CHECK(d.out != NULL, "the test cannot read the output of demo_lab1");
    const char *p = strstr(d.out, "open_fds before=");
    CHECK(p != NULL, "the output of demo_lab1 has no line \"open_fds before=<a> after=<b> (balanced)\"");
    p += strlen("open_fds before=");
    long a, b;
    CHECK(take_long(&p, &a) == 0 && strncmp(p, " after=", 7) == 0, "the open_fds line does not have the form \"open_fds before=<a> after=<b> (balanced)\"");
    p += 7;
    CHECK(take_long(&p, &b) == 0, "the open_fds line does not have the form \"open_fds before=<a> after=<b> (balanced)\"");
    CHECK(a == b, "demo_lab1 prints open_fds before=%ld after=%ld: a descriptor leaked", a, b);
    CHECK(strncmp(p, " (balanced)", 11) == 0, "the open_fds line has equal counts, but it does not end with \" (balanced)\"");
}

static void demo_empty_and_all_bad(void)
{
    struct demo   d;
    struct expect e;
    char          fpath[128], lpath[128];
    path_of(fpath, sizeof fpath, "empty.txt");
    path_of(lpath, sizeof lpath, "missions.log");
    put_file(fpath, "", 0);
    expect_log("", 0, &e);
    run_demo(&d, fpath, lpath);
    CHECK(d.status == 0, "demo_lab1 on an empty feed exited with status %d, expected 0", d.status);
    CHECK(d.logtext != NULL && d.nlines == 3, "demo_lab1 on an empty feed wrote %zu log lines, expected 3", d.nlines);
    check_last_line(&d, &e);

    static const char bad[] =
        "1755534061.482913 NWS ACCF-4587 68.7100\n"
        "\n"
        "1755534061.482913 NWS ACCF-4587 68.7100 west 420.0 P\n";
    path_of(fpath, sizeof fpath, "bad.txt");
    put_file(fpath, bad, sizeof bad - 1);
    expect_log(bad, sizeof bad - 1, &e);
    run_demo(&d, fpath, lpath);
    CHECK(d.status == 1, "demo_lab1 rejected every line of a feed and exited with status %d, expected 1", d.status);
    CHECK(d.logtext != NULL, "demo_lab1 wrote no mission log");
    compare_body(&d, &e);
    check_last_line(&d, &e);
}

static void demo_contactfeed(void)
{
    need_demo();
    CHECK(access(g_feedtool, X_OK) == 0, "%s does not exist. Run make first: it installs the given programs", g_feedtool);

    static const char *const sensors[] = { "NWS", "AUR", "AIS", "SAT" };
    char   fpath[128], part[128], err[128], lpath[128];
    path_of(fpath, sizeof fpath, "dirty.txt");
    path_of(part, sizeof part, "dirty-part.txt");
    path_of(err, sizeof err, "contactfeed.err");
    path_of(lpath, sizeof lpath, "missions.log");
    unlink(fpath);

    size_t cap = 0, len = 0;
    char  *feed = NULL;
    for (int s = 0; s < 4; s++) {
        char *argv[] = { g_feedtool, "--sensor", (char *)sensors[s], "--burst", "40", "--profile", "dirty",
                         "--damage-every", "5", "--seed", "7", NULL };
        int st = run_program(argv, part, err, NULL);
        CHECK(st == 0, "%s --sensor %s exited with status %d", g_feedtool, sensors[s], st);
        size_t n    = 0;
        char  *text = get_file(part, &n);
        CHECK(text != NULL && n > 0, "%s wrote no feed", g_feedtool);
        if (len + n + 1 > cap) {
            cap  = (len + n + 1) * 2;
            feed = realloc(feed, cap);
            CHECK(feed != NULL, "the test is out of memory");
        }
        memcpy(feed + len, text, n);
        len += n;
        free(text);
    }
    put_file(fpath, feed, len);

    struct demo   d;
    struct expect e;
    expect_log(feed, len, &e);
    CHECK(e.bad > 0 && e.good > 0, "the dirty feed from %s has no rejected line", g_feedtool);
    run_demo(&d, fpath, lpath);
    CHECK(d.status == 0, "demo_lab1 on a dirty feed from contactfeed exited with status %d, expected 0", d.status);
    CHECK(d.logtext != NULL, "demo_lab1 wrote no mission log");
    compare_body(&d, &e);
    check_last_line(&d, &e);
}

/* demo_lab1 --follow: lines appended while it runs are logged, and it stops
   when the feed has been quiet for the idle time. */
static void demo_follow(void)
{
    need_demo();
    char fpath[128], lpath[128], out[128], err[128];
    path_of(fpath, sizeof fpath, "stream.txt");
    path_of(lpath, sizeof lpath, "missions.log");
    path_of(out, sizeof out, "demo.out");
    path_of(err, sizeof err, "demo.err");
    put_file(fpath, "", 0);
    unlink(lpath);

    static const char *const parts[] = {
        "1755534061.482913 NWS ACCF-4587 68.7100 -105.7000 420.0 P\n",
        "1755534061.501244 AIS VESL-0912 48.3",                 /* half a line */
        "000 -123.4000 12.0 R\nnot a report\n",
        "1755534062.100000 SAT SHIP-0042 51.2500 -130.5000 15.5 R\n",
    };
    enum { IDLE_MS = 1500, STEP_US = 250000 };

    struct demo d;
    memset(&d, 0, sizeof d);
    snprintf(d.feed, sizeof d.feed, "%s", fpath);
    snprintf(d.log, sizeof d.log, "%s", lpath);

    /* The demo runs in a child process, as in run_program. But this process
       does not wait for it: it appends to the feed while the demo reads.
       waitpid() with WNOHANG asks if the demo has ended, and does not wait. */
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0, "the test cannot fork: %s", strerror(errno));
    if (pid == 0) {
        int o = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int e = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (o < 0 || e < 0 || dup2(o, STDOUT_FILENO) < 0 || dup2(e, STDERR_FILENO) < 0) {
            _exit(126);
        }
        char *argv[] = { (char *)g_demo, d.feed, d.log, "--follow", "1500", NULL };
        execv(argv[0], argv);
        _exit(127);
    }

    size_t cap = 512, len = 0;
    char  *all = malloc(cap);
    CHECK(all != NULL, "the test is out of memory");
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        pause_us(STEP_US);
        int status = 0;
        CHECK(waitpid(pid, &status, WNOHANG) == 0,
              "demo_lab1 --follow %d stopped after %zu of 4 appends, with the feed quiet for less than %d ms. "
              "It must wait for more lines, not stop at the first read() of 0", IDLE_MS, i, IDLE_MS);
        size_t n = strlen(parts[i]);
        append_file(fpath, parts[i], n);
        memcpy(all + len, parts[i], n);
        len += n;
    }
    all[len] = '\0';

    int status = 0, waited_ms = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            break;
        }
        if (waited_ms >= IDLE_MS + 3000) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            fail("demo_lab1 --follow %d did not stop %d ms after the last line. It must stop when no new line "
                 "has arrived for the idle time", IDLE_MS, IDLE_MS + 3000);
        }
        pause_us(1000);
        waited_ms++;
    }
    CHECK(WIFEXITED(status), "demo_lab1 --follow crashed: signal %d", WTERMSIG(status));
    d.status = WEXITSTATUS(status);
    d.pid    = pid;
    CHECK(d.status == 0, "demo_lab1 --follow on a feed with accepted lines exited with status %d, expected 0", d.status);

    size_t loglen = 0;
    d.logtext = get_file(lpath, &loglen);
    CHECK(d.logtext != NULL, "demo_lab1 --follow wrote no mission log");
    CHECK(loglen == 0 || d.logtext[loglen - 1] == '\n', "the last line of the mission log has no '\\n'");
    d.lines = split_lines(d.logtext, loglen, &d.nlines);

    struct expect e;
    expect_log(all, len, &e);
    CHECK(e.read == 4 && e.good == 3 && e.bad == 1, "the test feed is wrong: the given contact_parse changed");
    free(all);
    compare_body(&d, &e);
    check_last_line(&d, &e);
}

/* ---- the runner -------------------------------------------------------- */

struct check {
    const char *group;
    const char *name;
    void      (*fn)(void);
};

static const struct check kChecks[] = {
    { "io.c", "sc_write_all and sc_read_full copy 1 MB through a file", io_file_roundtrip },
    { "io.c", "sc_write_all writes all of 1 MB when write() moves part of it, or is interrupted", io_partial_write },
    { "io.c", "sc_read_full reads all of 1 MB when read() returns part of it, or is interrupted", io_partial_read },
    { "io.c", "sc_open_write appends with append=1, and empties with append=0", io_append_truncate },
    { "io.c", "sc_open_write makes 0644, and sc_open_write_mode makes the mode it gets", io_modes },
    { "io.c", "a failed call gives -1 and errno, and the next call works", io_errors },
    { "io.c", "sc_close closes, and sc_close(-1) gives EBADF", io_close },

    { "clock.c", "sc_now_ns never goes back, and measures a 50 ms sleep", clock_monotonic },
    { "clock.c", "sc_now_wall gives the date, and accepts NULL", clock_wall },
    { "clock.c", "sc_stamp writes <seconds>.<6 digits>, and refuses a small buffer", clock_stamp },


    { "log.c", "each log line is <timestamp> <LEVEL in 8 columns> <message>", log_format },
    { "log.c", "log_level_name names the six levels, and NULL for a bad one; log_writef of a bad level gives EINVAL",
      log_levels },
    { "log.c", "a second log_open keeps the lines of the first", log_appends },
    { "log.c", "a new log is 0600, and an old 0644 log becomes 0600", log_private },
    { "log.c", "4 processes write 500 lines each: 2000 whole lines", log_concurrent },
    { "log.c", "a 4095-byte message gives one line of 512 bytes or less", log_long_message },
    { "log.c", "NULL handles and a missing directory give errors, and the next call works", log_errors },

    { "proc.c", "proc_report(0) gives the pid, ppid, name and threads of this process", proc_self },
    { "proc.c", "open_fds rises by 2 after two opens, and falls after two closes", proc_fds },
    { "proc.c", "proc_report of a child process gives the values of that child", proc_other },
    { "proc.c", "proc_report errors give -1, and the next report works", proc_errors },
    { "proc.c", "proc_report reads the two context switch counters", proc_ctxt },
    { "proc.c", "proc_report_write writes the layout of syscall.h", proc_write },


    { "demo_lab1", "no argument: exit status 2", demo_usage },
    { "demo_lab1", "a feed or a log that cannot open: exit status 1", demo_cannot_open },
    { "demo_lab1", "a test feed: exit status 0, and a mission log with mode 0600", demo_status_and_mode },
    { "demo_lab1", "a test feed: the start line and the process line", demo_first_lines },
    { "demo_lab1", "a test feed: one log line for each feed line, in order", demo_body },
    { "demo_lab1", "a test feed: the last log line has the counts", demo_last_line },
    { "demo_lab1", "a test feed: every log line has the format, and 512 bytes or less", demo_format },
    { "demo_lab1", "a test feed: the output says (balanced)", demo_balanced },
    { "demo_lab1", "an empty feed gives status 0; a feed of bad lines gives status 1", demo_empty_and_all_bad },
    { "demo_lab1", "a dirty feed from contactfeed: the log agrees line by line", demo_contactfeed },
    { "demo_lab1", "--follow: lines appended while it runs are logged in order, and it stops when the feed is quiet",
      demo_follow },
};

#define NCHECKS (sizeof kChecks / sizeof kChecks[0])

/* Print each line of a message, indented under its FAIL line. */
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

    /* The check runs in a child process. alarm() ends it after TIMEOUT_S
       seconds, and waitpid() collects its status. Then kill() with a negative
       pid stops each process that the check started. */
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(msgfd);
        printf("  FAIL  %s\n        the test cannot fork\n", c->name);
        return 0;
    }
    if (pid == 0) {
        /* Its own process group, so every process that the check starts is
           stopped with it. */
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

    char *msg = get_file(msg_path, NULL);
    char *out = get_file(out_path, NULL);
    int   ok  = WIFEXITED(status) && WEXITSTATUS(status) == 0;

    if (ok) {
        printf("  PASS  %s\n", c->name);
    } else {
        printf("  FAIL  %s\n", c->name);
        if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM) {
            printf("        no result after %d s: a call did not return\n", TIMEOUT_S);
        } else if (WIFSIGNALED(status)) {
            printf("        your code crashed: signal %d (%s)\n", WTERMSIG(status), strsignal(WTERMSIG(status)));
        } else if (msg != NULL && msg[0] != '\0') {
            print_indented(msg, 40);
        } else {
            printf("        the check stopped with exit status %d: your code called exit()\n", WEXITSTATUS(status));
        }
        if (out != NULL && out[0] != '\0') {
            printf("        your code printed:\n");
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

/* One row of the summary for each source file the checks are grouped by.
   Raise it when a group is added; the runner stops with a message rather than
   drop a row in silence. */
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

    snprintf(g_dir, sizeof g_dir, "/tmp/sentinel_accept_XXXXXX");
    if (mkdtemp(g_dir) == NULL) {
        fprintf(stderr, "cannot create a directory in /tmp: %s\n", strerror(errno));
        return 2;
    }

    printf("SENTINEL Lab 1 acceptance\n");

    /* One count for each group, so the summary names the file that is broken.
       A total says that work remains; a share for each file says where. The groups are contiguous in kChecks, so a running pair of
       counters is enough and no table of group names is needed. */
    const char *group      = "";
    size_t      passed     = 0;
    size_t      grp_pass   = 0, grp_total = 0;
    char        grp_name[GROUPS_MAX][32];
    size_t      grp_p[GROUPS_MAX], grp_t[GROUPS_MAX];
    size_t      ngroups    = 0;

    for (size_t i = 0; i < NCHECKS; i++) {
        if (strcmp(group, kChecks[i].group) != 0) {
            if (ngroups > 0) {
                grp_p[ngroups - 1] = grp_pass;
                grp_t[ngroups - 1] = grp_total;
            }
            group = kChecks[i].group;
            printf("\n%s\n", group);
            if (ngroups == GROUPS_MAX) {
                fprintf(stderr, "accept_lab1: more than %d check groups. "
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
        printf("  %-12s %2zu of %-2zu  %3d%%\n", grp_name[g], grp_p[g], grp_t[g],
               percent(grp_p[g], grp_t[g]));
    }

    printf("\n%zu of %zu checks pass (%d%%).\n", passed, NCHECKS, percent(passed, NCHECKS));
    printf("These checks are a part of the contract. The course tests check more of it:\n"
           "Section 6.1 of docs/lab1/lab1_brief.pdf.\n");
    return passed == NCHECKS ? 0 : 1;
}
