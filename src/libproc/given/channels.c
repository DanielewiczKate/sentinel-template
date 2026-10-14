/* channels.c — the sensor table, and the reader of the sensor channels.
 *
 * GIVEN CODE. You read this file, and you do not change it. Your
 * sensor_spawn, sensor_stop and sensor_reap in src/sensor.c fill the table
 * that this file reads.
 *
 * A channel is the read end of the pipe of one sensor. The centre holds it.
 * Each channel has its own line reader (common/linereader.h), because a
 * read() from a pipe can stop in the middle of a line. The reader keeps the
 * part of a line that came before the rest of it arrived.
 *
 * The centre never waits in read(). It waits in poll(), and then reads each
 * channel that poll() marked. So each read end is non-blocking: a read()
 * on an empty pipe then gives EAGAIN at once, and the centre goes on to the
 * next channel.
 */

#include "sentinel/proc.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

/* The size of each read from a channel. A line longer than this is lost,
   and lr_next_line says so with EMSGSIZE. 4096 is 32 times the longest
   correct line. */
#define CHANNEL_BUF 4096

int sensor_net_init(sensor_net_t *net, const char *feed_path, log_t *log)
{
    if (net == NULL || feed_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(net, 0, sizeof *net);
    net->feed_path = feed_path;
    net->log       = log;

    for (int i = 0; i < SENSOR_COUNT; i++) {
        net->sensors[i].id  = (sensor_id_t)i;
        net->sensors[i].pid = 0;
        net->sensors[i].fd  = -1;
        net->sensors[i].lr  = NULL;
    }

    if (log != NULL) {
        log_writef(log, LOG_SYSTEM,
                   "sensor network ready, feed=%s, one pipe for each sensor",
                   feed_path);
    }
    return 0;
}

int sensor_channel_open(sensor_t *s, int fd)
{
    if (s == NULL || fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (s->fd >= 0) {
        errno = EBUSY;
        return -1;
    }

    /* Only the read end becomes non-blocking. The write end in the sensor
       stays blocking, so that a sensor waits when its pipe is full, and no
       line is cut. */
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        return -1;
    }

    linereader_t *lr = lr_open(fd, CHANNEL_BUF, 1);
    if (lr == NULL) {
        return -1;
    }

    s->fd    = fd;
    s->lr    = lr;
    s->taken = 0;
    s->more  = 0;
    return 0;
}

void sensor_channel_close(sensor_t *s)
{
    if (s == NULL) {
        return;
    }
    /* The reader owns the descriptor, so lr_close closes it. */
    if (s->lr != NULL) {
        lr_close(s->lr);
    } else if (s->fd >= 0) {
        close(s->fd);
    }
    s->lr    = NULL;
    s->fd    = -1;
    s->taken = 0;
    s->more  = 0;
}

int sensor_spawn_all(sensor_net_t *net)
{
    int started = 0;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        sensor_id_t id = (sensor_id_t)i;

        /* Each sensor process runs for the whole run. SAT stops sending
           between two passes, but its process does not stop. A sensor that
           stops before the centre stops it is a failure. */
        if (sensor_spawn(net, id, sensor_rate(id), 0) == 0) {
            started++;
        }
    }
    return started;
}

void sensor_net_shutdown(sensor_net_t *net)
{
    if (net == NULL) {
        return;
    }
    for (int i = 0; i < SENSOR_COUNT; i++) {
        sensor_stop(net, (sensor_id_t)i);
        sensor_channel_close(&net->sensors[i]);
    }
}

sensor_t *sensor_by_pid(sensor_net_t *net, pid_t pid)
{
    if (net == NULL || pid <= 0) {
        return NULL;
    }
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (net->sensors[i].pid == pid) {
            return &net->sensors[i];
        }
    }
    return NULL;
}

int sensor_running_count(const sensor_net_t *net)
{
    if (net == NULL) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (net->sensors[i].pid != 0) {
            n++;
        }
    }
    return n;
}

/* --- reading the channels ------------------------------------------------ */

void sensor_net_pollfds(const sensor_net_t *net, struct pollfd *fds)
{
    for (int i = 0; i < SENSOR_COUNT; i++) {
        fds[i].fd      = (net != NULL) ? net->sensors[i].fd : -1;
        fds[i].events  = POLLIN;
        fds[i].revents = 0;
    }
}

/* Start the next pass: each channel can give its limit again. */
static void pass_reset(sensor_net_t *net)
{
    net->cursor = 0;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        net->sensors[i].taken = 0;
    }
}

int sensor_net_next_line(sensor_net_t *net, const struct pollfd *fds,
                         sensor_id_t *from, const char **line, size_t *len)
{
    if (net == NULL || fds == NULL || from == NULL || line == NULL || len == NULL) {
        errno = EINVAL;
        return -1;
    }

    while (net->cursor < SENSOR_COUNT) {
        sensor_t *s = &net->sensors[net->cursor];

        /* Read a channel that poll() marked. Also read a channel that
           stopped at the limit in the last pass: its reader can hold whole
           lines while its pipe is empty, and poll() cannot see the reader.
           POLLHUP comes with end of file. */
        int marked = fds[net->cursor].fd == s->fd && s->fd >= 0 &&
                     (fds[net->cursor].revents & (POLLIN | POLLHUP | POLLERR));
        if (s->lr == NULL || (!marked && !s->more)) {
            net->cursor++;
            continue;
        }
        if (s->taken >= SENSOR_LINES_PER_PASS) {
            s->more = 1;        /* there can be more; take it in the next pass */
            net->cursor++;
            continue;
        }

        int rc = lr_next_line(s->lr, line, len);
        if (rc == 1) {
            s->taken++;
            *from = s->id;
            return 1;
        }
        if (rc == 0) {
            /* End of file: no process holds the write end now, and the
               centre has read each line in the pipe. The channel closes
               here, so that a new sensor_spawn can make a new pipe. */
            if (net->log != NULL) {
                log_writef(net->log, LOG_SYSTEM, "sensor %s channel closed",
                           sensor_name(s->id));
            }
            sensor_channel_close(s);
            net->cursor++;
            continue;
        }
        if (errno == EMSGSIZE) {
            /* One line was too long. The reader removed it, and it is still
               good. The caller counts the lost line. */
            s->taken++;
            *from = s->id;
            errno = EMSGSIZE;
            return -1;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            s->more = 0;        /* empty for now */
            net->cursor++;
            continue;
        }
        if (net->log != NULL) {
            log_writef(net->log, LOG_ALERT, "sensor %s channel read error: %s",
                       sensor_name(s->id), strerror(errno));
        }
        sensor_channel_close(s);
        net->cursor++;
    }

    pass_reset(net);
    return 0;
}

int sensor_net_more(const sensor_net_t *net)
{
    if (net == NULL) {
        return 0;
    }
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (net->sensors[i].lr != NULL && net->sensors[i].more) {
            return 1;
        }
    }
    return 0;
}

int sensor_net_open_count(const sensor_net_t *net)
{
    if (net == NULL) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (net->sensors[i].fd >= 0) {
            n++;
        }
    }
    return n;
}
