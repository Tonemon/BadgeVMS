#include <stdio.h>
#include <stdlib.h>

#include <arpa/inet.h>
#include <badgevms/wifi.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

int main(int argc, char *argv[]) {
    int num_results = wifi_scan_get_num_results();

    printf("Total APs scanned = %u\n", num_results);
    for (int i = 0; i < num_results; i++) {
        wifi_station_handle s = wifi_scan_get_result(i);
        if (s) {
            printf("AP \t\t%d\n", i);
            printf("SSID \t\t%s\n", wifi_station_get_ssid(s));
            printf("Channel \t\t%d\n", wifi_station_get_primary_channel(s));
            printf("================\n");
            wifi_scan_free_station(s);
        }
    }

    wifi_connect();

    int              sfd, s;
    struct addrinfo  hints;
    struct addrinfo *result, *rp;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family    = AF_INET;
    hints.ai_socktype  = 0;
    hints.ai_flags     = 0;
    hints.ai_protocol  = 0;
    hints.ai_canonname = NULL;
    hints.ai_addr      = NULL;
    hints.ai_next      = NULL;

    printf("Sizeof struct addrinfo %u\n", sizeof(struct addrinfo));

    printf("Calling getaddrinfo\n");

    s = getaddrinfo("why2025.org", "80", &hints, &result);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo:\n");
        exit(1);
    }

    printf("getadadrinfo returned success\n");

    struct sockaddr_in *addr;
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        addr     = (struct sockaddr_in *)rp->ai_addr;
        char *ip = inet_ntoa((struct in_addr)addr->sin_addr);
        printf("Found ip: %s\n", ip);
    }
    freeaddrinfo(result);

    printf("Done scanning!\n");
}
