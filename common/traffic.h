#ifndef SENTINEL_TRAFFIC_H
#define SENTINEL_TRAFFIC_H

#include "contact.h"

#include <stddef.h>
#include <stdio.h>

#define TRAFFIC_ZONE_NAME_MAX 31

typedef struct {
    char   name[TRAFFIC_ZONE_NAME_MAX + 1];
    double lat, lon;
    double radius_km;
} traffic_zone_t;

typedef struct {
    char   name[TRAFFIC_ZONE_NAME_MAX + 1];
    double lat_s, lat_n;
    double lon_w, lon_e;
} traffic_region_t;

typedef struct {
    char              (*ids)[CONTACT_ID_MAX + 1];
    size_t              n_ids;
    traffic_zone_t     *zones;
    size_t              n_zones;
    traffic_region_t   *regions;
    size_t              n_regions;
} traffic_t;

int traffic_load(const char *path, traffic_t *t, long *bad_line);

int traffic_is_filed(const traffic_t *t, const contact_t *c);

const traffic_region_t *traffic_in_region(const traffic_t *t, const contact_t *c);

void traffic_free(traffic_t *t);

double traffic_distance_km(double lat1, double lon1, double lat2, double lon2);

#define TRAFFIC_UNKNOWN_MAX 256
#define TRAFFIC_SAME_KM     25.0
#define TRAFFIC_SAME_S      30

typedef struct {
    char    id[CONTACT_ID_MAX + 1];
    char    sensor[CONTACT_SENSOR_LEN + 1];
    double  lat, lon;
    long    reports;
    int64_t first_ns, last_ns;

    char    sensors[SENSOR_COUNT][CONTACT_SENSOR_LEN + 1];
    int     n_sensors;

    char    region[TRAFFIC_ZONE_NAME_MAX + 1];
} traffic_unknown_t;

typedef struct {
    traffic_unknown_t items[TRAFFIC_UNKNOWN_MAX];
    int               n;
    long              reports;
} traffic_watch_t;

void traffic_watch_init(traffic_watch_t *w);

int traffic_watch(traffic_watch_t *w, const traffic_t *t, const contact_t *c,
                  int64_t now_ns);

int traffic_watch_write_json(const traffic_watch_t *w, FILE *f, int64_t now_ns);

int traffic_json_string(FILE *f, const char *s);

#endif
