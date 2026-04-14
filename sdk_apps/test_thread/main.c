#include "badgevms/misc_funcs.h"
#include "badgevms/process.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>

void thread_main(void *data) {
    pid_t pid = getpid();
    printf("Hello, I am thread %u, going to go sleep now\n", pid);

    usleep((pid + 10) * 1000 * 1000);
    atomic_store((atomic_bool *)data, true);

    printf("Bye from thread %u\n", pid);
}

int main(int argc, char *argv[]) {
    atomic_bool t1 = false, t2 = false;
    pid_t       thread1 = thread_create(thread_main, &t1, 4096);
    pid_t       thread2 = thread_create(thread_main, &t2, 4096);

    bool saw_t1 = false, saw_t2 = false;

    while (1) {
        if (atomic_load(&t1)) {
            printf("Thread 1 done!\n");
            saw_t1 = true;
        }
        if (atomic_load(&t2)) {
            printf("Thread 2 done!\n");
            saw_t2 = true;
        }

        if (saw_t1 && saw_t2) {
            printf("All my threads are done!\n");
            break;
        }
        printf(".\n");
        usleep(1000 * 1000);
    }
}
