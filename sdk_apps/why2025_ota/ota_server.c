#define _GNU_SOURCE
#include "ota_server.h"
#include "thirdparty/cJSON.h"

#include <arpa/inet.h>
#include <badgevms/application.h>
#include <badgevms/misc_funcs.h>
#include <badgevms/ota.h>
#include <badgevms/process.h>
#include <badgevms/tls_server.h>
#include <badgevms/wifi.h>

static ota_host_state_t *g_host_state = NULL;

static void ap_sta_joined(const uint8_t *mac, const char *ip) {
    if (!g_host_state) return;
    snprintf(g_host_state->last_client_mac, sizeof(g_host_state->last_client_mac),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    strncpy(g_host_state->last_client_ip, ip, sizeof(g_host_state->last_client_ip) - 1);
    g_host_state->last_client_ip[sizeof(g_host_state->last_client_ip) - 1] = '\0';
    atomic_store(&g_host_state->has_last_client, true);
    printf("[OTA host] Badge joined: MAC=%s IP=%s\n",
           g_host_state->last_client_mac, g_host_state->last_client_ip);
}
#include <dirent.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* lwIP omits these from the stripped SDK headers */
#ifndef SOL_SOCKET
#define SOL_SOCKET 0xfff
#endif
#ifndef INADDR_ANY
#define INADDR_ANY ((in_addr_t)0x00000000)
#endif

/* ── Connection abstraction (plain or TLS) ────────────────────── */

typedef struct {
    int        fd;
    tls_conn_t tls; /* NULL for plain HTTP */
} conn_ctx_t;

static ssize_t conn_read(conn_ctx_t *c, void *buf, size_t len) {
    if (c->tls) return tls_conn_read(c->tls, buf, len);
    return read(c->fd, buf, len);
}

static ssize_t conn_write(conn_ctx_t *c, const void *buf, size_t len) {
    if (c->tls) return tls_conn_write(c->tls, buf, len);
    return write(c->fd, buf, len);
}

static void conn_close(conn_ctx_t *c) {
    if (c->tls) tls_conn_close(c->tls);
    close(c->fd);
}

bool ota_host_get_ip(char *ip_out, size_t len) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(53);
    inet_aton("8.8.8.8", &sa.sin_addr);

    /* UDP connect sets the routing decision without sending anything,
       so getsockname() returns the local address the kernel would use. */
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(sock);
        return false;
    }

    struct sockaddr_in local;
    socklen_t          local_len = sizeof(local);
    int                ok        = getsockname(sock, (struct sockaddr *)&local, &local_len);
    close(sock);

    if (ok < 0 || local.sin_addr.s_addr == 0) return false;

    strncpy(ip_out, inet_ntoa(local.sin_addr), len - 1);
    ip_out[len - 1] = '\0';
    return true;
}

/* ── HTTP helpers ─────────────────────────────────────────────── */

static void send_status_body(conn_ctx_t *c, int code, const char *status,
                             const char *ctype, const void *body, size_t blen) {
    char hdr[300];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.0 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             code, status, ctype, blen);
    conn_write(c, hdr, strlen(hdr));
    if (body && blen)
        conn_write(c, body, blen);
}

static void send_text(conn_ctx_t *c, int code, const char *status, const char *text) {
    send_status_body(c, code, status, "text/plain", text, strlen(text));
}

static void send_json_str(conn_ctx_t *c, int code, const char *status, const char *json) {
    send_status_body(c, code, status, "application/json", json, strlen(json));
}

/* ── Route handlers ───────────────────────────────────────────── */

static void handle_ping(conn_ctx_t *c) {
    send_text(c, 200, "OK", "pong");
}

static void handle_summaries(conn_ctx_t *c) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) { send_text(c, 500, "Error", "OOM"); return; }

    application_t          *app;
    application_list_handle list = application_list(&app);
    while (app) {
        if (app->source == APPLICATION_SOURCE_BADGEHUB) {
            cJSON *item = cJSON_CreateObject();
            if (item) {
                cJSON_AddStringToObject(item, "slug", app->unique_identifier);
                cJSON_AddStringToObject(item, "name",
                    app->name ? app->name : app->unique_identifier);
                cJSON_AddItemToArray(arr, item);
            }
        }
        app = application_list_get_next(list);
    }
    application_list_close(list);

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (json) { send_json_str(c, 200, "OK", json); free(json); }
    else       { send_text(c, 500, "Error", "JSON OOM"); }
}

static void handle_latest_revision(conn_ctx_t *c, const char *slug) {
    if (strcmp(slug, "why2025_firmware") == 0) {
        send_text(c, 200, "OK", "1\n");
        return;
    }
    application_t *app = application_get(slug);
    if (!app) { send_text(c, 404, "Not Found", "404"); return; }
    application_free(app);
    send_text(c, 200, "OK", "1\n");
}

