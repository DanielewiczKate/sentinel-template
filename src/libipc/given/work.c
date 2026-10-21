/* work.c — GIVEN. What one report costs.
 *
 * A worker does not compare a contact with a radar picture: there is no
 * radar. So it must SPEND something for each report, or the pool has no work
 * to share. What it spends decides each measure of the pool, so it must be
 * exact.
 *
 * The first version of this code waited for a CLOCK_MONOTONIC time:
 *
 *     int64_t until = sc_now_ns() + service_us * 1000;
 *     while (sc_now_ns() < until) { }
 *
 * That is a WAIT, not WORK. When the operating system takes the processor
 * from the thread, the elapsed clock continues. The thread comes back, finds
 * that the time passed, and stops at once. On eight idle processors the two
 * give the same result. On ONE processor they do not: eight workers each
 * "cost" 200 us and all eight finish in the same 200 us, so the pool seems
 * to scale and the processor seems free. Each panel of the page then shows
 * an effect that the program does not have.
 *
 * So the work is real arithmetic on the fields of the contact, and its
 * amount is calibrated one time against CLOCK_THREAD_CPUTIME_ID. Then
 * --service-us is what it states: microseconds of PROCESSOR time for each
 * report.
 *
 * Two rules keep this correct:
 *
 *   The compiler must not remove the loop. work_do returns its result, it is
 *   in its own file, and each caller stores the result. A test of this
 *   project once measured work that -O1 had removed.
 *
 *   The loop must not read the clock. On aarch64 a read of
 *   CLOCK_THREAD_CPUTIME_ID is a real system call, so a loop that reads it
 *   at each pass measures the clock more than the work. The calibration
 *   reads the clock two times, and the work loop never reads it.
 */

#include "sentinel/ipc.h"
#include "sentinel/syscall.h"
#include "cpu.h"

#include <stdlib.h>

/* Kept for the life of the process. The calibration takes about 1 s, and a
   test that makes twenty pools must not pay it twenty times. The value is a
   property of the machine, not of the pool. */
static long g_passes_per_us = 0;
static int  g_spread_pct    = -1;   /* how far the three tries disagreed */

/* One try of the calibration: increase the number of passes until the loop
   costs enough processor time to measure well, then give the passes for
   each microsecond.

   THE TARGET IS 200 ms, FROM A MEASURE. The same try, twelve times on this
   machine:

       sample length      passes per us         spread
       20 ms              744 to 813            about 9%
       200 ms             796 to 805            about 1%

   The clock is not the cause of the error of a short sample: clock_getres
   gives 1 ns, and the smallest step seen is 208 ns. The speed of the
   machine changes during the sample, and 20 ms is too short to average it.
   Each measured drain time is divided by this number, so an error of 9%
   here is an error of 9% in each of them.

   A read of the clock costs 202 ns on this machine. That is one more reason
   why the work loop never reads it. */
static long calibrate_once(void)
{
    const int64_t target_ns = 200 * 1000000;

    /* Start near the target, not far below it. The first sample is the
       least reliable, so it must not be the one that ends the loop. */
    long passes = 2000000;

    for (int attempt = 0; attempt < 24; attempt++) {
        contact_t c;
        c.lat = 68.71;
        c.lon = -105.70;
        c.speed = 420.0;

        int64_t before = cpu_thread_ns();
        volatile double acc = work_do(&c, 0, passes);
        int64_t cpu = cpu_thread_ns() - before;
        (void)acc;                     /* volatile: the store must occur */

        if (cpu >= target_ns) {
            double per_us = (double)passes / ((double)cpu / 1000.0);
            long   n = (long)(per_us + 0.5);
            return n > 0 ? n : 1;
        }
        if (cpu <= 0) {
            passes *= 4;               /* less than the clock can measure */
        } else {
            /* Go to the target with a small margin. Two times more at each
               try needs many more tries on a fast machine. */
            double grow = (double)target_ns / (double)cpu * 1.2;
            if (grow < 1.5) {
                grow = 1.5;
            }
            if (grow > 64.0) {
                grow = 64.0;
            }
            passes = (long)((double)passes * grow);
        }
    }
    return -1;
}

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a;
    long y = *(const long *)b;
    return (x > y) - (x < y);
}

long work_calibrate(void)
{
    if (g_passes_per_us > 0) {
        return g_passes_per_us;
    }

    /* Warm the processor before the measure.
     *
     * A measure found this. With no warm-up, the first burst test drained
     * 2000 reports of 500 us in 735 ms, not in the 1000 ms of the setting:
     * the work cost 27% too little. The cause is the frequency of the
     * processor. A program calibrates in its first milliseconds, when the
     * processor runs at a low frequency, so it counts too few passes for each
     * microsecond. When the workers run, the frequency is higher, each pass
     * is faster, and 500 us of passes costs less than 500 us.
     *
     * That error makes the system look better than it is, and nothing
     * reports it. So spend 40 ms first, then measure. */
    {
        contact_t warm;
        warm.lat = 49.0;
        warm.lon = -123.0;
        warm.speed = 20.0;
        int64_t until = cpu_thread_ns() + 40 * 1000000;
        while (cpu_thread_ns() < until) {
            volatile double acc = work_do(&warm, 0, 2000000);
            (void)acc;
        }
    }

    /* Three tries, and the middle one. A change of frequency, or another
       program that takes the processor, can spoil one try. The middle of
       three removes one bad try. */
    long tries[3];
    for (int i = 0; i < 3; i++) {
        tries[i] = calibrate_once();
        if (tries[i] < 0) {
            return -1;
        }
    }
    qsort(tries, 3, sizeof tries[0], cmp_long);

    /* The spread is the reason for three tries. The log reports it: do not
       measure with a calibration that does not repeat. */
    g_spread_pct = tries[1] > 0
        ? (int)(100.0 * (double)(tries[2] - tries[0]) / (double)tries[1] + 0.5)
        : -1;
    g_passes_per_us = tries[1];
    return g_passes_per_us;
}

int work_calibration_spread_pct(void)
{
    return g_spread_pct;
}

/* The work.
 *
 * It reads the contact, so the loop is not empty, and it returns its
 * result, so the compiler cannot remove it. The arithmetic is simple on
 * purpose: it is the cost of one report, and not a real correlation.
 *
 * service_us 0 runs exactly passes_per_us passes, for the calibration. In
 * each other case it runs service_us x passes_per_us passes.
 */
double work_do(const contact_t *c, int service_us, long passes_per_us)
{
    long passes;
    if (service_us > 0) {
        if (passes_per_us <= 0) {
            return 0.0;
        }
        passes = (long)service_us * passes_per_us;
    } else if (service_us == 0) {
        passes = passes_per_us;        /* calibration calls it this way */
    } else {
        return 0.0;
    }

    double acc = c->lat;
    double inc = c->lon;
    double mul = 1.0 + (c->speed * 1e-12);
    for (long i = 0; i < passes; i++) {
        acc = acc * mul + inc;
        inc = inc * 0.9999999 + 1e-9;
    }
    return acc;
}
