#include "sentinel/syscall.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    proc_info_t info;
    assert(proc_report(0, &info) == 0);
    assert(info.pid == getpid());
    assert(info.ppid == getppid());
    assert(info.threads >= 1);

    char tmp[] = "/tmp/sentinel_test_proc_XXXXXX";
    int fd = mkstemp(tmp);
    assert(fd >= 0);
    assert(proc_report_write(fd, &info) == 0);
    close(fd);
    unlink(tmp);

    assert(proc_report(0, NULL) == -1);
    assert(errno == EINVAL);

    // show that a following call still works
    assert(proc_report(0, &info) == 0);

    return 0;
}