static void build_revision_json(conn_ctx_t *c, const char *slug, const char *name,
                                const char *bin, const char *dir_path,
                                const char *our_ip) {
    cJSON *root    = cJSON_CreateObject();
    cJSON *version = cJSON_CreateObject();
    cJSON *meta    = cJSON_CreateObject();
    cJSON *app_arr = cJSON_CreateArray();
    cJSON *app_obj = cJSON_CreateObject();
    cJSON *files   = cJSON_CreateArray();

    if (!root || !version || !meta || !app_arr || !app_obj || !files) {
        cJSON_Delete(root);
        send_text(c, 500, "Error", "OOM");
        return;
    }

    cJSON_AddStringToObject(meta, "name", name);
    if (bin) cJSON_AddStringToObject(app_obj, "executable", bin);
    cJSON_AddItemToArray(app_arr, app_obj);
    cJSON_AddItemToObject(meta, "application", app_arr);
    cJSON_AddItemToObject(version, "app_metadata", meta);
    cJSON_AddItemToObject(version, "files", files);
    cJSON_AddItemToObject(root, "version", version);

    char url[512];

    if (dir_path) {
        DIR *dir = opendir(dir_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                if (strcmp(entry->d_name, "manifest.json") == 0) continue;
                cJSON *f = cJSON_CreateObject();
                if (!f) continue;
                snprintf(url, sizeof(url),
                         "http://%s/api/v3/projects/%s/rev1/files/%s",
                         our_ip, slug, entry->d_name);
                cJSON_AddStringToObject(f, "url",       url);
                cJSON_AddStringToObject(f, "full_path", entry->d_name);
                cJSON_AddItemToArray(files, f);
            }
            closedir(dir);
        }
    } else {
        cJSON *f1 = cJSON_CreateObject();
        snprintf(url, sizeof(url), "http://%s/api/v3/projects/%s/rev1/files/badgevms.bin",
                 our_ip, slug);
        if (f1) {
            cJSON_AddStringToObject(f1, "url",       url);
            cJSON_AddStringToObject(f1, "full_path", "badgevms.bin");
            cJSON_AddItemToArray(files, f1);
        }
        cJSON *f2 = cJSON_CreateObject();
        snprintf(url, sizeof(url), "http://%s/api/v3/projects/%s/rev1/files/version.txt",
                 our_ip, slug);
        if (f2) {
            cJSON_AddStringToObject(f2, "url",       url);
            cJSON_AddStringToObject(f2, "full_path", "version.txt");
            cJSON_AddItemToArray(files, f2);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) { send_json_str(c, 200, "OK", json); free(json); }
    else       { send_text(c, 500, "Error", "JSON OOM"); }
}

static void handle_revision_json(conn_ctx_t *c, const char *slug, const char *our_ip) {
    if (strcmp(slug, "why2025_firmware") == 0) {
        build_revision_json(c, slug, "BadgeVMS Firmware", NULL, NULL, our_ip);
        return;
    }

    application_t *app = application_get(slug);
    if (!app) { send_text(c, 404, "Not Found", "404"); return; }

    const char *name = app->name        ? app->name        : slug;
    const char *bin  = app->binary_path ? app->binary_path : slug;

    char *sample = application_create_file_string(app, "version.txt");

    char *dir_path = NULL;
    if (sample) {
        char *slash = strrchr(sample, '/');
        if (slash) *slash = '\0';
        dir_path = sample;
    }

    build_revision_json(c, slug, name, bin, dir_path, our_ip);
    application_free(app);
    free(sample);
}

static void send_file_stream(conn_ctx_t *c, FILE *f, long fsize) {
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.0 200 OK\r\n"
             "Content-Type: application/octet-stream\r\n"
             "Content-Length: %ld\r\n"
             "Connection: close\r\n"
             "\r\n",
             fsize);
    conn_write(c, hdr, strlen(hdr));

    char   buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        conn_write(c, buf, n);
}

