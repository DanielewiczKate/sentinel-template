/* test_lab2.c — your test of src/sensor.c and src/prio_channel.c.
 *
 * The course wrote the includes, the paths, read_lines(), the first test
 * and main(). Write the body of each other test function: the comment above
 * it gives what to check. Each check is an assert(). make test builds and
 * runs this file.
 */

#include "sentinel/proc.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* The Makefile gives SENTINEL_BUILD_DIR, an absolute path: make runs the
   test in src/libproc/, and a failed execvp is silent in the parent. The
   second definition is only for an editor that reads this file with no
   build flags. */
#ifndef SENTINEL_BUILD_DIR
#define SENTINEL_BUILD_DIR "./build"
#endif

#define FEED SENTINEL_BUILD_DIR "/contactfeed"
#define LOGP "/tmp/sentinel_test_lab2.log"
#define FIFO "/tmp/sentinel_test_lab2.fifo"

/* Read the channels of net until each one is closed, or until timeout_ms
   have passed. Returns the number of lines that came from sensor `id` and
   name it. This is the loop of the given centre_run, with no scheduler. */
static long read_lines(sensor_net_t *net, sensor_id_t id, int timeout_ms)
{
    int64_t end  = sc_now_ns() + (int64_t)timeout_ms * 1000000;
    long    good = 0;

    while (sensor_net_open_count(net) > 0 && sc_now_ns() < end) {
        struct pollfd fds[SENSOR_COUNT];
        sensor_net_pollfds(net, fds);
        poll(fds, SENSOR_COUNT, sensor_net_more(net) ? 0 : 100);

        sensor_id_t from;
        const char *line;
        size_t      len;
        int         rc;
        while ((rc = sensor_net_next_line(net, fds, &from, &line, &len)) != 0) {
            contact_t c;
            if (rc == 1 && from == id &&
                contact_parse(line, len, &c) == CONTACT_OK &&
                strcmp(c.sensor, sensor_name(id)) == 0) {
                good++;
            }
        }
    }
    return good;
}

/* EXAMPLE, written by the course. A sensor with burst 5 sends 5 lines on
   its own channel and exits. Then the channel reaches end of file, and
   read_lines returns. */
static void test_burst(void)
{
    log_t *lg = log_open(LOGP);
    assert(lg != NULL);
    sensor_net_t net;
    assert(sensor_net_init(&net, FEED, lg) == 0);

    assert(sensor_spawn(&net, SENSOR_NWS, 5, 1) == 0);
    assert(net.sensors[SENSOR_NWS].pid > 0);
    assert(net.n_sensors == 1);
    assert(read_lines(&net, SENSOR_NWS, 5000) == 5);
    assert(sensor_net_open_count(&net) == 0);   /* the channel ended */

    sensor_net_shutdown(&net);
    log_close(lg);
}

/* A sensor that runs. Make a network as test_burst does. Start NWS with
   sensor_spawn(&net, SENSOR_NWS, 20, 0): burst 0, so it runs until it is
   stopped. A second sensor_spawn of NWS gives -1 with errno EEXIST, and
   net.n_sensors is still 1. Then sensor_net_shutdown(&net) (it calls your
   sensor_stop) and log_close(lg). */
static void test_twice(void)
{
}

/* A sensor that finished. Make a network, and start AUR with burst 3. Call
   read_lines(&net, SENSOR_AUR, 5000): it gives 3. The process can still
   exist for a short time after its channel ends, so call sensor_reap(&net)
   until it gives 1: at most 200 times, with usleep(10000) between two
   calls. Then the last result is 1, net.sensors[SENSOR_AUR].pid is 0, and
   net.n_sensors is 0. Then sensor_net_shutdown and log_close. */
static void test_reap(void)
{
}

/* The FIFO. prio_channel_t ch; prio_open(&ch, FIFO) gives 0. Then
   stat(FIFO, &st) gives 0, S_ISFIFO(st.st_mode) is true, and
   (st.st_mode & 0777) is 0600. After prio_close(&ch), stat(FIFO, &st)
   gives -1 with errno ENOENT. */
static void test_fifo(void)
{
}

int main(void)
{
    test_burst();
    test_twice();
    test_reap();
    test_fifo();
    printf("test_lab2: OK\n");
    return 0;
}
