#ifndef SENTINEL_SUPERVISE_H
#define SENTINEL_SUPERVISE_H

#include "sentinel/proc.h"

int supervise_install(void);

int supervise_reap(sensor_net_t *net, int restart);

int supervise_pending(void);

#endif
