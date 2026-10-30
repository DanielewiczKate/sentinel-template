#include "sentinel/sync.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define CELL_LAT_MIN  40.0
#define CELL_LAT_MAX  85.0
#define CELL_LON_MIN -145.0
#define CELL_LON_MAX -50.0

static double cell_lat_step(void) { return (CELL_LAT_MAX - CELL_LAT_MIN) / CELL_ROWS; }
static double cell_lon_step(void) { return (CELL_LON_MAX - CELL_LON_MIN) / CELL_COLS; }

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void cell_of(double lat, double lon, int *row, int *col)
{
    *row = clampi((int)((lat - CELL_LAT_MIN) / cell_lat_step()), 0, CELL_ROWS - 1);
    *col = clampi((int)((lon - CELL_LON_MIN) / cell_lon_step()), 0, CELL_COLS - 1);
}

static int cells_in_reach(double lat, double lon, double reach_km,
                          int out[4])
{
    int row, col;
    cell_of(lat, lon, &row, &col);

    int n = 0;
    out[n++] = row * CELL_COLS + col;

    double lat_lo = CELL_LAT_MIN + row * cell_lat_step();
    double lat_hi = lat_lo + cell_lat_step();
    double lon_lo = CELL_LON_MIN + col * cell_lon_step();
    double lon_hi = lon_lo + cell_lon_step();

    int drow = 0, dcol = 0;
    if (assoc_distance_km(lat, lon, lat_lo, lon) < reach_km && row > 0) {
        drow = -1;
    } else if (assoc_distance_km(lat, lon, lat_hi, lon) < reach_km &&
               row < CELL_ROWS - 1) {
        drow = 1;
    }
    if (assoc_distance_km(lat, lon, lat, lon_lo) < reach_km && col > 0) {
        dcol = -1;
    } else if (assoc_distance_km(lat, lon, lat, lon_hi) < reach_km &&
               col < CELL_COLS - 1) {
        dcol = 1;
    }

    if (drow != 0) {
        out[n++] = (row + drow) * CELL_COLS + col;
    }
    if (dcol != 0) {
        out[n++] = row * CELL_COLS + (col + dcol);
    }
    if (drow != 0 && dcol != 0) {
        out[n++] = (row + drow) * CELL_COLS + (col + dcol);
    }
    return n;
}

static void sort_ranks(int *v, int n)
{
    for (int i = 1; i < n; i++) {
        int k = v[i], j = i - 1;
        while (j >= 0 && v[j] > k) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = k;
    }
}

