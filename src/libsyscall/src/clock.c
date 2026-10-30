#include "sentinel/syscall.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

int64_t sc_now_ns(void)
{
    struct timespec ts;

    // CLOCK_MONOTONIC with a valid stack address cannot fail
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void sc_now_wall(int64_t *sec, int32_t *usec)
{
    struct timespec ts;

    // CLOCK_REALTIME with a valid stack address cannot fail
    clock_gettime(CLOCK_REALTIME, &ts);

    if (NULL != sec)
        *sec = (int64_t)ts.tv_sec;
    if (NULL != usec)
        *usec = (int32_t)(ts.tv_nsec / 1000);
}

int sc_stamp(char *buf, size_t buflen)
{
    if (NULL == buf) {
        // EINVAL since buf must be passed
        errno = EINVAL;
        return -1;
    }

    int64_t sec;
    int32_t usec;
    sc_now_wall(&sec, &usec);

    char tmp[SC_STAMP_MAX];
    int n = snprintf(tmp, sizeof tmp, "%lld.%06d", (long long)sec, usec);

    if ((size_t)n + 1 > buflen) {
        // This should not occur, 32 bytes is a large number
        errno = ERANGE;
        return -1;
    }

    // Copy at the end, so a snprintf does not corrupt buf
    memcpy(buf, tmp, (size_t)n + 1);
    return n;
}
