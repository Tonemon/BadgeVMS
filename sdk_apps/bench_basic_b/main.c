#include "badgevms/misc_funcs.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define NUM_ITERATIONS 100
#define ALLOC_SIZE     4096 * 10

static inline bool is_single_bit_flip(char a, char b) {
    char diff = a ^ b;

    // Check if diff is a power of 2 (exactly one bit set)
    return (diff & (diff - 1)) == 0;
}

int main(int argc, char *argv[]) {
    struct timeval start, end;
    gettimeofday(&start, NULL);
    char task_id = (char)getpid();

    char *ptrs[100] = {0};

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        // Allocate some memory
        char *ptr = malloc(ALLOC_SIZE);
        // if (task_id == 2) {
        //     *((char*)((void*)0x4c100000)) = '0';
        //}
        if (ptr) {
            memset(ptr, task_id, ALLOC_SIZE);
            ptrs[i % 100] = ptr;

            // Free older allocations to avoid OOM
            if (i > 50 && ptrs[(i - 50) % 100]) {
                if (ptrs[(i - 50) % 100]) {
                    for (int k = 0; k < ALLOC_SIZE; ++k) {
                        char volatile t;
                        // Read all memory back
                        t = ptrs[(i - 50) % 100][k];

                        if (t != task_id) {
                            uintptr_t vaddr = (uintptr_t)(&ptrs[(i - 50) % 100][k > 20 ? k - 20 : 0]);
                            if (is_single_bit_flip(t, task_id)) {
                                printf(
                                    "\033[0;31mSingle bit flip expected 0x%X (%i) got 0x%X (%i) in ptr number "
                                    "%i, vaddr %p, paddr 0x%08lx\033[0m\n",
                                    task_id,
                                    task_id,
                                    t,
                                    t,
                                    (i - 50) % 100,
                                    &ptrs[(i - 50) % 100][k],
                                    vaddr_to_paddr(vaddr)
                                );
                            } else {
                                printf(
                                    "\033[0;31mError unexpected memory contents! expected 0x%X (%i) got 0x%X (%i) in "
                                    "ptr number "
                                    "%i, vaddr %p, paddr 0x%08lx\033[0m\n",
                                    task_id,
                                    task_id,
                                    t,
                                    t,
                                    (i - 50) % 100,
                                    &ptrs[(i - 50) % 100][k],
                                    vaddr_to_paddr(vaddr)
                                );

                                for (int z = 0; z < 3; ++z) {
                                    for (int l = 0; l < 1; ++l) {
                                        printf("0x%08X: ", vaddr + (l * 40));
                                        for (int p = 0; p < 40; ++p) {
                                            printf("%02x ", ((char *)(vaddr))[(l * 40) + p]);
                                        }
                                        printf("\n");
                                    }
                                    usleep(10000);
                                }
                                die("Unexpected memory contents in memory bench");
                                return 1;
                            }
                        }
                        ptrs[(i - 50) % 100][k] = -task_id;
                    }
                    free(ptrs[(i - 50) % 100]);
                    ptrs[(i - 50) % 100] = NULL;
                }
            }
        }

        // Force context switch every few iterations
        if (i % 10 == 0) {
            usleep(1);
        }
    }

    // Clean up remaining allocations
    for (int i = 0; i < 100; i++) {
        if (ptrs[i])
            free(ptrs[i]);
    }

    gettimeofday(&end, NULL);

    long microseconds = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);

    printf("Task %d completed in %ld microseconds\n", task_id, microseconds);
    printf("Task %d: %ld microseconds per iteration\n", task_id, microseconds / NUM_ITERATIONS);
}
