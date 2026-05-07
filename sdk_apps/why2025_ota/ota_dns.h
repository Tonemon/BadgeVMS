#pragma once

#include <stdbool.h>
#include <stdatomic.h>

typedef struct {
    char        ip[32];          /* IP to advertise for badge.why2025.org */
    atomic_bool running;
    atomic_bool stop_requested;
    atomic_int  queries_answered;
} ota_dns_state_t;

void ota_dns_server_thread(void *arg);