int cell_track_init(cell_track_t *ct)
{
    if (ct == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(ct, 0, sizeof *ct);

    for (int i = 0; i < CELL_COUNT; i++) {
        if (track_init(&ct->cells[i].track) != 0 ||
            pthread_mutex_init(&ct->cells[i].lock, NULL) != 0) {
            return -1;
        }
    }
    atomic_init(&ct->refused, 0);
    atomic_init(&ct->two_cell_plots, 0);
    return 0;
}

void cell_track_destroy(cell_track_t *ct)
{
    if (ct == NULL) {
        return;
    }
    for (int i = 0; i < CELL_COUNT; i++) {
        track_destroy(&ct->cells[i].track);
        pthread_mutex_destroy(&ct->cells[i].lock);
    }
}

static int submit_locked(cell_track_t *ct, const contact_t *c,
                         const int *ranks, int n,
                         const assoc_gate_t *gate, const valid_rules_t *rules,
                         char assoc_id[CONTACT_ID_MAX + 1])
{
    for (int i = 0; i < n; i++) {
        valid_err_t v = track_validate(&ct->cells[ranks[i]].track, c, rules);
        if (v != VALID_OK) {
            return -(2 + (int)v);
        }
    }

    const assoc_gate_t *g = (gate != NULL) ? gate : &ASSOC_DEFAULT;
    int    best_cell = ranks[0];
    double best_km   = 0.0;
    const track_entry_t *best = NULL;

    for (int i = 0; i < n; i++) {
        track_t *tr = &ct->cells[ranks[i]].track;
        const track_entry_t *e = track_find(tr, c->id);
        if (e == NULL) {
            e = track_associate(tr, c, g);
        }
        if (e == NULL) {
            continue;
        }
        double km = assoc_distance_km(e->lat, e->lon, c->lat, c->lon);
        if (best == NULL || km < best_km) {
            best      = e;
            best_km   = km;
            best_cell = ranks[i];
        }
    }

    contact_t filed = *c;
    int       cell  = ranks[0];
    if (best != NULL) {
        memcpy(filed.id, best->id, CONTACT_ID_MAX + 1);
        cell = best_cell;
    }
    if (assoc_id != NULL) {
        memcpy(assoc_id, filed.id, CONTACT_ID_MAX + 1);
    }

    return track_update(&ct->cells[cell].track, &filed);
}

static int submit(cell_track_t *ct, const contact_t *c,
                  const assoc_gate_t *gate, const valid_rules_t *rules,
                  char assoc_id[CONTACT_ID_MAX + 1], int ordered)
{
    if (ct == NULL || c == NULL) {
        errno = EINVAL;
        return -1;
    }

    const assoc_gate_t *g = (gate != NULL) ? gate : &ASSOC_DEFAULT;

    double reach_km = g->max_km +
        g->max_speed_kn * 1.852 * ((double)g->max_age_ns / 1e9 / 3600.0);

    int ranks[4];
    int n = cells_in_reach(c->lat, c->lon, reach_km, ranks);

    if (ordered) {
        sort_ranks(ranks, n);
    }

    for (int i = 0; i < n; i++) {
        if (pthread_mutex_trylock(&ct->cells[ranks[i]].lock) != 0) {
            pthread_mutex_lock(&ct->cells[ranks[i]].lock);
            ct->cells[ranks[i]].contended++;
        }
    }

    int rc = submit_locked(ct, c, ranks, n, gate, rules, assoc_id);

    for (int i = n - 1; i >= 0; i--) {
        pthread_mutex_unlock(&ct->cells[ranks[i]].lock);
    }

    if (n > 1) {
        atomic_fetch_add(&ct->two_cell_plots, 1);
    }
    if (rc < -1) {
        atomic_fetch_add(&ct->refused, 1);
    }

    return rc;
}

int cell_track_submit(cell_track_t *ct, const contact_t *c,
                      const assoc_gate_t *gate, const valid_rules_t *rules,
                      char assoc_id[CONTACT_ID_MAX + 1])
{
    return submit(ct, c, gate, rules, assoc_id, 1);
}

int cell_track_submit_naive(cell_track_t *ct, const contact_t *c,
                            const assoc_gate_t *gate,
                            const valid_rules_t *rules,
                            char assoc_id[CONTACT_ID_MAX + 1])
{
    return submit(ct, c, gate, rules, assoc_id, 0);
}

long cell_track_total_reports(cell_track_t *ct)
{
    long n = 0;
    for (int i = 0; i < CELL_COUNT; i++) {
        pthread_mutex_lock(&ct->cells[i].lock);
        n += track_total_reports(&ct->cells[i].track);
        pthread_mutex_unlock(&ct->cells[i].lock);
    }
    return n;
}

long cell_track_total_tracks(cell_track_t *ct)
{
    long n = 0;
    for (int i = 0; i < CELL_COUNT; i++) {
        pthread_mutex_lock(&ct->cells[i].lock);
        n += track_total_tracks(&ct->cells[i].track);
        pthread_mutex_unlock(&ct->cells[i].lock);
    }
    return n;
}

long cell_track_count_entries(cell_track_t *ct)
{
    long n = 0;
    for (int i = 0; i < CELL_COUNT; i++) {
        pthread_mutex_lock(&ct->cells[i].lock);
        n += track_count_entries(&ct->cells[i].track);
        pthread_mutex_unlock(&ct->cells[i].lock);
    }
    return n;
}

long cell_track_refused(cell_track_t *ct)
{
    return atomic_load(&ct->refused);
}

long cell_track_two_cell_plots(cell_track_t *ct)
{
    return atomic_load(&ct->two_cell_plots);
}
