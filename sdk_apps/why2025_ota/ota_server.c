#define _GNU_SOURCE
#include "ota_server.h"
#include "thirdparty/cJSON.h"

#include <arpa/inet.h>
#include <badgevms/application.h>
#include <badgevms/wifi.h>
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

static void send_status_body(int fd, int code, const char *status,
                             const char *ctype, const void *body, size_t blen) {
    char hdr[300];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.0 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             code, status, ctype, blen);
    write(fd, hdr, strlen(hdr));
    if (body && blen)
        write(fd, body, blen);
}

static void send_text(int fd, int code, const char *status, const char *text) {
    send_status_body(fd, code, status, "text/plain", text, strlen(text));
}

static void send_json_str(int fd, int code, const char *status, const char *json) {
    send_status_body(fd, code, status, "application/json", json, strlen(json));
}

/* ── Route handlers ───────────────────────────────────────────── */

static void handle_ping(int fd) {
    send_text(fd, 200, "OK", "pong");
}

static void handle_summaries(int fd) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) { send_text(fd, 500, "Error", "OOM"); return; }

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
    if (json) { send_json_str(fd, 200, "OK", json); free(json); }
    else       { send_text(fd, 500, "Error", "JSON OOM"); }
}

static void handle_latest_revision(int fd, const char *slug) {
    application_t *app = application_get(slug);
    if (!app) { send_text(fd, 404, "Not Found", "404"); return; }
    application_free(app);
    send_text(fd, 200, "OK", "1\n");
}

static void handle_revision_json(int fd, const char *slug, const char *our_ip) {
    application_t *app = application_get(slug);
    if (!app) { send_text(fd, 404, "Not Found", "404"); return; }

    const char *name = app->name        ? app->name        : slug;
    const char *bin  = app->binary_path ? app->binary_path : slug;

    cJSON *root    = cJSON_CreateObject();
    cJSON *version = cJSON_CreateObject();
    cJSON *meta    = cJSON_CreateObject();
    cJSON *app_arr = cJSON_CreateArray();
    cJSON *app_obj = cJSON_CreateObject();
    cJSON *files   = cJSON_CreateArray();

    if (!root || !version || !meta || !app_arr || !app_obj || !files) {
        application_free(app);
        cJSON_Delete(root);
        send_text(fd, 500, "Error", "OOM");
        return;
    }

    cJSON_AddStringToObject(meta, "name", name);
    cJSON_AddStringToObject(app_obj, "executable", bin);
    cJSON_AddItemToArray(app_arr, app_obj);
    cJSON_AddItemToObject(meta, "application", app_arr);
    cJSON_AddItemToObject(version, "app_metadata", meta);
    cJSON_AddItemToObject(version, "files", files);
    cJSON_AddItemToObject(root, "version", version);

    char url[300];

    /* ELF entry */
    cJSON *f1 = cJSON_CreateObject();
    snprintf(url, sizeof(url), "http://%s/api/v3/projects/%s/rev1/files/%s",
             our_ip, slug, bin);
    if (f1) {
        cJSON_AddStringToObject(f1, "url",       url);
        cJSON_AddStringToObject(f1, "full_path", bin);
        cJSON_AddItemToArray(files, f1);
    }

    /* version.txt entry */
    cJSON *f2 = cJSON_CreateObject();
    snprintf(url, sizeof(url), "http://%s/api/v3/projects/%s/rev1/files/version.txt",
             our_ip, slug);
    if (f2) {
        cJSON_AddStringToObject(f2, "url",       url);
        cJSON_AddStringToObject(f2, "full_path", "version.txt");
        cJSON_AddItemToArray(files, f2);
    }

    application_free(app);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) { send_json_str(fd, 200, "OK", json); free(json); }
    else       { send_text(fd, 500, "Error", "JSON OOM"); }
}

