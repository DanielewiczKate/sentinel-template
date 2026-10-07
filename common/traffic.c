#include "traffic.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EARTH_KM 6371.0088

static int cmp_id(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

double traffic_distance_km(double lat1, double lon1, double lat2, double lon2)
{
    double rad  = M_PI / 180.0;
    double dlat = (lat2 - lat1) * rad;
    double dlon = (lon2 - lon1) * rad;
    double h    = sin(dlat / 2) * sin(dlat / 2) +
                  cos(lat1 * rad) * cos(lat2 * rad) * sin(dlon / 2) * sin(dlon / 2);
    return 2.0 * EARTH_KM * asin(sqrt(h));
}

static int grow(void **arr, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) {
        return 0;
    }
    size_t ncap = *cap ? *cap * 2 : 64;
    while (ncap < need) {
        ncap *= 2;
    }
    void *p = realloc(*arr, ncap * elem);
    if (p == NULL) {
        return -1;
    }
    *arr = p;
    *cap = ncap;
    return 0;
}

int traffic_load(const char *path, traffic_t *t, long *bad_line)
{
    if (path == NULL || t == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(t, 0, sizeof *t);
    if (bad_line != NULL) {
        *bad_line = 0;
    }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }

    size_t id_cap = 0, zone_cap = 0, region_cap = 0;
    char   line[256];
    long   lineno = 0;

    while (fgets(line, sizeof line, f) != NULL) {
        lineno++;

        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') {
            continue;
        }

        char kind[8], name[64];
        double lat, lon, radius, lat_n, lon_e;
        char extra;

        if (sscanf(p, "%7s", kind) != 1) {
            goto bad;
        }

        if (strcmp(kind, "id") == 0) {
            if (sscanf(p, "id %63s %c", name, &extra) != 1 ||
                strlen(name) > CONTACT_ID_MAX) {
                goto bad;
            }
            void *arr = t->ids;
            if (grow(&arr, &id_cap, t->n_ids + 1, sizeof *t->ids) != 0) {
                goto fail;
            }
            t->ids = arr;
            memcpy(t->ids[t->n_ids], name, strlen(name) + 1);
            t->n_ids++;
        } else if (strcmp(kind, "zone") == 0) {
            if (sscanf(p, "zone %63s %lf %lf %lf %c", name, &lat, &lon, &radius, &extra) != 4 ||
                strlen(name) > TRAFFIC_ZONE_NAME_MAX ||
                !isfinite(lat) || lat < -90.0 || lat > 90.0 ||
                !isfinite(lon) || lon < -180.0 || lon > 180.0 ||
                !isfinite(radius) || radius <= 0.0) {
                goto bad;
            }
            void *arr = t->zones;
            if (grow(&arr, &zone_cap, t->n_zones + 1, sizeof *t->zones) != 0) {
                goto fail;
            }
            t->zones = arr;
            traffic_zone_t *z = &t->zones[t->n_zones++];
            memcpy(z->name, name, strlen(name) + 1);
            z->lat       = lat;
            z->lon       = lon;
            z->radius_km = radius;
        } else if (strcmp(kind, "region") == 0) {
            if (sscanf(p, "region %63s %lf %lf %lf %lf %c",
                       name, &lat, &lat_n, &lon, &lon_e, &extra) != 5 ||
                strlen(name) > TRAFFIC_ZONE_NAME_MAX ||
                !isfinite(lat) || !isfinite(lat_n) || lat < -90.0 ||
                lat_n > 90.0 || lat >= lat_n ||
                !isfinite(lon) || !isfinite(lon_e) || lon < -180.0 ||
                lon_e > 180.0 || lon >= lon_e) {
                goto bad;
            }
            void *arr = t->regions;
            if (grow(&arr, &region_cap, t->n_regions + 1, sizeof *t->regions) != 0) {
                goto fail;
            }
            t->regions = arr;
            traffic_region_t *r = &t->regions[t->n_regions++];
            memcpy(r->name, name, strlen(name) + 1);
            r->lat_s = lat;
            r->lat_n = lat_n;
            r->lon_w = lon;
            r->lon_e = lon_e;
        } else {
            goto bad;
        }
    }

    if (ferror(f)) {
        goto fail;
    }
    fclose(f);

    qsort(t->ids, t->n_ids, sizeof *t->ids, cmp_id);
    return 0;

bad:
    if (bad_line != NULL) {
        *bad_line = lineno;
    }
    fclose(f);
    traffic_free(t);
    errno = EINVAL;
    return -1;

fail: {
        int saved = errno;
        fclose(f);
        traffic_free(t);
        errno = saved;
        return -1;
    }
}

int traffic_is_filed(const traffic_t *t, const contact_t *c)
{
    if (t == NULL || c == NULL) {
        return 0;
    }
    if (t->n_ids > 0 &&
        bsearch(c->id, t->ids, t->n_ids, sizeof *t->ids, cmp_id) != NULL) {
        return 1;
    }
    for (size_t i = 0; i < t->n_zones; i++) {
        const traffic_zone_t *z = &t->zones[i];
        if (traffic_distance_km(c->lat, c->lon, z->lat, z->lon) <= z->radius_km) {
            return 1;
        }
    }
    return 0;
}

