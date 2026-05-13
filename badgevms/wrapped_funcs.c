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

#include "badgevms/pathfuncs.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "logical_names.h"
#include "lwip/ip4_addr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "rom/uart.h"
#include "task.h"
#include "thirdparty/dlmalloc.h"

#include <stdio.h>
#include <stdlib.h>

#include <dirent.h>
#include <fcntl.h>
#include <iconv.h>
#include <math.h>
#include <regex.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <time.h>
#include <wchar.h>

static char const *TAG = "wrapped_functions";

// Our implementation of dlmalloc is not thread safe.
// Protect the kernel from itself only.
static SemaphoreHandle_t kernel_malloc_lock = NULL;

char *why_environ = NULL;

IRAM_ATTR void why_die(char const *reason) {
    esp_system_abort(reason);
}

char *why_strerror(int errnum) {
    task_info_t *task_info = get_task_info();
    return strerror_r(errnum, task_info->strerror_buf, STRERROR_BUFLEN);
}

char *why_strtok(char *str, char const *delim) {
    task_info_t *task_info = get_task_info();
    return strtok_r(str, delim, &task_info->strtok_saveptr);
}

char *why_asctime(const struct tm *tm) {
    task_info_t *task_info = get_task_info();
    return asctime_r(tm, task_info->asctime_buf);
}

char *why_ctime(time_t const *timep) {
    task_info_t *task_info = get_task_info();
    return ctime_r(timep, task_info->ctime_buf);
}

struct tm *why_gmtime(time_t const *timep) {
    task_info_t *task_info = get_task_info();
    return gmtime_r(timep, &task_info->gmtime_tm);
}

struct tm *why_localtime(time_t const *timep) {
    task_info_t *task_info = get_task_info();
    return localtime_r(timep, &task_info->localtime_tm);
}

int why_rand() {
    task_info_t *task_info = get_task_info();
    return rand_r(&task_info->seed);
}

void why_srand(unsigned int seed) {
    task_info_t *task_info = get_task_info();
    task_info->seed        = seed;
}

void why_srandom(unsigned int seed) {
    task_info_t *task_info = get_task_info();
    task_info->seed        = seed;
}

long why_random() {
    task_info_t *task_info = get_task_info();
    return rand_r(&task_info->seed);
}

int *why___errno() {
    task_info_t *task_info = get_task_info();
    ESP_LOGI("why_errno", "Calling __errno from task %p", task_info->handle);
    return &task_info->_errno;
}

int *why_errno() {
    task_info_t *task_info = get_task_info();
    ESP_LOGI("why_errno", "Calling errno from task %p", task_info->handle);
    return &task_info->_errno;
}

int why_isatty(int fd) {
    task_info_t *task_info = get_task_info();
    ESP_LOGI("why_isatty", "Calling isatty from task %p", task_info->handle);
    return 1;
}

char *why_getenv(char const *name) {
    task_info_t *task_info = get_task_info();
    ESP_LOGW("why_getenv", "Calling getenv from task %p variable %s", task_info->handle, name);

    if (strcmp(name, "TERM") == 0) {
        return "line";
    }

    return NULL;
}

int why_atexit(void (*function)(void)) {
    task_info_t *task_info = get_task_info();
    ESP_LOGI("why_atexit", "Calling atexit from task %p", task_info->handle);

    return 0;
}

void IRAM_ATTR *why_malloc(size_t size) {
    task_info_t *task_info = get_task_info();

    if (!task_info->pid) {
        xSemaphoreTake(kernel_malloc_lock, portMAX_DELAY);
    }
    // ESP_LOGW("malloc", "Calling malloc(%zi) from task %d", size, task_info->pid);
    void *ptr = dlmalloc(size);

    // ESP_LOGI("malloc", "Calling malloc(%zi) from task %d, returning %p", size, task_info->pid, ptr);
    if (!task_info->pid) {
        xSemaphoreGive(kernel_malloc_lock);
    }
    return ptr;
}

void IRAM_ATTR *why_calloc(size_t nmemb, size_t size) {
    task_info_t *task_info = get_task_info();

    if (!task_info->pid) {
        xSemaphoreTake(kernel_malloc_lock, portMAX_DELAY);
    }

    // task_info_t *task_info = get_task_info();
    // ESP_LOGI("calloc", "Calling calloc(%zi, %zi) from task %d", nmemb, size, task_info->pid);
    void *ptr = dlcalloc(nmemb, size);

    if (!task_info->pid) {
        xSemaphoreGive(kernel_malloc_lock);
    }
    return ptr;
}

