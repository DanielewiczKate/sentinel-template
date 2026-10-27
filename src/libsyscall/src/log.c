#include "sentinel/syscall.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

struct log {
    int fd;
};

log_t *log_open(const char *path)
{
    if (NULL == path) {
        // NULL path cannot be written to
        errno = EINVAL;
        return NULL;
    }

    // Open path in append mode, since logs are sequential
    int fd = sc_open_write(path, 1);
    if (fd < 0)
        return NULL;

    // Elivate to write permission
    if (fchmod(fd, 0600) != 0) {
        int err = errno;
        sc_close(fd);
        errno = err;
        return NULL;
    }

    // Pointer to new log. Heap allocated.
    log_t *lg = malloc(sizeof *lg);
    if (NULL == lg) {
        int err = errno;
        sc_close(fd);
        errno = err;
        return NULL;
    }

    // Place the write pointer into the log structure
    lg->fd = fd;
    return lg;
}

int log_writef(log_t *lg, log_level_t level, const char *fmt, ...)
{
    if (NULL == lg || NULL == fmt) {
        errno = EINVAL;
        return -1;
    }

    const char *lname = log_level_name(level);
    if (NULL == lname) {
        errno = EINVAL;
        return -1;
    }

    // Get timestap at time of log_writef call
    char stamp[SC_STAMP_MAX];
    sc_stamp(stamp, sizeof stamp);

    // a line is at most 512 bytes, including its '\n'
    char line[513];
    int prefix_n = snprintf(line, sizeof line, "%s %-8s ", stamp, lname);

    int msg_cap = 512 - prefix_n - 1;
    if (msg_cap < 0)
        msg_cap = 0;


    // Use vsnprintf to do formatted strings. This works by catching
    // fomatting exit sequences (%i for instance) and then inserts the
    // correct value from the ... term of the function.
    // This could technically be done with hacky intToString functions
    // but that is not the spec and is hacky
    va_list ap;
    va_start(ap, fmt);
    int msg_n = vsnprintf(line + prefix_n, (size_t)msg_cap + 1, fmt, ap);
    va_end(ap);

    if (msg_n < 0) {
        errno = EOVERFLOW;
        return -1;
    }
    if (msg_n > msg_cap)
        msg_n = msg_cap;

    // every '\n' in the message becomes a space, so one call writes one line
    for (int i = 0; i < msg_n; i++)
        if ('\n' == line[prefix_n + i])
            line[prefix_n + i] = ' ';
    line[prefix_n + msg_n] = '\n';

    // Finally, write the cleaned msg_n to the log
    return sc_write_all(lg->fd, line, (size_t)(prefix_n + msg_n + 1));
}

int log_close(log_t *lg)
{
    if (NULL == lg)
        return 0;

    int res = sc_close(lg->fd);
    int err = errno;
    free(lg);

    if (res != 0) {
        errno = err;
        return -1;
    }
    return 0;
}

const char *log_level_name(log_level_t level)
{
    static const char *const names[] = { "SYSTEM", "INFO", "PENDING", "PRIORITY", "ALERT", "TRACK" };

    if ((unsigned)level >= 6)
        return NULL;
    return names[level];
}