static void handle_file(int fd, const char *slug, const char *filename) {
    application_t *app = application_get(slug);
    if (!app) { send_text(fd, 404, "Not Found", "404"); return; }

    /* version.txt — serve version string from memory */
    if (strcmp(filename, "version.txt") == 0) {
        const char *ver = app->version ? app->version : "0";
        send_text(fd, 200, "OK", ver);
        application_free(app);
        return;
    }

    /* Other files — read from filesystem */
    char *abs = application_create_file_string(app, filename);
    application_free(app);

    if (!abs) { send_text(fd, 404, "Not Found", "Path error"); return; }

    FILE *f = fopen(abs, "r");
    free(abs);
    if (!f) { send_text(fd, 404, "Not Found", "File not found"); return; }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 0) {
        fclose(f);
        send_text(fd, 500, "Error", "Seek failed");
        return;
    }

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.0 200 OK\r\n"
             "Content-Type: application/octet-stream\r\n"
             "Content-Length: %ld\r\n"
             "Connection: close\r\n"
             "\r\n",
             fsize);
    write(fd, hdr, strlen(hdr));

    char   buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        write(fd, buf, n);
    fclose(f);
}

/* ── URL dispatcher ───────────────────────────────────────────── */

static void dispatch(int fd, const char *path, const char *our_ip) {
    /* /api/v3/ping */
    if (strncmp(path, "/api/v3/ping", 12) == 0) {
        handle_ping(fd);
        return;
    }
    /* /api/v3/project-summaries */
    if (strncmp(path, "/api/v3/project-summaries", 25) == 0) {
        handle_summaries(fd);
        return;
    }
    /* /api/v3/project-latest-revisions/{slug} */
    if (strncmp(path, "/api/v3/project-latest-revisions/", 33) == 0) {
        handle_latest_revision(fd, path + 33);
        return;
    }
    /* /api/v3/projects/{slug}/... */
    if (strncmp(path, "/api/v3/projects/", 17) == 0) {
        const char *rest  = path + 17;
        const char *slash = strchr(rest, '/');
        if (!slash) { send_text(fd, 404, "Not Found", "404"); return; }

        char   slug[128] = {0};
        size_t slen = (size_t)(slash - rest);
        if (slen >= sizeof(slug)) slen = sizeof(slug) - 1;
        memcpy(slug, rest, slen);

        /* skip /rev{n} */
        const char *after  = slash + 1;
        const char *fslash = strchr(after, '/');
        if (!fslash) {
            handle_revision_json(fd, slug, our_ip);
            return;
        }
        if (strncmp(fslash, "/files/", 7) == 0) {
            handle_file(fd, slug, fslash + 7);
            return;
        }
        send_text(fd, 404, "Not Found", "404");
        return;
    }
    send_text(fd, 404, "Not Found", "404");
}

/* ── Server loop (runs in background thread) ──────────────────── */

void ota_host_server_thread(void *arg) {
    ota_host_state_t *s = (ota_host_state_t *)arg;

    /* Broadcast our own AP — old badge connects to it directly.
     * DHCP will advertise 192.168.4.1 as the DNS server automatically. */
    wifi_start_ap("BadgeVMS-OTA", "");
    strncpy(s->ip, "192.168.4.1", sizeof(s->ip) - 1);
    s->ip[sizeof(s->ip) - 1] = '\0';

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { atomic_store(&s->running, false); return; }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Short accept timeout so we can poll stop_requested */
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
    printf("[OTA host] listening on %s:%d\n", s->ip, OTA_HOST_PORT);

    while (!atomic_load(&s->stop_requested)) {
        struct sockaddr_in cli;
        socklen_t          cli_len = sizeof(cli);
        int cfd = accept(lfd, (struct sockaddr *)&cli, &cli_len);
        if (cfd < 0) continue; /* timeout — re-check stop_requested */

        struct timeval rtv = {.tv_sec = 10, .tv_usec = 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

        /* Read request headers */
        char req[2048] = {0};
        int  total     = 0;
        while (total < (int)sizeof(req) - 1) {
            ssize_t n = read(cfd, req + total, sizeof(req) - 1 - total);
            if (n <= 0) break;
            total += (int)n;
            if (strstr(req, "\r\n\r\n")) break;
        }

        char method[16] = {0}, path[512] = {0};
        sscanf(req, "%15s %511s", method, path);

        /* Strip query string */
        char *q = strchr(path, '?');
        if (q) *q = '\0';

        if (method[0] && path[0]) {
            printf("[OTA host] %s %s\n", method, path);
            dispatch(cfd, path, s->ip);
            atomic_fetch_add(&s->requests_served, 1);
        }
        close(cfd);
    }

    close(lfd);
    s->listen_fd = -1;
    wifi_stop_ap();
    atomic_store(&s->running, false);
    printf("[OTA host] stopped\n");
}