void IRAM_ATTR *why_realloc(void *_Nullable ptr, size_t size) {
    task_info_t *task_info = get_task_info();

    if (!task_info->pid) {
        xSemaphoreTake(kernel_malloc_lock, portMAX_DELAY);
    }

    // task_info_t *task_info = get_task_info();
    // ESP_LOGI("realloc", "Calling realloc(%p, %zi) from task %d", ptr, size, task_info->pid);
    void *new_ptr = dlrealloc(ptr, size);

    if (!task_info->pid) {
        xSemaphoreGive(kernel_malloc_lock);
    }
    return new_ptr;
}

void *why_reallocarray(void *_Nullable ptr, size_t nmemb, size_t size) {
    task_info_t *task_info = get_task_info();

    if (!task_info->pid) {
        xSemaphoreTake(kernel_malloc_lock, portMAX_DELAY);
    }
    // task_info_t *task_info = get_task_info();
    // ESP_LOGI("reallocarray", "Calling reallocarray(%p, %zi, %zi) from task %d", ptr, nmemb, size, task_info->pid);
    void *new_ptr = dlrealloc(ptr, nmemb * size);

    if (!task_info->pid) {
        xSemaphoreGive(kernel_malloc_lock);
    }
    return new_ptr;
}

void IRAM_ATTR why_free(void *_Nullable ptr) {
    task_info_t *task_info = get_task_info();

    if (!task_info->pid) {
        xSemaphoreTake(kernel_malloc_lock, portMAX_DELAY);
    }
    dlfree(ptr);
    if (!task_info->pid) {
        xSemaphoreGive(kernel_malloc_lock);
    }
}

iconv_t why_iconv_open(char const *tocode, char const *fromcode) {
    iconv_t res = iconv_open(tocode, fromcode);
    if (likely(res != (iconv_t)-1)) {
        task_record_resource_alloc(RES_ICONV_OPEN, res);
    }
    return res;
}

int why_iconv_close(iconv_t cd) {
    task_record_resource_free(RES_ICONV_OPEN, cd);
    return iconv_close(cd);
}

int why_regcomp(regex_t *restrict preg, char const *restrict regex, int cflags) {
    int res = regcomp(preg, regex, cflags);
    if (res == 0) {
        task_record_resource_alloc(RES_REGCOMP, preg);
    }
    return res;
}

void why_regfree(regex_t *preg) {
    task_record_resource_free(RES_REGCOMP, preg);
    return regfree(preg);
}

int why_tcgetattr(int fd, struct termios *termios_p) {
    return 0;
}

int why_tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
    return 0;
}

wchar_t *why_wcsdup(wchar_t const *s) {
    if (!s) {
        return NULL;
    }

    size_t   len = wcslen(s);
    wchar_t *ptr = why_malloc((len + 1) * sizeof(wchar_t));
    if (ptr) {
        memcpy(ptr, s, len * sizeof(wchar_t));
        ptr[len] = L'\0';
    }
    return ptr;
}

char *why_strdup(char const *s) {
    if (!s) {
        return NULL;
    }

    size_t len = strlen(s);
    char  *ptr = why_malloc((len + 1) * sizeof(char));
    if (ptr) {
        memcpy(ptr, s, len * sizeof(char));
        ptr[len] = '\0';
    }
    return ptr;
}

char *why_strndup(char const *s, size_t n) {
    size_t len = strnlen(s, n);
    char  *ptr = why_malloc((len + 1) * sizeof(char));
    if (ptr) {
        memcpy(ptr, s, len * sizeof(char));
        ptr[len] = '\0';
    }
    return ptr;
}

IRAM_ATTR
ssize_t why_write(int fd, void const *buf, size_t count) {
    if (fd < 0 || fd >= MAXFD || !get_task_info()->thread->file_handles[fd].is_open) {
        get_task_info()->_errno = EBADF;
        return -1;
    }

    task_info_t *task_info = get_task_info();
    ESP_LOGD("why_write", "Calling write from task %p fd = %i count = %zi", task_info->handle, fd, count);
    if (task_info->thread->file_handles[fd].device->_write) {
        return task_info->thread->file_handles[fd].device->_write(
            task_info->thread->file_handles[fd].device,
            task_info->thread->file_handles[fd].dev_fd,
            buf,
            count
        );
    } else {
        ESP_LOGE("why_write", "fd %i has no valid write function", fd);
    }
    return 0;
}

