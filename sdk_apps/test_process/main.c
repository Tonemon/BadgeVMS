#include "badgevms/misc_funcs.h"
#include "badgevms/process.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>

int main(int argc, char *argv[]) {
    int children = 2;
    printf("Spawning process1\n");
    pid_t process1 = process_create("FLASH0:test_framebuffer.elf", 4096, 0, NULL);
    printf("Got child %u\n", process1);
    printf("Spawning process2\n");
    pid_t process2 = process_create("FLASH0:test_framebuffer.elf", 4096, 0, NULL);
    printf("Got child %u\n", process2);

    while (children) {
        printf("Waiting on children...\n");
        pid_t c = wait(false, 1000);
        if (c != -1) {
            printf("Child %u ended\n", c);
            --children;
        }
    }

    printf("All child processes ended\n");
}
