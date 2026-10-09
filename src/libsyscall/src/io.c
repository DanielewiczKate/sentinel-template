#include <sentinel/syscall.h>


int sc_open_read(const char *path)
{
    (void)path;
    /* TODO */
    return -1;
}
 
int sc_open_write(const char *path, int append)
{
    (void)path;
    (void)append;
    /* TODO */
    return -1;
}
 
int sc_open_write_mode(const char *path, int append, mode_t mode)
{
    (void)path;
    (void)append;
    (void)mode;
    /* TODO */
    return -1;
}
 
int sc_close(int fd)
{
    (void)fd;
    /* TODO */
    return -1;
}
 
int sc_write_all(int fd, const void *buf, size_t n)
{
    (void)fd;
    (void)buf;
    (void)n;
    /* TODO */
    return -1;
}
 
ssize_t sc_read_full(int fd, void *buf, size_t n)
{
    (void)fd;
    (void)buf;
    (void)n;
    /* TODO */
    return -1;
}