IRAM_ATTR
ssize_t why_read(int fd, void *buf, size_t count) {
    if (fd < 0 || fd >= MAXFD || !get_task_info()->thread->file_handles[fd].is_open) {
        get_task_info()->_errno = EBADF;
        return -1;
    }

    task_info_t *task_info = get_task_info();
    ESP_LOGI("why_read", "Calling read from task %p fd = %i count = %zi", task_info->handle, fd, count);
    if (task_info->thread->file_handles[fd].device->_read) {
        ESP_LOGD(
            "why_read",
            "Calling driver _read(%p, %i, %p, %zi)",
            task_info->thread->file_handles[fd].device,
            task_info->thread->file_handles[fd].dev_fd,
            buf,
            count
        );
        return task_info->thread->file_handles[fd].device->_read(
            task_info->thread->file_handles[fd].device,
            task_info->thread->file_handles[fd].dev_fd,
            buf,
            count
        );
    } else {
        ESP_LOGE("why_read", "fd %i has no valid read function", fd);
    }

    return 0;
}

off_t why_lseek(int fd, off_t offset, int whence) {
    task_info_t *task_info = get_task_info();
    ESP_LOGI("why_lseek", "Calling lseek from task %p", task_info->handle);
    if (task_info->thread->file_handles[fd].device->_lseek) {
        return task_info->thread->file_handles[fd].device->_lseek(
            task_info->thread->file_handles[fd].device,
            task_info->thread->file_handles[fd].dev_fd,
            offset,
            whence
        );
    } else {
        ESP_LOGE("why_lseek", "fd %i has no valid lseek function", fd);
    }
    return 0;
}

static int _why_open(char const *pathname, int flags, mode_t mode, device_t **device) {
    int    dev_fd = -1;
    path_t parsed_path;
    int    res = parse_path(pathname, &parsed_path);

    if (res) {
        goto out;
    }

    *device = device_get(parsed_path.device);
    if (!*device) {
        goto out;
    }

    dev_fd = (*device)->_open(device, &parsed_path, flags, mode);
    if (dev_fd < 0) {
        goto out;
    }

out:
    path_free(&parsed_path);
    return dev_fd;
}

int why_socket(int domain, int type, int protocol) {
    task_info_t *task_info = get_task_info();
    ESP_LOGW("why_socket", "Calling socket from task %p", task_info->handle);

    if (domain != AF_INET) {
        task_info->_errno = EAFNOSUPPORT;
        return -1;
    }

    device_t *dev = device_get("SOCKET0");
    if (!dev) {
        task_info->_errno = ENODEV;
        return -1;
    }

    int fd = -1;

    for (int i = 0; i < MAXFD; ++i) {
        if (task_info->thread->file_handles[i].is_open == false) {
            fd = i;
            break;
        }
    }

    if (fd == -1) {
        task_info->_errno = ENOMEM;
        return -1;
    }


    int dev_fd = socket(domain, type, protocol);
    if (dev_fd < 0) {
        task_info->_errno = ENOMEM;
        return -1;
    }

    ESP_LOGW("why_socket", "Got device specific fd %i for task fd %i", dev_fd, fd);
    task_info->thread->file_handles[fd].is_open = true;
    task_info->thread->file_handles[fd].dev_fd  = dev_fd;
    task_info->thread->file_handles[fd].device  = dev;

    return fd;
}

static inline int _why_task_get_socket(int fd) {
    task_info_t *task_info = get_task_info();
    ESP_LOGD("why_open_socket", "Calling open socket from task %p fd %i", task_info->handle, fd);

    if (fd < 0 || fd >= MAXFD || !task_info->thread->file_handles[fd].is_open) {
        task_info->_errno = EBADF;
        return -1;
    }

    if (!task_info->thread->file_handles[fd].device ||
        task_info->thread->file_handles[fd].device->type != DEVICE_TYPE_SOCKET) {
        task_info->_errno = ENOTSOCK;
        return -1;
    }

    return task_info->thread->file_handles[fd].dev_fd;
}

int why_listen(int sockfd, int backlog) {
    int sock = _why_task_get_socket(sockfd);

    if (sock < 0) {
        get_task_info()->_errno = EBADF;
        return -1;
    }

    return listen(sock, backlog);
}


