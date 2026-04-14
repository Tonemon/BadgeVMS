#include <stdio.h>
#include <stdlib.h>

#include <sys/time.h>
#include <unistd.h>

#define NUM_SWITCHES 100000
int volatile counter = 0;

int main(int argc, char *argv[]) {
    struct timeval start, end;

    for (int i = 0; i < 32; ++i) {
        malloc(512);
        usleep(1);
    }

    gettimeofday(&start, NULL);

    while (counter < NUM_SWITCHES) {
        counter++;
        usleep(1);
    }

    gettimeofday(&end, NULL);

    long microseconds = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);

    printf("Context Switch Benchmark Results:\n");
    printf("Total switches: %d\n", NUM_SWITCHES);
    printf("Total time: %ld microseconds\n", microseconds);
    printf("Average per switch: %ld microseconds\n", microseconds / NUM_SWITCHES);
    printf("Switches per second: %ld\n", (long)(NUM_SWITCHES * 1000000.0 / microseconds));
}
