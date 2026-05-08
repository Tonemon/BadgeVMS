#pragma once

#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>

#define OTA_HOST_PORT     80
#define OTA_HOST_TLS_PORT 443

typedef struct {
    char        ip[32];
    int         listen_fd;
    atomic_bool running;
    atomic_bool stop_requested;
    atomic_int  requests_served;
} ota_host_state_t;

bool ota_host_get_ip(char *ip_out, size_t len);
void ota_host_server_thread(void *arg);
void ota_host_tls_server_thread(void *arg);
