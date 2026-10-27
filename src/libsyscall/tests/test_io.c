#include "sentinel/syscall.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char path[] = "/tmp/sentinel_test_io_XXXXXX";
    int tfd = mkstemp(path);
    assert(tfd >= 0);
    close(tfd);

    int wfd = sc_open_write(path, 0);
    assert(wfd >= 0);
    assert(sc_write_all(wfd, "hello", 5) == 0);
    assert(sc_close(wfd) == 0);

    int rfd = sc_open_read(path);
    assert(rfd >= 0);
    char buf[5] = { 0 };
    assert(sc_read_full(rfd, buf, 5) == 5);
    assert(memcmp(buf, "hello", 5) == 0);
    assert(sc_read_full(rfd, buf, 1) == 0); // EOF
    assert(sc_close(rfd) == 0);

    assert(sc_open_read("/tmp/sentinel_test_io_missing") == -1);
    assert(errno == ENOENT);

    assert(sc_write_all(-1, "x", 1) == -1);
    assert(errno == EBADF);

    // show that a following call still works
    int rfd2 = sc_open_read(path);
    assert(rfd2 >= 0);
    assert(sc_close(rfd2) == 0);

    unlink(path);
    return 0;
}
