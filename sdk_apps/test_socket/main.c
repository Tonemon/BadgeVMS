#include <stdio.h>
#include <stdlib.h>

#include <arpa/inet.h>
#include <badgevms/wifi.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define HOST "192.168.4.1"
#define PORT 9191

int tcp_client_test() {
    int                sockfd, connfd;
    struct sockaddr_in servaddr, cli;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        printf("socket creation failed...\n");
        exit(0);
    } else
        printf("Socket successfully created: %d\n", sockfd);
    bzero(&servaddr, sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    inet_aton(HOST, &servaddr.sin_addr);
    servaddr.sin_port = htons(PORT);

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) != 0) {
        printf("connection with the server failed...\n");
        close(sockfd);
        return -1;
    } else {
        printf("connected to the server..\n");
    }

    write(sockfd, "Hello, server!", 15);

    while (1) {
        char buffer[1024];

        ssize_t n = read(sockfd, buffer, sizeof(buffer) - 1);
        if (n <= 0) {
            printf("Connection closed or error occurred\n");
            break;
        }
        buffer[n] = '\0';
        printf("Received: %s\n", buffer);

        // echo
        if (write(sockfd, buffer, n) < 0) {
            printf("Error writing to socket\n");
            break;
        }

        // exit?
        if (strncmp(buffer, "exit", 4) == 0) {
            printf("Exiting...\n");
            break;
        }
    }

    close(sockfd);
}


int tcp_server_test() {
    int                sockfd, connfd;
    struct sockaddr_in servaddr, cli;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        printf("socket creation failed...\n");
        exit(0);
    } else
        printf("Socket successfully created: %d\n", sockfd);
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = 0; // INADDR_ANY
    servaddr.sin_port        = htons(PORT);

    // Bind the socket to the address and port
    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) != 0) {
        printf("socket bind failed...\n");
        close(sockfd);
        return -1;
    } else
        printf("Socket successfully binded..\n");

    // Now server is ready to listen and verification
    if (listen(sockfd, 5) != 0) {
        printf("Listen failed...\n");
        close(sockfd);
        return -1;
    } else
        printf("Server listening..\n");

    socklen_t len = sizeof(cli);
    while (1) {
        connfd = accept(sockfd, (struct sockaddr *)&cli, &len);
        if (connfd < 0) {
            printf("server accept failed...\n");
            close(sockfd);
            return -1;
        }

        printf("Client connected: %s:%d\n", inet_ntoa(cli.sin_addr), ntohs(cli.sin_port));

        char buff[1024];
        while (1) {
            bzero(buff, sizeof(buff));
            size_t nread = read(connfd, buff, sizeof(buff) - 1);
            if (nread <= 0) {
                printf("Client disconnected or read error\n");
                break;
            }
            buff[nread] = '\0';
            printf("From client: %s", buff);

            if (strncmp(buff, "exit", 4) == 0) {
                printf("Client requested exit\n");
                break;
            }

            write(connfd, buff, nread);
        }

        close(connfd);
    }
    close(sockfd);
    return 0;
}

int main(int argc, char *argv[]) {
    wifi_connect();
    printf("Connected to WiFi\n");
    printf("Starting TCP client test...\n");
    tcp_client_test();
    printf("Starting TCP server test...\n");
    tcp_server_test();
    return 0;
}