int why_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int sock = _why_task_get_socket(sockfd);

    if (sock < 0) {
        get_task_info()->_errno = EBADF;
        return -1;
    }

    if (addr->sa_family != AF_INET) {
        get_task_info()->_errno = EAFNOSUPPORT;
        return -1;
    }

    struct sockaddr_in *addr_in = (struct sockaddr_in *)addr;
    ESP_LOGI("why_connect", "Connecting to %s:%d", inet_ntoa(addr_in->sin_addr), ntohs(addr_in->sin_port));
    return connect(sock, (struct sockaddr *)addr_in, addrlen);
}

int why_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    int sock = _why_task_get_socket(sockfd);
    if (sock < 0) {
        get_task_info()->_errno = EBADF;
        return -1;
    }
    ESP_LOGW("why_accept", "Accepting connection on socket %i", sockfd);
    int newfd = accept(sock, addr, addrlen);

    if (newfd < 0) {
        get_task_info()->_errno = errno;
        return -1;
    }

    task_info_t *task_info = get_task_info();
    ESP_LOGW("why_accept", "Accepted connection on socket %i, new fd %i", sockfd, newfd);
    for (int i = 0; i < MAXFD; ++i) {
        if (task_info->thread->file_handles[i].is_open == false) {
            task_info->thread->file_handles[i].is_open = true;
            task_info->thread->file_handles[i].dev_fd  = newfd;
            task_info->thread->file_handles[i].device  = task_info->thread->file_handles[sockfd].device;
            ESP_LOGW("why_accept", "Assigned new fd %i to accepted socket %i", i, newfd);
            return i;
        }
    }

    ESP_LOGE("why_accept", "No free file handles available for new socket %i", newfd);
    get_task_info()->_errno = EMFILE;
    close(newfd);
    return -1;
}

int why_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int sock = _why_task_get_socket(sockfd);
    if (sock < 0) {
        get_task_info()->_errno = EBADF;
        return -1;
    }

    if (addr->sa_family != AF_INET) {
        get_task_info()->_errno = EAFNOSUPPORT;
        return -1;
    }

    struct sockaddr_in *addr_in = (struct sockaddr_in *)addr;
    ESP_LOGI("why_bind", "Binding to %s:%d", inet_ntoa(addr_in->sin_addr), ntohs(addr_in->sin_port));
    return bind(sock, (struct sockaddr *)addr_in, addrlen);
}

ssize_t why_recvfrom(int sockfd, void *buf, size_t len, int flags,
                     struct sockaddr *src_addr, socklen_t *addrlen) {
    int sock = _why_task_get_socket(sockfd);
    if (sock < 0) { get_task_info()->_errno = EBADF; return -1; }
    return recvfrom(sock, buf, len, flags, src_addr, addrlen);
}

ssize_t why_sendto(int sockfd, const void *buf, size_t len, int flags,
                   const struct sockaddr *dest_addr, socklen_t addrlen) {
    int sock = _why_task_get_socket(sockfd);
    if (sock < 0) { get_task_info()->_errno = EBADF; return -1; }
    return sendto(sock, buf, len, flags, dest_addr, addrlen);
}

int why_setsockopt(int sockfd, int level, int optname,
                   const void *optval, socklen_t optlen) {
    int sock = _why_task_get_socket(sockfd);
    if (sock < 0) { get_task_info()->_errno = EBADF; return -1; }
    return setsockopt(sock, level, optname, optval, optlen);
}

int why_getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    int sock = _why_task_get_socket(sockfd);
    if (sock < 0) { get_task_info()->_errno = EBADF; return -1; }
    return getsockname(sock, addr, addrlen);
}

