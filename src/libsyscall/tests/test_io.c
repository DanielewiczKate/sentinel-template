#include <sentinel/syscall.h>
#include <stdio.h>

int main(void)
{
    int fd = sc_open_read("/etc/hostname");
    if (fd < 0) {
        printf("FAIL: sc_open_read\n");
        return 1;
    }
    sc_close(fd);

    printf("ok\n");
    return 0;
}