const traffic_region_t *traffic_in_region(const traffic_t *t, const contact_t *c)
{
    if (t == NULL || c == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < t->n_regions; i++) {
        const traffic_region_t *r = &t->regions[i];
        if (c->lat >= r->lat_s && c->lat <= r->lat_n &&
            c->lon >= r->lon_w && c->lon <= r->lon_e) {
            return r;
        }
    }
    return NULL;
}

void traffic_free(traffic_t *t)
{
    if (t == NULL) {
        return;
    }
    free(t->ids);
    free(t->zones);
    free(t->regions);
    memset(t, 0, sizeof *t);
}

void traffic_watch_init(traffic_watch_t *w)
{
    if (w != NULL) {
        memset(w, 0, sizeof *w);
    }
}

static void note_sensor(traffic_unknown_t *u, const char *sensor)
{
    for (int i = 0; i < u->n_sensors; i++) {
        if (strcmp(u->sensors[i], sensor) == 0) {
            return;
        }
    }
    if (u->n_sensors < SENSOR_COUNT) {
        memcpy(u->sensors[u->n_sensors++], sensor, CONTACT_SENSOR_LEN + 1);
    }
}

static void note_region(traffic_unknown_t *u, const traffic_t *t,
                        const contact_t *c)
{
    const traffic_region_t *r = traffic_in_region(t, c);
    if (r == NULL) {
        u->region[0] = '\0';
    } else {
        memcpy(u->region, r->name, strlen(r->name) + 1);
    }
}

int traffic_watch(traffic_watch_t *w, const traffic_t *t, const contact_t *c,
                  int64_t now_ns)
{
    if (w == NULL || t == NULL || c == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (traffic_is_filed(t, c)) {
        return 0;
    }
    w->reports++;

    int64_t window = (int64_t)TRAFFIC_SAME_S * 1000000000;
    for (int i = 0; i < w->n; i++) {
        traffic_unknown_t *u = &w->items[i];
        int same = strcmp(u->id, c->id) == 0 ||
                   (now_ns - u->last_ns <= window &&
                    traffic_distance_km(u->lat, u->lon, c->lat, c->lon) <= TRAFFIC_SAME_KM);
        if (same) {
            u->lat     = c->lat;
            u->lon     = c->lon;
            u->last_ns = now_ns;
            u->reports++;
            note_sensor(u, c->sensor);
            note_region(u, t, c);
            return 0;
        }
    }

    int slot = w->n;
    if (w->n == TRAFFIC_UNKNOWN_MAX) {
        slot = 0;
        for (int i = 1; i < w->n; i++) {
            if (w->items[i].last_ns < w->items[slot].last_ns) {
                slot = i;
            }
        }
    } else {
        w->n++;
    }

    traffic_unknown_t *u = &w->items[slot];
    memset(u, 0, sizeof *u);
    memcpy(u->id, c->id, sizeof u->id);
    memcpy(u->sensor, c->sensor, sizeof u->sensor);
    u->lat      = c->lat;
    u->lon      = c->lon;
    u->reports  = 1;
    u->first_ns = now_ns;
    u->last_ns  = now_ns;
    note_sensor(u, c->sensor);
    note_region(u, t, c);
    return 1;
}

int traffic_json_string(FILE *f, const char *s)
{
    if (fputc('"', f) == EOF) {
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int rc;
        if (*p == '"' || *p == '\\') {
            rc = fprintf(f, "\\%c", *p);
        } else if (*p < 0x20 || *p >= 0x7f) {
            rc = fprintf(f, "\\u%04x", *p);
        } else {
            rc = fputc(*p, f) == EOF ? -1 : 1;
        }
        if (rc < 0) {
            return -1;
        }
    }
    return fputc('"', f) == EOF ? -1 : 0;
}

int traffic_watch_write_json(const traffic_watch_t *w, FILE *f, int64_t now_ns)
{
    if (w == NULL || f == NULL) {
        errno = EINVAL;
        return -1;
    }
    fputc('[', f);
    for (int i = 0; i < w->n; i++) {
        const traffic_unknown_t *u = &w->items[i];
        fprintf(f, "%s{\"id\":", i ? "," : "");
        traffic_json_string(f, u->id);
        fprintf(f, ",\"sensor\":");
        traffic_json_string(f, u->sensor);

        fprintf(f, ",\"sensors\":[");
        for (int s = 0; s < u->n_sensors; s++) {
            if (s) {
                fputc(',', f);
            }
            traffic_json_string(f, u->sensors[s]);
        }
        fputc(']', f);

        fprintf(f, ",\"lat\":%.4f,\"lon\":%.4f,\"region\":", u->lat, u->lon);
        if (u->region[0] != '\0') {
            traffic_json_string(f, u->region);
        } else {
            fputs("null", f);
        }
        fprintf(f, ",\"reports\":%ld,\"age_ms\":%lld}",
                u->reports, (long long)((now_ns - u->last_ns) / 1000000));
    }
    fputc(']', f);
    return ferror(f) ? -1 : 0;
}