int why_open(char const *pathname, int flags, mode_t mode) {
    task_info_t *task_info = get_task_info();
    ESP_LOGI("why_open", "Calling open from task %p for path %s", task_info->handle, pathname);

    int       fd     = -1;
    int       dev_fd = -1;
    device_t *device;

    logical_name_result_t lname        = logical_name_resolve_const(pathname, 0);
    size_t                result_count = lname.result_count;
    // search the list.
    ESP_LOGI("why_open", "Finding file %s, %zi options\n", pathname, result_count);
    for (int i = 0; i < result_count; ++i) {
        ESP_LOGI("why_open", "Trying location: %s\n", lname.result);
        dev_fd = _why_open(lname.result, flags, mode, &device);
        if (dev_fd >= 0) {
            ESP_LOGI("why_open", "Found file at %s\n", lname.result);
            break;
        }

        logical_name_result_free(lname);
        ESP_LOGI("why_open", "Resolving at index %i\n", i + 1);
        lname = logical_name_resolve_const(pathname, i + 1);
    }

    if (dev_fd < 0) {
        task_info->_errno = ENOENT;
        goto out;
    }

    for (int i = 0; i < MAXFD; ++i) {
        if (task_info->thread->file_handles[i].is_open == false) {
            fd = i;
            break;
        }
    }

    if (fd == -1) {
        task_info->_errno = ENOMEM;
        goto out;
    }

    ESP_LOGD("why_open", "Got device specific fd %i for task fd %i", dev_fd, fd);

    task_info->thread->file_handles[fd].is_open = true;
    task_info->thread->file_handles[fd].dev_fd  = dev_fd;
    task_info->thread->file_handles[fd].device  = device;

out:
    ESP_LOGI("why_open", "Calling open from task %p for path %s returning %i", task_info->handle, pathname, fd);
    logical_name_result_free(lname);
    return fd;
}

int why_close(int fd) {
    task_info_t *task_info = get_task_info();
    ESP_LOGI("why_close", "Calling close from task %p", task_info->handle);

    if (fd > MAXFD)
        goto out;

    if (task_info->thread->file_handles[fd].device->_close) {
        int ret = task_info->thread->file_handles[fd].device->_close(
            task_info->thread->file_handles[fd].device,
            task_info->thread->file_handles[fd].dev_fd
        );
        memset(&task_info->thread->file_handles[fd], 0, sizeof(file_handle_t));
        return ret;
    } else {
        ESP_LOGE("why_close", "fd %i has no valid close function", fd);
    }

out:
    task_info->_errno = EBADF;
    return -1;
}

int get_dev_fd(int task_fd) {
    task_info_t *task_info = get_task_info();
    if (task_fd < 0 || task_fd >= MAXFD || !task_info->thread->file_handles[task_fd].is_open)
        return -1;
    return task_info->thread->file_handles[task_fd].dev_fd;
}

pid_t why_getpid(void) {
    task_info_t *task_info = get_task_info();
    return task_info->pid;
}

void why_exit(int status) {
    task_info_t *task_info = get_task_info();
    ESP_LOGW("exit", "Task %u called exit()", task_info->pid);
    vTaskDelete(NULL);
}

int why_system(char const *command) {
    return 0;
}

void why__exit(int status) {
    why_exit(status);
}

void why__Exit(int status) {
    why_exit(status);
}

void why_abort(void) {
    task_info_t *task_info = get_task_info();
    ESP_LOGW("abort", "Task %u called abort()", task_info->pid);
    vTaskDelete(NULL);
}

#undef inet_ntoa
#undef inet_aton
char *inet_ntoa(struct in_addr __in) {
    return ip4addr_ntoa((ip4_addr_t const *)&(__in));
}

int inet_aton(char const *__cp, struct in_addr *__inp) {
    return ip4addr_aton(__cp, (ip4_addr_t *)__inp);
}

char const *get_mac_address() {
    static char mac_address_string[18] = {0};
    uint8_t     mac[6];
    if (mac_address_string[0]) {
        return (char const *)&mac_address_string;
    }

    // Attempt to read the base MAC address. This is the universal MAC address
    // from which other interface-specific MAC addresses are derived.
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_BASE);
    if (ret != ESP_OK) {
        // Log an error if the read operation fails
        ESP_LOGE("MAC_ADDR", "Failed to get base MAC address with error code %d", ret);
        // Clear the string on failure
        strncpy(mac_address_string, "00:00:00:00:00:00", 18);
        return (char const *)&mac_address_string;
    }
    snprintf(mac_address_string, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return (char const *)&mac_address_string;
}

void wrapped_functions_init(void) {
    ESP_LOGI(TAG, "Initializing");

    kernel_malloc_lock = xSemaphoreCreateMutex();
}

static __always_inline uint32_t asuint(float f) {
#if defined(__riscv_flen) && __riscv_flen >= 32
    uint32_t result;
    __asm__("fmv.x.w\t%0, %1" : "=r"(result) : "f"(f));
    return result;
#else
    union {
        float    f;
        uint32_t i;
    } u = {f};
    return u.i;
#endif
}

static __always_inline int issignalingf_inline(float x) {
    uint32_t ix = asuint(x);
    return 2 * (ix ^ 0x00400000u) > 0xFF800000u;
}

int __issignalingf(float f) {
    return issignalingf_inline(f);
}
