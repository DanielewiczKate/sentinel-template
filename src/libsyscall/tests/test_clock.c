#include "sentinel/syscall.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    int64_t a = sc_now_ns();
    int64_t b = sc_now_ns();
    assert(b >= a);

    int64_t sec;
    int32_t usec;
    sc_now_wall(&sec, &usec);
    assert(sec > 0);
    assert(usec >= 0 && usec < 1000000);
    sc_now_wall(NULL, NULL); // must not crash

    char buf[SC_STAMP_MAX];
    int n = sc_stamp(buf, sizeof buf);
    assert(n > 0);
    assert((int)strlen(buf) == n);
    assert(strchr(buf, '.') != NULL);

    assert(sc_stamp(NULL, sizeof buf) == -1);
    assert(errno == EINVAL);

    char small[2];
    assert(sc_stamp(small, sizeof small) == -1);
    assert(errno == ERANGE);

    // show that a following call still works
    assert(sc_stamp(buf, sizeof buf) > 0);

    return 0;
}
