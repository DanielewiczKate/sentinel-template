/* common/contact.h — the record format that all four parts share.
 *
 * A contact report is one line of text, at most CONTACT_LINE_MAX bytes
 * including the newline:
 *
 *   <time> <sensor> <contact> <lat> <lon> <speed> <flag>
 *   1755534061.482913 NWS ACCF-4587 68.7100 -105.7000 420.0 P
 *
 * The 128-byte limit is a design decision. Linux writes up to PIPE_BUF (4096)
 * bytes to a pipe or a FIFO in one atomic step. So many programs can write to
 * the centre's priority FIFO, and their lines do not mix, when each line is
 * one write(). Lab 2 measures this. A limit above PIPE_BUF would break it.
 */

#ifndef SENTINEL_CONTACT_H
#define SENTINEL_CONTACT_H

#include <stdint.h>
#include <stddef.h>

/* Including the '\n'. A buffer for a line needs this plus 1 for the '\0'. */
#define CONTACT_LINE_MAX 128

#define CONTACT_SENSOR_LEN 3    /* exactly 3 letters: NWS AUR AIS SAT */
#define CONTACT_ID_MAX     15   /* up to 15 characters */

/* The four sensors of Table 1, and their nominal rates. */
typedef enum {
    SENSOR_NWS = 0,   /* North Warning System radar,   ~20/s   */
    SENSOR_AUR,       /* CP-140 Aurora patrol aircraft, ~5/s   */
    SENSOR_AIS,       /* Coastal radar and AIS receiver,~50/s  */
    SENSOR_SAT,       /* RADARSAT satellite,        ~2000/s    */
    SENSOR_COUNT
} sensor_id_t;

typedef enum {
    PRIORITY_ROUTINE = 0,   /* 'R' */
    PRIORITY_PRIORITY = 1   /* 'P' */
} priority_t;

typedef struct {
    int64_t  t_sec;                            /* seconds since the epoch    */
    int32_t  t_usec;                           /* microseconds, 0..999999    */
    char     sensor[CONTACT_SENSOR_LEN + 1];   /* ends with '\0'             */
    char     id[CONTACT_ID_MAX + 1];           /* ends with '\0'             */
    double   lat;                              /* degrees, '-' is south      */
    double   lon;                              /* degrees, '-' is west       */
    double   speed;                            /* knots                      */
    priority_t flag;
} contact_t;

/* The reason that the parser refuses a line. contact_parse returns one of
   these values with no change, so a caller can report which rule the input
   broke. contact_strerror gives the name of the rule. */
typedef enum {
    CONTACT_OK = 0,
    CONTACT_ERR_TOO_LONG,      /* the line is empty, or longer than CONTACT_LINE_MAX */
    CONTACT_ERR_FIELD_COUNT,   /* not exactly 7 fields                   */
    CONTACT_ERR_TIME,          /* not a correct <sec>.<usec>             */
    CONTACT_ERR_SENSOR,        /* not one of NWS AUR AIS SAT             */
    CONTACT_ERR_ID,            /* empty, or longer than CONTACT_ID_MAX   */
    CONTACT_ERR_LAT,           /* not a number, or outside -90..90       */
    CONTACT_ERR_LON,           /* not a number, or outside -180..180     */
    CONTACT_ERR_SPEED,         /* not a number, or negative              */
    CONTACT_ERR_FLAG           /* not 'P' or 'R'                         */
} contact_err_t;

/* A name for a contact_err_t, for a person to read. It never returns NULL. */
const char *contact_strerror(contact_err_t e);

/* The name of a sensor ("NWS"), or NULL if id is not a sensor_id_t value. */
const char *sensor_name(sensor_id_t id);

/* The nominal number of reports each second for a sensor: NWS 20, AUR 5,
   AIS 50, SAT 2000. NWS, AUR and AIS send at this rate for the whole run.
   SAT sends at this rate in a pass of 10 s, and then nothing for 10 s. */
int sensor_rate(sensor_id_t id);

/* Format c into buf as one line, including the '\n' at the end.
 * Returns the number of bytes written. Returns -1 if the line does not fit
 * in buflen or is longer than CONTACT_LINE_MAX.
 */
int contact_format(const contact_t *c, char *buf, size_t buflen);

/* Parse one line into out. The line can end with '\n' or not. len is the
 * length of the line, not counting a '\0'.
 *
 * Returns CONTACT_OK (0) when the line is a correct report, or one of the
 * contact_err_t values. contact_parse changes out only when it returns
 * CONTACT_OK.
 */
contact_err_t contact_parse(const char *line, size_t len, contact_t *out);

#endif /* SENTINEL_CONTACT_H */