static void handle_file(conn_ctx_t *c, const char *slug, const char *filename) {
    if (strcmp(slug, "why2025_firmware") == 0) {
        if (strcmp(filename, "version.txt") == 0) {
            char *ver = NULL;
            if (ota_get_running_version(&ver) && ver) {
                send_text(c, 200, "OK", ver);
                free(ver);
            } else {
                send_text(c, 200, "OK", "0");
            }
            return;
        }
        if (strcmp(filename, "badgevms.bin") == 0) {
            size_t fwsize = ota_get_firmware_size();
            if (!fwsize) { send_text(c, 500, "Error", "Firmware size error"); return; }

            char hdr[256];
            snprintf(hdr, sizeof(hdr),
                     "HTTP/1.0 200 OK\r\n"
                     "Content-Type: application/octet-stream\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     fwsize);
            conn_write(c, hdr, strlen(hdr));

            char   buf[4096];
            size_t offset = 0;
            while (offset < fwsize) {
                size_t chunk = sizeof(buf);
                if (offset + chunk > fwsize) chunk = fwsize - offset;
                if (!ota_read_firmware(offset, buf, chunk)) break;
                conn_write(c, buf, chunk);
                offset += chunk;
            }
            return;
        }
        send_text(c, 404, "Not Found", "404");
        return;
    }

    application_t *app = application_get(slug);
    if (!app) { send_text(c, 404, "Not Found", "404"); return; }

    if (strcmp(filename, "version.txt") == 0) {
        const char *ver = app->version ? app->version : "0";
        send_text(c, 200, "OK", ver);
        application_free(app);
        return;
    }

    char *abs = application_create_file_string(app, filename);
    application_free(app);

    if (!abs) { send_text(c, 404, "Not Found", "Path error"); return; }

    FILE *f = fopen(abs, "r");
    free(abs);
    if (!f) { send_text(c, 404, "Not Found", "File not found"); return; }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 0) {
        fclose(f);
        send_text(c, 500, "Error", "Seek failed");
        return;
    }

    send_file_stream(c, f, fsize);
    fclose(f);
}

/* ── URL dispatcher ───────────────────────────────────────────── */

static void dispatch(conn_ctx_t *c, const char *path, const char *our_ip) {
    if (strncmp(path, "/api/v3/ping", 12) == 0) {
        handle_ping(c);
        return;
    }
    if (strncmp(path, "/api/v3/project-summaries", 25) == 0) {
        handle_summaries(c);
        return;
    }
    if (strncmp(path, "/api/v3/project-latest-revisions/", 33) == 0) {
        handle_latest_revision(c, path + 33);
        return;
    }
    if (strncmp(path, "/api/v3/projects/", 17) == 0) {
        const char *rest  = path + 17;
        const char *slash = strchr(rest, '/');
        if (!slash) { send_text(c, 404, "Not Found", "404"); return; }

        char   slug[128] = {0};
        size_t slen = (size_t)(slash - rest);
        if (slen >= sizeof(slug)) slen = sizeof(slug) - 1;
        memcpy(slug, rest, slen);

        const char *after  = slash + 1;
        const char *fslash = strchr(after, '/');
        if (!fslash) {
            handle_revision_json(c, slug, our_ip);
            return;
        }
        if (strncmp(fslash, "/files/", 7) == 0) {
            handle_file(c, slug, fslash + 7);
            return;
        }
        send_text(c, 404, "Not Found", "404");
        return;
    }
    send_text(c, 404, "Not Found", "404");
}

/* ── Shared per-connection request handler ────────────────────── */

static void handle_connection(conn_ctx_t *c, const char *our_ip,
                               ota_host_state_t *s) {
    char req[2048] = {0};
    int  total     = 0;
    while (total < (int)sizeof(req) - 1) {
        ssize_t n = conn_read(c, req + total, sizeof(req) - 1 - total);
        if (n <= 0) break;
        total += (int)n;
        if (strstr(req, "\r\n\r\n")) break;
    }

    char method[16] = {0}, path[512] = {0};
    sscanf(req, "%15s %511s", method, path);

    char *q = strchr(path, '?');
    if (q) *q = '\0';

    if (method[0] && path[0]) {
        printf("[OTA host] %s %s\n", method, path);
        dispatch(c, path, our_ip);
        atomic_fetch_add(&s->requests_served, 1);
    }
}

/* ── Corruption monitor (polls ap_sta_joined[0] every 50 ms) ─── */

static void psram_corruption_monitor(void *arg) {
    (void)arg;
    uint32_t paddr = vaddr_to_paddr((uint32_t)(uintptr_t)ap_sta_joined);
    uint32_t expected;
    memcpy(&expected, (void *)ap_sta_joined, 4);
    printf("[OTA monitor] start: ap_sta_joined=%p paddr=0x%08lx word=0x%08lx\n",
           (void *)ap_sta_joined, (unsigned long)paddr, (unsigned long)expected);
    unsigned int ticks = 0;
    for (;;) {
        usleep(50000); /* 50 ms */
        ticks++;
        uint32_t cur;
        memcpy(&cur, (void *)ap_sta_joined, 4);
        if (cur != expected) {
            printf("[OTA monitor] CORRUPTION at t=%ums! 0x%08lx -> 0x%08lx\n",
                   ticks * 50, (unsigned long)expected, (unsigned long)cur);
            expected = cur;
        }
    }
}

