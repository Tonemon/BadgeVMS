#include "ota_dns.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
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

#define DNS_PORT 53

/* DNS-wire-encoded "badge.why2025.org\0" */
static const uint8_t TARGET_NAME[] = {
    5, 'b','a','d','g','e',
    7, 'w','h','y','2','0','2','5',
    3, 'o','r','g',
    0
};

/* Advance past a DNS QNAME; returns pointer to byte after the name, or NULL. */
static const uint8_t *skip_qname(const uint8_t *p, const uint8_t *end) {
    while (p < end) {
        uint8_t len = *p++;
        if (len == 0) return p;
        if ((len & 0xC0) == 0xC0) {   /* compression pointer: two bytes total */
            return (p < end) ? p + 1 : NULL;
        }
        p += len;
    }
    return NULL;
}

void ota_dns_server_thread(void *arg) {
    ota_dns_state_t *s = (ota_dns_state_t *)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { atomic_store(&s->running, false); return; }

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(DNS_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        atomic_store(&s->running, false);
        return;
    }

    atomic_store(&s->running, true);
    printf("[OTA DNS] listening on :%d → badge.why2025.org = %s\n", DNS_PORT, s->ip);

    uint8_t buf[512];
    uint8_t resp[512];

    while (!atomic_load(&s->stop_requested)) {
        struct sockaddr_in cli;
        socklen_t          cli_len = sizeof(cli);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&cli, &cli_len);
        if (n < 12) continue;   /* timeout or malformed */

        /* Only handle standard queries (QR=0, OPCODE=0000) */
        if ((buf[2] & 0xF8) != 0x00) continue;

        const uint8_t *qname_start = buf + 12;
        const uint8_t *end         = buf + n;
        const uint8_t *after_qname = skip_qname(qname_start, end);
        if (!after_qname || after_qname + 4 > end) continue;

        uint16_t qtype        = (uint16_t)((after_qname[0] << 8) | after_qname[1]);
        size_t   qname_len    = (size_t)(after_qname - qname_start);
        size_t   qsection_len = (size_t)(after_qname + 4 - qname_start);

        bool matches = (qname_len == sizeof(TARGET_NAME) &&
                        memcmp(qname_start, TARGET_NAME, sizeof(TARGET_NAME)) == 0);

        /* Build response: copy header, set QR+AA, copy question, optionally add answer */
        memcpy(resp, buf, 12);
        resp[2] = 0x84;   /* QR=1, AA=1, OPCODE=0 */
        resp[3] = 0x00;   /* RA=0, RCODE=0 */
        /* QDCOUNT (resp[4:5]) stays as-is from the query */
        resp[6] = 0x00; resp[7] = 0x00;   /* ANCOUNT = 0 by default */
        resp[8] = 0x00; resp[9] = 0x00;   /* NSCOUNT = 0 */
        resp[10] = 0x00; resp[11] = 0x00; /* ARCOUNT = 0 */

        size_t resp_len = 12;
        memcpy(resp + resp_len, qname_start, qsection_len);
        resp_len += qsection_len;

        if (matches && qtype == 1 /* A */) {
            resp[6] = 0x00; resp[7] = 0x01;   /* ANCOUNT = 1 */

            /* Name: pointer back to offset 12 (the question's QNAME) */
            resp[resp_len++] = 0xC0;
            resp[resp_len++] = 0x0C;
            resp[resp_len++] = 0x00; resp[resp_len++] = 0x01; /* TYPE A */
            resp[resp_len++] = 0x00; resp[resp_len++] = 0x01; /* CLASS IN */
            /* TTL = 60 s */
            resp[resp_len++] = 0x00; resp[resp_len++] = 0x00;
            resp[resp_len++] = 0x00; resp[resp_len++] = 0x3C;
            /* RDLENGTH = 4 */
            resp[resp_len++] = 0x00; resp[resp_len++] = 0x04;
            /* RDATA: IPv4 in network order */
            struct in_addr ip_addr;
            inet_aton(s->ip, &ip_addr);
            uint32_t ip_n = ip_addr.s_addr;
            resp[resp_len++] = (ip_n >>  0) & 0xFF;
            resp[resp_len++] = (ip_n >>  8) & 0xFF;
            resp[resp_len++] = (ip_n >> 16) & 0xFF;
            resp[resp_len++] = (ip_n >> 24) & 0xFF;

            atomic_fetch_add(&s->queries_answered, 1);
        } else if (!matches) {
            resp[3] = 0x03; /* RCODE = NXDOMAIN */
        }
        /* For matching AAAA/other types: NOERROR with empty answer section */

        sendto(sock, resp, resp_len, 0, (struct sockaddr *)&cli, cli_len);
    }

    close(sock);
    atomic_store(&s->running, false);
    printf("[OTA DNS] stopped\n");
}
