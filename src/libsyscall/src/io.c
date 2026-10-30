#include <sentinel/syscall.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

int sc_open_read(const char *path)
{
    while(1) {
        int res = open(path, O_RDONLY);
        int err = errno;

        if(-1 != res) 
            return res;

        // Retry if error is EINTR
        if (EINTR == err)
            continue;

        return -1;
    }
}
 
int sc_open_write(const char *path, int append)
{
    while(1) {
        int res = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
        int err = errno;

        if(-1 != res) 
            return res;

        // Retry if error is EINTR
        if (EINTR == err)
            continue;

        return -1;
    }
}
 
int sc_open_write_mode(const char *path, int append, mode_t mode)
{
    while(1) {
        int res = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), mode);
        int err = errno;

        if(-1 != res) 
            return res;

        // Retry if error is EINTR
        if (EINTR == err)
            continue;

        return -1;
    }
}
 
int sc_close(int fd)
{
    int res = close(fd);
    int err = errno;

    // EINTR: Linux has released fd already, not a failure
    if (-1 == res && EINTR != err)
        return -1;

    return 0;
}

int sc_write_all(int fd, const void *buf, size_t n)
{
    ssize_t offset = 0;
    while(1) {
        if(offset == (ssize_t)n) 
            return 0;

        ssize_t res = write(fd, (const char *)buf + offset, n - offset);
        int err = errno;
        
        // Only increase offset when res is valid (ie not an error)
        if (res > 0) {
            offset += res;
            continue;
        }

        // No more bytes written, error.
        if (res == 0) {
            errno = EIO;
            return -1;
        }
        // Retry if error is EINTR
        if (EINTR == err)
            continue;

        return -1;
    }
}
 
ssize_t sc_read_full(int fd, void *buf, size_t n)
{
    ssize_t offset = 0;
    while(1) {
        if(offset == (ssize_t)n) 
            return n;

        ssize_t res = read(fd, (char *)buf + offset, n - offset);
        int err = errno;
        
        // Only increase offset when res is valid (ie not an error)
        if (res > 0) {
            offset += res;
            continue;
        }

        // No more bytes read, done.
        if (res == 0)
            return (ssize_t)offset;

        // Retry if error is EINTR
        if (EINTR == err)
            continue;

        return -1;
    }
}
