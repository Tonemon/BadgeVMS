/* This file is part of BadgeVMS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "memory.h"

#include "esp_cache.h"
#include "esp_log.h"
#include "esp_mmu_map.h"
#include "esp_psram.h"
#include "freertos/portmacro.h"
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#include "hal/cache_types.h"
#include "hal/mmu_hal.h"
#include "hal/mmu_ll.h"
#include "hal/mmu_types.h"
#include "soc/ext_mem_defs.h"
#include "soc/soc.h"
#include "task.h"
#include "thirdparty/dlmalloc.h"
#include "wrapped_funcs.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include <errno.h>

typedef struct {
    uint32_t         start;   // laddr start
    uint32_t         end;     // laddr end
    size_t           size;    // region size
    cache_bus_mask_t bus_id;  // bus_id mask, for accessible cache buses
    mmu_target_t     targets; // region supported physical targets
    uint32_t         caps;    // vaddr capabilities
} mmu_mem_region_t;

extern mmu_mem_region_t const g_mmu_mem_regions[SOC_MMU_LINEAR_ADDRESS_REGION_NUM];

extern void spi_flash_enable_interrupts_caches_and_other_cpu(void);
extern void spi_flash_disable_interrupts_caches_and_other_cpu(void);

extern void                     init_memory_heap_caps();
static char const              *TAG                 = "memory";
IRAM_ATTR static volatile pid_t current_mapped_task = 0;
static allocator_t              page_allocator;
static allocator_t              framebuffer_allocator;

IRAM_ATTR static portMUX_TYPE cache_mmu_mutex = portMUX_INITIALIZER_UNLOCKED;

// Copied from ESP-IDF 5.4.2 for speed
__attribute__((always_inline)) static inline uint32_t why_mmu_hal_get_id_from_target(mmu_target_t target) {
    return (target == MMU_TARGET_FLASH0) ? MMU_LL_FLASH_MMU_ID : MMU_LL_PSRAM_MMU_ID;
}

IRAM_ATTR void dump_mmu() {
    uint32_t mmu_id = mmu_hal_get_id_from_target(MMU_TARGET_PSRAM0);

    for (pid_t pid = 0; pid < MAX_PID; ++pid) {
        task_info_t *task_info = get_taskinfo_for_pid(pid);
        if (!task_info)
            continue;

        esp_rom_printf("\n*********** PID %02u MAP   ***********\n", task_info->pid);
        allocation_range_t *r = task_info->thread->pages;
        while (r) {
            for (int i = 0; i < r->size / SOC_MMU_PAGE_SIZE; ++i) {
                esp_rom_printf(
                    "vaddr 0x%08lx -> paddr 0x%08lx\n",
                    r->vaddr_start + (i * SOC_MMU_PAGE_SIZE),
                    r->paddr_start + (i * SOC_MMU_PAGE_SIZE)
                );
            }
            r = r->next;
        }
    }

    esp_rom_printf("\n*********** MMU DUMP START ***********\n");
    for (int i = 0; i < SOC_MMU_ENTRY_NUM; i++) {
        uint32_t entry = mmu_ll_read_entry(mmu_id, i);
        if (entry) {
            esp_rom_printf(
                "entry: %03lu: vaddr 0x%08lx -> paddr 0x%08lx\n",
                i,
                SOC_EXTRAM_LOW + (i * SOC_MMU_PAGE_SIZE),
                (entry & ~0xC00) << 16
            );
        }
    }
    esp_rom_printf("*********** MMU DUMP END   ***********\n");
}

static bool volatile scheduler_was_started = false;
__attribute__((always_inline)) static inline void critical_enter() {
    portENTER_CRITICAL_SAFE(&cache_mmu_mutex);
}

__attribute__((always_inline)) static inline void critical_exit() {
    portEXIT_CRITICAL_SAFE(&cache_mmu_mutex);
}

// Copied from ESP-IDF 5.4.2 for speed
__attribute__((always_inline)) static inline bool
    why_mmu_ll_check_valid_paddr_region(uint32_t mmu_id, uint32_t paddr_start, uint32_t len) {
    int max_paddr_page_num = 0;
    if (mmu_id == MMU_LL_FLASH_MMU_ID) {
        max_paddr_page_num = SOC_MMU_FLASH_MAX_PADDR_PAGE_NUM;
    } else if (mmu_id == MMU_LL_PSRAM_MMU_ID) {
        max_paddr_page_num = SOC_MMU_PSRAM_MAX_PADDR_PAGE_NUM;
    } else {
        HAL_ASSERT(false);
    }

    return (paddr_start < (mmu_ll_get_page_size(mmu_id) * max_paddr_page_num)) &&
           (len < (mmu_ll_get_page_size(mmu_id) * max_paddr_page_num)) &&
           ((paddr_start + len - 1) < (mmu_ll_get_page_size(mmu_id) * max_paddr_page_num));
}

// Copied from ESP-IDF 5.4.2 for speed
__attribute__((always_inline)) static inline uint32_t why_mmu_hal_pages_to_bytes(uint32_t mmu_id, uint32_t page_num) {
    mmu_page_size_t page_size  = mmu_ll_get_page_size(mmu_id);
    uint32_t        shift_code = 0;
    switch (page_size) {
        case MMU_PAGE_64KB: shift_code = 16; break;
        case MMU_PAGE_32KB: shift_code = 15; break;
        case MMU_PAGE_16KB: shift_code = 14; break;
        default: HAL_ASSERT(shift_code);
    }
    return page_num << shift_code;
}

__attribute__((always_inline)) static inline bool
    why_mmu_hal_check_valid_ext_vaddr_region(uint32_t mmu_id, uint32_t vaddr_start, uint32_t len, mmu_vaddr_t type) {
    return mmu_ll_check_valid_ext_vaddr_region(mmu_id, vaddr_start, len, type);
}

// Copied from ESP-IDF 5.4.2 for speed
__attribute__((always_inline)) static inline void
    why_mmu_hal_map_region(uint32_t mmu_id, mmu_target_t mem_type, uint32_t vaddr, uint32_t paddr, uint32_t len) {
    uint32_t page_size_in_bytes = why_mmu_hal_pages_to_bytes(mmu_id, 1);
    // HAL_ASSERT(vaddr % page_size_in_bytes == 0);
    // HAL_ASSERT(paddr % page_size_in_bytes == 0);
    // HAL_ASSERT(mmu_ll_check_valid_paddr_region(mmu_id, paddr, len));
    // HAL_ASSERT(mmu_hal_check_valid_ext_vaddr_region(mmu_id, vaddr, len, MMU_VADDR_DATA | MMU_VADDR_INSTRUCTION));

    uint32_t page_num = (len + page_size_in_bytes - 1) / page_size_in_bytes;
    uint32_t entry_id = 0;
    uint32_t mmu_val; // This is the physical address in the format that MMU supported

    mmu_val = mmu_ll_format_paddr(mmu_id, paddr, mem_type);

    while (page_num) {
        entry_id       = mmu_ll_get_entry_id(mmu_id, vaddr);
        uint32_t entry = mmu_ll_read_entry(mmu_id, entry_id);
        if (entry) {
            esp_rom_printf(
                "Entry %u (0x%08lX) was already mapped, currently mapped page 0x%08x, wanted 0x%08x\n",
                entry_id,
                vaddr,
                (entry & ~0xC00) << 16,
                paddr
            );
            esp_system_abort("Unexpected mmu state");
        }
        mmu_ll_write_entry(mmu_id, entry_id, mmu_val, mem_type);

        vaddr += page_size_in_bytes;
        mmu_val++;
        page_num--;
    }
}

__attribute__((always_inline)) static inline void invalidate_caches(uintptr_t vaddr_start, uint32_t len) {
    esp_cache_msync((void *)vaddr_start, len, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    esp_cache_msync((void *)vaddr_start, len, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
}

__attribute__((always_inline)) static inline void writeback_caches(uintptr_t vaddr_start, uint32_t len) {
    esp_cache_msync((void *)vaddr_start, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
}

__attribute__((always_inline)) static inline void
    why_mmu_hal_unmap_region(uint32_t mmu_id, uint32_t vaddr, uint32_t len) {
    uint32_t page_size_in_bytes = why_mmu_hal_pages_to_bytes(mmu_id, 1); // Already inline
    // HAL_ASSERT(vaddr % page_size_in_bytes == 0);
    // HAL_ASSERT(mmu_hal_check_valid_ext_vaddr_region(mmu_id, vaddr, len, MMU_VADDR_DATA | MMU_VADDR_INSTRUCTION));

    uint32_t page_num = (len + page_size_in_bytes - 1) / page_size_in_bytes;
    uint32_t entry_id = 0;
    while (page_num) {
        entry_id       = mmu_ll_get_entry_id(mmu_id, vaddr);
        uint32_t entry = mmu_ll_read_entry(mmu_id, entry_id);
        if (!entry) {
            esp_rom_printf("Entry %u (0x%08lx) was not mapped\n", entry_id, vaddr);
            esp_system_abort("Unexpected mmu state");
        }
        mmu_ll_set_entry_invalid(mmu_id, entry_id);

        vaddr += page_size_in_bytes;
        page_num--;
    }
}

__attribute__((always_inline)) static inline void
    map_regions(allocation_range_t *head_range, allocation_range_t *tail_range) {
    uint32_t            mmu_id     = mmu_hal_get_id_from_target(MMU_TARGET_PSRAM0);
    uintptr_t           start      = tail_range->vaddr_start;
    uintptr_t           total_size = 0;
    allocation_range_t *r          = head_range;
    if (!r)
        return;

    while (r) {
        ESP_DRAM_LOGV(
            DRAM_STR("map_regions"),
            "Mapping region ptr %p, vaddr_start: %p, paddr_start %p, size %u, r->next = %p",
            r,
            (void *)r->vaddr_start,
            (void *)r->paddr_start,
            r->size,
            r->next
        );

        why_mmu_hal_map_region(mmu_id, MMU_TARGET_PSRAM0, r->vaddr_start, r->paddr_start, r->size);
        total_size += r->size;
        r           = r->next;
    }

    // Invalidate all caches at once
    invalidate_caches(start, total_size);
}

IRAM_ATTR void remap_task(task_info_t *task_info) {
    if (current_mapped_task) {
        ESP_DRAM_LOGE(DRAM_STR("map_task"), "Expected task %u but actual current task is %u", 0, current_mapped_task);
        esp_system_abort("Task info does not match");
    }

    uint32_t mmu_id = mmu_hal_get_id_from_target(MMU_TARGET_PSRAM0);

    critical_enter();
    allocation_range_t *r = task_info->thread->pages;
    while (r) {
        why_mmu_hal_map_region(mmu_id, MMU_TARGET_PSRAM0, r->vaddr_start, r->paddr_start, r->size);
        r = r->next;
    }

    // Invalidate all caches at once
    invalidate_caches(task_info->thread->start, task_info->thread->size);
    current_mapped_task = task_info->pid;
    critical_exit();
}

void IRAM_ATTR unmap_task(task_info_t *task_info) {
    if (current_mapped_task != task_info->pid) {
        ESP_DRAM_LOGE(
            DRAM_STR("unmap_task"),
            "Expected task %u but actual current task is %u",
            task_info->pid,
            current_mapped_task
        );
        esp_system_abort("Task info does not match");
    }

    // esp_rom_printf("Unmappingg %u\n", task_info->pid);
    allocation_range_t *r = task_info->thread->pages;
    if (!r) {
        // Nothing to do, whatever is in ram is still in ram
        goto out;
    }

    uint32_t mmu_id = why_mmu_hal_get_id_from_target(MMU_TARGET_PSRAM0);

    critical_enter();
    writeback_caches(task_info->thread->start, task_info->thread->size);

    while (r) {
        why_mmu_hal_unmap_region(mmu_id, r->vaddr_start, r->size);
        r = r->next;
    }
    critical_exit();
out:
    current_mapped_task = 0;
}

IRAM_ATTR void pages_deallocate(allocation_range_t *head_range) {
    allocation_range_t *r = head_range;
    while (r) {
        ESP_LOGI(
            TAG,
            "Deallocating page. vaddr_start = %p, paddr_start = %p, size = %zi",
            (void *)r->vaddr_start,
            (void *)r->paddr_start,
            r->size
        );
        page_deallocate(r->paddr_start);
        allocation_range_t *n = r->next;
        free(r);
        r = n;
    }
}

IRAM_ATTR bool pages_allocate(
    uintptr_t vaddr_start, uintptr_t pages, allocation_range_t **head_range, allocation_range_t **tail_range
) {
    *head_range = NULL;
    *tail_range = NULL;

    if (pages > buddy_get_free_pages(&page_allocator)) {
        return false;
    }

    uint32_t to_allocate   = pages;
    uint32_t allocate_size = pages;

    // Since we don't know how much contiguous free pages are available we will
    // have to build up a list of pages to allocate. We want to keep the section with
    // interrupts disabled as short as possible so do all of the work beforehand.
    //
    // We want to have as few mappings as possible, so just allocating 1 at a time is
    // not an option
    while (to_allocate) {
        uintptr_t new_page = 0;
        // allocate_size      = allocate_size > to_allocate ? to_allocate : allocate_size;
        allocate_size      = 1;
        ESP_LOGI(TAG, "Attempting allocation of size %li pages\n", allocate_size);

        allocation_range_t *new_range = malloc(sizeof(allocation_range_t));
        if (new_range) {
            new_page = page_allocate(allocate_size * SOC_MMU_PAGE_SIZE);
        } else {
            // bail
            ESP_LOGE(TAG, "Failed to allocate range structure");
            allocate_size = 1;
        }

        if (!new_page) {
            free(new_range);

            if (allocate_size == 1) {
                // No more memory
                allocation_range_t *r = *head_range;
                while (r) {
                    allocation_range_t *n = r->next;
                    page_deallocate(r->paddr_start);
                    free(r);
                    r = n;
                }
                *head_range = NULL;
                *tail_range = NULL;
                return false;
            }
            // Try again
            allocate_size -= 1;
            continue;
        }

        ESP_LOGI(TAG, "Got new page at address %p", (void *)new_page);
        // We are the first allocation
        if (!*tail_range) {
            *tail_range = new_range;
        }
        new_range->vaddr_start = vaddr_start;
        // The allocator doesn't know the physical ranges
        new_range->paddr_start = new_page;
        new_range->size        = allocate_size * SOC_MMU_PAGE_SIZE;
        new_range->next        = *head_range;

        // We are the new head
        *head_range  = new_range;
        // Next allocation will be immediately after us in vaddr
        vaddr_start += allocate_size * SOC_MMU_PAGE_SIZE;
        to_allocate -= allocate_size;

        ESP_LOGI(
            TAG,
            "New range: vaddr_start = %p, paddr_start = %p, size = %zi",
            (void *)new_range->vaddr_start,
            (void *)new_range->paddr_start,
            new_range->size
        );
    }

    return true;
}

uintptr_t IRAM_ATTR framebuffer_vaddr_allocate(size_t size, size_t *out_pages) {
    size_t aligned_size = ((size + (SOC_MMU_PAGE_SIZE - 1)) & ~(SOC_MMU_PAGE_SIZE - 1));
    void  *ret          = buddy_allocate(&framebuffer_allocator, aligned_size, 0, 0);
    if (ret) {
        *out_pages = aligned_size / SOC_MMU_PAGE_SIZE;
        return (uintptr_t)ret;
    }

    *out_pages = 0;
    return 0;
}

void IRAM_ATTR framebuffer_vaddr_deallocate(uintptr_t start_address) {
    if (start_address) {
        buddy_deallocate(&framebuffer_allocator, (void *)start_address);
    }
}

void IRAM_ATTR framebuffer_map_pages(allocation_range_t *head_range, allocation_range_t *tail_range) {
    critical_enter();
    map_regions(head_range, tail_range);
    critical_exit();
}

void IRAM_ATTR framebuffer_unmap_pages(allocation_range_t *head_range) {
    critical_enter();
    allocation_range_t *r      = head_range;
    uint32_t            mmu_id = why_mmu_hal_get_id_from_target(MMU_TARGET_PSRAM0);

    while (r) {
        why_mmu_hal_unmap_region(mmu_id, r->vaddr_start, r->size);
        r = r->next;
    }
    critical_exit();
}

void IRAM_ATTR NOINLINE_ATTR *why_sbrk(intptr_t increment) {
    task_info_t *task_info = get_task_info();
    uintptr_t    old       = task_info->thread->end;
    ESP_LOGI("sbrk", "Calling sbrk(%zi) from task %d", increment, task_info->pid);

    if (!increment)
        goto out;

    if (increment > 0) {
        // Allocating new pages
        uintptr_t vaddr_start = task_info->thread->end;

        if (vaddr_start + increment > SOC_EXTRAM_HIGH) {
            goto error;
        }

        uint32_t pages = increment / SOC_MMU_PAGE_SIZE;

        // Ranges are in reverse order, when we insert our new ranges into
        // the task_info this range needs to be tied to the old head
        allocation_range_t *head_range = NULL;
        allocation_range_t *tail_range = NULL;

        if (!pages_allocate(vaddr_start, pages, &head_range, &tail_range)) {
            goto error;
        }

        // Actually map our new memory
        ESP_LOGI(
            TAG,
            "Allocating new ranges for pid %i, head: %p, tail %p, allocations %p",
            task_info->pid,
            head_range,
            tail_range,
            task_info->thread->pages
        );

        // Map our new page table entries in one atomic operation
        critical_enter();
        {
            map_regions(head_range, tail_range);

            tail_range->next         = task_info->thread->pages;
            task_info->thread->pages = head_range;

            task_info->thread->size += increment;
            task_info->thread->end  += increment;
        }
        critical_exit();
    } else {
        // increment is negative; free exactly |increment| bytes, not (size - |increment|)
        int32_t  decrement_amount = (int32_t)(-increment);
        int32_t  to_decrement     = decrement_amount;
        uint32_t mmu_id           = why_mmu_hal_get_id_from_target(MMU_TARGET_PSRAM0);

        allocation_range_t *r = task_info->thread->pages;
        while (r && to_decrement) {
            // We always free from the end
            if (r->size <= to_decrement) {
                // Current block is smaller than we want to decrement
                ESP_LOGI(
                    TAG,
                    "Deallocating whole page. vaddr_start = %p, paddr_start = %p, size = %zi",
                    (void *)r->vaddr_start,
                    (void *)r->paddr_start,
                    r->size
                );
                // Don't try to deallocate a page with caches disabled
                page_deallocate(r->paddr_start);
                allocation_range_t *n = r->next;

                // Unmap and change the page table entries in one atomic operation
                critical_enter();
                {
                    why_mmu_hal_unmap_region(mmu_id, r->vaddr_start, r->size);
                    task_info->thread->pages = n;
                }
                critical_exit();

                to_decrement -= r->size;
                free(r);
                r = n;
            } else {
                // Current block is larger than we want to decrement
                ESP_LOGI(
                    TAG,
                    "Deallocating partial. vaddr_start = %p, paddr_start = %p, size = %zi, removing %li",
                    (void *)r->vaddr_start,
                    (void *)r->paddr_start,
                    r->size,
                    to_decrement
                );

                // Write back, unmap, remap, and invalidate in one atomic operation
                critical_enter();
                {
                    writeback_caches(r->vaddr_start, r->size - to_decrement);
                    why_mmu_hal_unmap_region(mmu_id, r->vaddr_start, r->size);
                    r->size -= to_decrement;
                    why_mmu_hal_map_region(mmu_id, MMU_TARGET_PSRAM0, r->vaddr_start, r->paddr_start, r->size);
                    invalidate_caches(r->vaddr_start, r->size);
                }
                critical_exit();

                page_deallocate(r->paddr_start + to_decrement);
                to_decrement = 0;
            }
        }
        task_info->thread->size -= decrement_amount;
        task_info->thread->end  -= decrement_amount;
    }

out:
    ESP_LOGI(
        "sbrk",
        "Calling sbrk(%zi) from task %d, returning %p, thread->size=%zi, thread->end=%p",
        increment,
        task_info->pid,
        (void *)old,
        task_info->thread->size,
        (void *)task_info->thread->end
    );
    return (void *)old;

error:
    ESP_LOGW(TAG, "Out of memory for task %i", task_info->pid);
    task_info->_errno = ENOMEM;
    return (void *)-1;
}

void page_deallocate(uintptr_t paddr_start) {
    buddy_deallocate(&page_allocator, (void *)PADDR_TO_ADDR(paddr_start));
}

uintptr_t page_allocate(size_t size) {
    void *ret = buddy_allocate(&page_allocator, size, 0, 0);
    if (ret) {
        return ADDR_TO_PADDR((uintptr_t)ret);
    }
    // This is fine because page 0 is always used by the allocator itself
    return 0;
}

#define BAD_PAGES_MAX 10
static uintptr_t bad_pages[BAD_PAGES_MAX];
static int       bad_pages_num = 0;

void reserve_bad_pages(size_t psram_size) {
    size_t     total_pages = buddy_get_free_pages(&page_allocator);
    uintptr_t *pages       = calloc(1, total_pages * sizeof(uintptr_t));
    if (!pages) {
        ESP_LOGE("memory_init", "Couldn't allocate memory for free page pointers");
        return;
    }
    // Allocate all free pages
    for (int i = 0; i < total_pages - 1; ++i) {
        pages[i] = page_allocate(SOC_MMU_PAGE_SIZE);
    }

    for (int i = 0; i < total_pages - 1; ++i) {
        bool found = false;

        for (int k = 0; k < bad_pages_num; ++k) {
            if (bad_pages[k] == i * SOC_MMU_PAGE_SIZE) {
                found = true;
                break;
            }
        }

        if (found) {
            ESP_LOGW("memory_init", "Reserving bad page 0x%08X", i * SOC_MMU_PAGE_SIZE);
        } else {
            page_deallocate(pages[i]);
        }
    }
    free(pages);
}

static IRAM_ATTR bool test_psram(intptr_t v_start, size_t size) {
    int volatile *spiram = (int volatile *)v_start;
    size_t        p;
    int           errct       = 0;
    int           initial_err = -1;

    ESP_DRAM_LOGW(DRAM_STR("memory_test"), "Starting write");
    for (p = 0; p < (size / sizeof(int)); p += 8) {
        spiram[p] = p ^ 0xAAAAAAAA;
    }

    ESP_DRAM_LOGW(DRAM_STR("memory_test"), "Writing back pages");
    writeback_caches(v_start, size);
    ESP_DRAM_LOGW(DRAM_STR("memory_test"), "Invalidating pages");
    invalidate_caches(v_start, size);

    ESP_DRAM_LOGW(DRAM_STR("memory_test"), "Starting read");
    for (p = 0; p < (size / sizeof(int)); p += 8) {
        if (spiram[p] != (p ^ 0xAAAAAAAA)) {
            uintptr_t address = VADDR_START + (p * 4);
            uintptr_t page    = ((address - VADDR_START) & ~(SOC_MMU_PAGE_SIZE - 1));
            bool      found   = false;

            spi_flash_enable_interrupts_caches_and_other_cpu();
            ESP_LOGE("memory_test", "Bad pages detected, rebooting");
            esp_restart();

            for (int k = 0; k < 8; ++k) {
                if (bad_pages[k] == page) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (bad_pages_num == BAD_PAGES_MAX) {
                    ESP_DRAM_LOGE(DRAM_STR("memory_test"), "Too many bad pages");
                    // esp_system_abort("Too many bad pages in memory test");
                    spi_flash_enable_interrupts_caches_and_other_cpu();
                    esp_restart();
                }
                ESP_DRAM_LOGE(
                    DRAM_STR("memory_test"),
                    "Addres 0x%08lx page 0x%08lx failed, expected 0x%08lx, got 0x%08lx",
                    address,
                    page,
                    p ^ 0xAAAAAAAA,
                    spiram[p]
                );
                bad_pages[bad_pages_num++] = page;
            }
            errct++;
            if (errct == 1) {
                initial_err = p * 4;
            }
        }
    }
    if (errct) {
        ESP_DRAM_LOGE(
            DRAM_STR("memory_test"),
            "SPI SRAM memory test fail. %d/%d writes failed, first @ %X",
            errct,
            size / 32,
            initial_err + v_start
        );
        return false;
    } else {
        ESP_DRAM_LOGI(DRAM_STR("memory_test"), "SPI SRAM memory test OK");
        return true;
    }
}

uint32_t vaddr_to_paddr(uint32_t vaddr) {
    uint32_t mmu_id = why_mmu_hal_get_id_from_target(MMU_TARGET_PSRAM0);

    uint32_t     paddr;
    mmu_target_t target;
    mmu_hal_vaddr_to_paddr(mmu_id, vaddr, &paddr, &target);

    return paddr;
}

size_t get_free_psram_pages() {
    return buddy_get_free_pages(&page_allocator);
}

size_t get_total_psram_pages() {
    return buddy_get_total_pages(&page_allocator);
}

size_t get_free_framebuffer_pages() {
    return buddy_get_free_pages(&framebuffer_allocator);
}

size_t get_total_framebuffer_pages() {
    return buddy_get_total_pages(&framebuffer_allocator);
}

void writeback_and_invalidate_task(task_info_t *task_info) {
    critical_enter();
    {
        writeback_caches(task_info->thread->start, task_info->thread->size);
        invalidate_caches(task_info->thread->start, task_info->thread->size);
    }
    critical_exit();
}

void IRAM_ATTR memory_init() {
    for (int i = 0; i < SOC_MMU_LINEAR_ADDRESS_REGION_NUM; i++) {
        ESP_LOGI(TAG, "MMU Region  %u", i);
        ESP_LOGI(TAG, "  start   : %p", (void *)g_mmu_mem_regions[i].start);
        ESP_LOGI(TAG, "  size    : %u", g_mmu_mem_regions[i].size);
        ESP_LOGI(TAG, "  targets : %u", g_mmu_mem_regions[i].targets);
    }

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Initializing PSRAM");
    esp_psram_init();

    size_t psram_size = esp_psram_get_size();

    uint32_t mmu_id = why_mmu_hal_get_id_from_target(MMU_TARGET_PSRAM0);
    uint32_t out_len;

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Disabling caches and interrupts");
    spi_flash_disable_interrupts_caches_and_other_cpu();

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Unmapping all of our address space");
    mmu_ll_unmap_all(mmu_id);

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Mapping all pages for memory test");
    mmu_hal_map_region(mmu_id, MMU_TARGET_PSRAM0, VADDR_START, 0x0, psram_size, &out_len);

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Invalidate all pages for memory test");
    invalidate_caches(VADDR_START, out_len);

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Running memory test");
    if (!test_psram(VADDR_START, out_len)) {
        ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Found bad pages");
    }

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Unmapping all of our address space");
    mmu_ll_unmap_all(mmu_id);

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Map allocator address space");
    mmu_hal_map_region(mmu_id, MMU_TARGET_PSRAM0, VADDR_START, 0, SOC_MMU_PAGE_SIZE, &out_len);

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Invalidate allocator address space");
    invalidate_caches(VADDR_START, out_len);

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Re-enabling caches and interrupts");
    spi_flash_enable_interrupts_caches_and_other_cpu();

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Initialzing memory pool");
    init_pool(&page_allocator, (void *)VADDR_START, (void *)VADDR_START + psram_size, 0);

    if (bad_pages_num) {
        reserve_bad_pages(psram_size);
    }

    uintptr_t framebuffer_page = page_allocate(SOC_MMU_PAGE_SIZE);

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Disabling caches and interrupts");
    spi_flash_disable_interrupts_caches_and_other_cpu();

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Map framebuffer allocator address space");
    mmu_hal_map_region(
        mmu_id,
        MMU_TARGET_PSRAM0,
        FRAMEBUFFER_HEAP_START,
        framebuffer_page,
        SOC_MMU_PAGE_SIZE,
        &out_len
    );

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Invalidate allocator address space");
    invalidate_caches(FRAMEBUFFER_HEAP_START, out_len);

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Re-enabling caches and interrupts");
    spi_flash_enable_interrupts_caches_and_other_cpu();

    ESP_DRAM_LOGW(DRAM_STR("memory_init"), "Initialzing framebuffer pool");
    init_pool(
        &framebuffer_allocator,
        (void *)FRAMEBUFFER_HEAP_START,
        (void *)FRAMEBUFFER_HEAP_START + FRAMEBUFFER_HEAP_SIZE,
        0
    );

    init_memory_heap_caps();
    wrapped_functions_init();
    print_allocator(&page_allocator);
}