/* ── HTTP server loop (runs in background thread) ─────────────── */

void ota_host_server_thread(void *arg) {
    ota_host_state_t *s = (ota_host_state_t *)arg;

    g_host_state = s;

#define DIAG_CB() do { \
    uint32_t _w; memcpy(&_w, (void *)ap_sta_joined, 4); \
    printf("[OTA diag] ap_sta_joined[0]=0x%08lx @ %s:%d\n", (unsigned long)_w, __func__, __LINE__); \
} while (0)

    DIAG_CB(); /* before wifi_set_ap_sta_joined_cb */
    wifi_set_ap_sta_joined_cb(ap_sta_joined);
    wifi_start_ap("WHY2025-open", "");
    DIAG_CB(); /* after wifi_start_ap */
    thread_create(psram_corruption_monitor, NULL, 4096);
    strncpy(s->ip, "192.168.4.1", sizeof(s->ip) - 1);
    s->ip[sizeof(s->ip) - 1] = '\0';

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { atomic_store(&s->running, false); return; }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(OTA_HOST_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(lfd, 4) < 0) {
        close(lfd);
        atomic_store(&s->running, false);
        return;
    }

    s->listen_fd = lfd;
    atomic_store(&s->running, true);
    printf("[OTA host] HTTP listening on %s:%d\n", s->ip, OTA_HOST_PORT);

    while (!atomic_load(&s->stop_requested)) {
        struct sockaddr_in cli;
        socklen_t          cli_len = sizeof(cli);
        int cfd = accept(lfd, (struct sockaddr *)&cli, &cli_len);
        if (cfd < 0) continue;

        struct timeval rtv = {.tv_sec = 10, .tv_usec = 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &rtv, sizeof(rtv));

        conn_ctx_t c = { .fd = cfd, .tls = NULL };
        handle_connection(&c, s->ip, s);
        conn_close(&c);
    }

    close(lfd);
    s->listen_fd = -1;
    wifi_set_ap_sta_joined_cb(NULL);
    g_host_state = NULL;
    wifi_stop_ap();
    atomic_store(&s->running, false);
    printf("[OTA host] HTTP stopped\n");
}

/* ── HTTPS server loop (runs in background thread) ────────────── */

void ota_host_tls_server_thread(void *arg) {
    ota_host_state_t *s = (ota_host_state_t *)arg;

    /* Generate a self-signed certificate */
    uint8_t *cert_der = NULL, *key_der = NULL;
    size_t   cert_len = 0,     key_len  = 0;

    DIAG_CB(); /* before tls_generate_selfsigned */
    printf("[OTA host] Generating self-signed certificate...\n");
    if (!tls_generate_selfsigned(&cert_der, &cert_len, &key_der, &key_len)) {
        printf("[OTA host] TLS cert generation failed, HTTPS not available\n");
        return;
    }
    DIAG_CB(); /* after tls_generate_selfsigned */
    printf("[OTA host] Certificate generated (%zu bytes)\n", cert_len);

    /* tls_server_ctx_create takes ownership of cert_der/key_der and frees them */
    tls_server_ctx_t tls_ctx = tls_server_ctx_create(cert_der, cert_len, key_der, key_len);
    DIAG_CB(); /* after tls_server_ctx_create */
    if (!tls_ctx) {
        printf("[OTA host] TLS context creation failed, HTTPS not available\n");
        return;
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { tls_server_ctx_free(tls_ctx); return; }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(OTA_HOST_TLS_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(lfd, 4) < 0) {
        close(lfd);
        tls_server_ctx_free(tls_ctx);
        printf("[OTA host] HTTPS bind/listen failed\n");
        return;
    }

    printf("[OTA host] HTTPS listening on %s:%d\n", s->ip, OTA_HOST_TLS_PORT);

    while (!atomic_load(&s->stop_requested)) {
        struct sockaddr_in cli;
        socklen_t          cli_len = sizeof(cli);
        int cfd = accept(lfd, (struct sockaddr *)&cli, &cli_len);
        if (cfd < 0) continue;

        struct timeval rtv = {.tv_sec = 10, .tv_usec = 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &rtv, sizeof(rtv));

        int real_fd = get_dev_fd(cfd);
        tls_conn_t tls = (real_fd >= 0) ? tls_server_accept_fd(tls_ctx, real_fd) : NULL;
        if (!tls) {
            close(cfd);
            continue;
        }

        conn_ctx_t c = { .fd = cfd, .tls = tls };
        handle_connection(&c, s->ip, s);
        conn_close(&c);
    }

    close(lfd);
    tls_server_ctx_free(tls_ctx);
    printf("[OTA host] HTTPS stopped\n");
}
