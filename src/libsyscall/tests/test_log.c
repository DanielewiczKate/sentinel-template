#include "sentinel/syscall.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    char path[] = "/tmp/sentinel_test_log_XXXXXX";
    int tfd = mkstemp(path);
    assert(tfd >= 0);
    close(tfd);

    log_t *lg = log_open(path);
    assert(lg != NULL);

    struct stat st;
    assert(stat(path, &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    assert(log_writef(lg, LOG_INFO, "hello %d", 42) == 0);
    assert(log_close(lg) == 0);

    int rfd = sc_open_read(path);
    assert(rfd >= 0);
    char buf[256] = { 0 };
    sc_read_full(rfd, buf, sizeof buf - 1);
    sc_close(rfd);
    assert(strstr(buf, "INFO") != NULL);
    assert(strstr(buf, "hello 42") != NULL);

    assert(log_level_name((log_level_t)99) == NULL);
    assert(log_level_name(LOG_SYSTEM) != NULL);

    assert(log_open(NULL) == NULL);
    assert(errno == EINVAL);

    // show that a following call still works
    log_t *lg2 = log_open(path);
    assert(lg2 != NULL);
    assert(log_close(lg2) == 0);

    unlink(path);
    return 0;
}
