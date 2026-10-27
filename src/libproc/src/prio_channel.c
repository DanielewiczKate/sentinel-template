/* prio_channel.c — the priority channel, a named FIFO.
 *
 * THE COURSE WROTE A PART OF THIS FILE: open_failed(), under "GIVEN". Do not
 * change it. YOU write the three functions under "YOURS". The comment above
 * each one gives its steps, and proc.h gives the same function with each
 * error. Each one is a stub now: it fails, so that the library builds
 * before you write it. Replace the whole body of each stub, its (void) lines
 * too. Lecture 4.4 (Programming IPC) shows mkfifo. MAN PAGES, at the end
 * of this comment, names the manual page of each call.
 *
 * The sensor pipes carry the reports of the four processes that the centre
 * started. The priority channel is different: it has a name in the file
 * system, so any program on the machine can send the centre an urgent
 * contact. The centre does not start that program.
 *
 * Two facts change the code. If you forget one of them, the centre stops or
 * uses a full processor, and it prints no error:
 *
 * 1. open() of a FIFO waits until the other side opens it. A centre that
 *    opens its read end with O_RDONLY alone never reaches its loop. So the
 *    centre opens it with O_NONBLOCK.
 *
 * 2. When no process holds a write end, a read of the FIFO gives END OF
 *    FILE, not "empty". poll() then marks the channel each time, and the
 *    centre turns in its loop with no wait. So the centre holds a write end
 *    itself (fd_hold), and an empty FIFO is only empty.
 *
 * MAN PAGES. Each call of the C library and of Linux in this file, the
 * command that shows its manual page in a terminal, and the same page on
 * the web. The functions of the course have no man page: read them in
 * proc.h, syscall.h and common/cpu.h.
 *
 *   memset()                 man 3 memset
 *       https://man7.org/linux/man-pages/man3/memset.3.html
 *   strlen()                 man 3 strlen
 *       https://man7.org/linux/man-pages/man3/strlen.3.html
 *   memcpy()                 man 3 memcpy
 *       https://man7.org/linux/man-pages/man3/memcpy.3.html
 *   unlink()                 man 2 unlink
 *       https://man7.org/linux/man-pages/man2/unlink.2.html
 *   mkfifo()                 man 3 mkfifo
 *       https://man7.org/linux/man-pages/man3/mkfifo.3.html
 *   chmod()                  man 2 chmod
 *       https://man7.org/linux/man-pages/man2/chmod.2.html
 *   open(), O_NONBLOCK,      man 2 open
 *   O_CLOEXEC
 *       https://man7.org/linux/man-pages/man2/open.2.html
 *   poll()                   man 2 poll
 *       https://man7.org/linux/man-pages/man2/poll.2.html
 *   a FIFO                   man 7 fifo
 *       https://man7.org/linux/man-pages/man7/fifo.7.html
 *   a pipe, PIPE_BUF         man 7 pipe
 *       https://man7.org/linux/man-pages/man7/pipe.7.html
 *   errno, error names       man 3 errno
 *       https://man7.org/linux/man-pages/man3/errno.3.html
 */

#include "sentinel/proc.h"

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* ------------------------------------------------ GIVEN: do not change */

/* Undo a prio_open that failed after mkfifo: close what is open, remove the
   FIFO, and keep the errno of the call that failed. After this, ch is
   closed, so a later prio_close does nothing. */
static void open_failed(prio_channel_t *ch)
{
    int saved = errno;
    if (ch->fd_hold >= 0) {
        sc_close(ch->fd_hold);
        ch->fd_hold = -1;
    }
    if (ch->fd >= 0) {
        sc_close(ch->fd);
        ch->fd = -1;
    }
    unlink(ch->path);
    ch->path[0] = '\0';
    errno = saved;
}

/* ------------------------------------------------ YOURS */

/* prio_open: create the FIFO, and open it for the centre.
 *
 *   1. If ch or path is NULL, set errno to EINVAL and return -1.
 *   2. memset(ch, 0, sizeof *ch), then ch->fd = -1 and ch->fd_hold = -1.
 *   3. If strlen(path) >= sizeof ch->path, set errno to ENAMETOOLONG and
 *      return -1. A path that is cut gives the FIFO another name than the
 *      one that the senders open.
 *   4. Copy path into ch->path, with its '\0'.
 *   5. unlink(path). An entry from an earlier run can have the wrong mode.
 *      No check: usually there is no entry.
 *   6. mkfifo(path, 0600). If it fails: ch->path[0] = '\0', and return -1.
 *   7. chmod(path, 0600): the umask can change the mode of mkfifo, and
 *      chmod makes it exact. If it fails: open_failed(ch), return -1.
 *   8. ch->fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC). If it gives
 *      -1: open_failed(ch), return -1.
 *   9. ch->fd_hold = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC). It does
 *      not wait: the read end is open. If it gives -1: open_failed(ch),
 *      return -1.
 *  10. Return 0.
 */
int prio_open(prio_channel_t *ch, const char *path)
{
    (void)ch;
    (void)path;
    (void)open_failed;      /* steps 7 to 9 call it */
    errno = ENOSYS;
    return -1;
}

/* prio_close: close the FIFO and remove it from the file system.
 *
 *   1. If ch is NULL, return.
 *   2. If ch->fd_hold >= 0: sc_close(ch->fd_hold), and ch->fd_hold = -1.
 *   3. If ch->fd >= 0: sc_close(ch->fd), and ch->fd = -1.
 *   4. If ch->path[0] is not '\0': unlink(ch->path), and ch->path[0] = '\0'.
 *      A FIFO stays in the file system after each process closes it.
 *   A second call then does nothing.
 */
void prio_close(prio_channel_t *ch)
{
    (void)ch;
}

/* prio_send: send one report into a FIFO that exists.
 *
 *   1. If path or line is NULL, set errno to EINVAL and return -1.
 *   2. size_t len = strlen(line). The line needs a '\n' at its end when
 *      len is 0 or line[len - 1] is not '\n':
 *        int add_nl = (len == 0 || line[len - 1] != '\n');
 *   3. If len + add_nl > CONTACT_LINE_MAX, set errno to EMSGSIZE and
 *      return -1. Open nothing.
 *   4. Put the line and its '\n' in ONE buffer:
 *        char buf[CONTACT_LINE_MAX + 1];
 *        memcpy(buf, line, len);
 *        if (add_nl) buf[len++] = '\n';
 *   5. int fd = open(path, O_WRONLY | O_CLOEXEC). This open waits until
 *      the centre has the read end open. If it gives -1, return -1.
 *   6. int rc = sc_write_all(fd, buf, len): ONE write of the whole buffer.
 *      proc.h says why a second write() for the '\n' is wrong.
 *   7. Save errno, sc_close(fd), put errno back, and return rc.
 */
int prio_send(const char *path, const char *line)
{
    (void)path;
    (void)line;
    errno = ENOSYS;
    return -1;
}
