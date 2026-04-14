#include <stdio.h>
#include <stdlib.h>

#include <badgevms/pathfuncs.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static char const *get_file_type_string(mode_t mode) {
    if (S_ISREG(mode))
        return "regular file";
    if (S_ISDIR(mode))
        return "directory";
    if (S_ISCHR(mode))
        return "character device";
    if (S_ISBLK(mode))
        return "block device";
    if (S_ISFIFO(mode))
        return "FIFO/pipe";
    if (S_ISLNK(mode))
        return "symbolic link";
    if (S_ISSOCK(mode))
        return "socket";
    return "unknown";
}

static void print_permissions(mode_t mode) {
    printf("  Permissions: ");
    printf((mode & S_IRUSR) ? "r" : "-");
    printf((mode & S_IWUSR) ? "w" : "-");
    printf((mode & S_IXUSR) ? "x" : "-");
    printf((mode & S_IRGRP) ? "r" : "-");
    printf((mode & S_IWGRP) ? "w" : "-");
    printf((mode & S_IXGRP) ? "x" : "-");
    printf((mode & S_IROTH) ? "r" : "-");
    printf((mode & S_IWOTH) ? "w" : "-");
    printf((mode & S_IXOTH) ? "x" : "-");
    printf(" (octal: %o)\n", mode & 0777);
}

static void print_dirent_info(const struct dirent *entry, int entry_num) {
    printf("\n--- Directory Entry #%d ---\n", entry_num);
    printf("  d_name: \"%s\"\n", entry->d_name);
    printf("  d_name length: %zu characters\n", strlen(entry->d_name));

    printf("  d_name analysis: ");
    bool has_slash     = strchr(entry->d_name, '/') != NULL;
    bool has_backslash = strchr(entry->d_name, '\\') != NULL;
    bool has_colon     = strchr(entry->d_name, ':') != NULL;

    if (has_slash || has_backslash || has_colon) {
        printf("WARNING - contains path separators! (");
        if (has_slash)
            printf("/ ");
        if (has_backslash)
            printf("\\ ");
        if (has_colon)
            printf(": ");
        printf(")\n");
    } else {
        printf("clean filename (no path separators)\n");
    }

#ifdef _DIRENT_HAVE_D_TYPE
    printf("  d_type: ");
    switch (entry->d_type) {
        case DT_BLK: printf("block device\n"); break;
        case DT_CHR: printf("character device\n"); break;
        case DT_DIR: printf("directory\n"); break;
        case DT_FIFO: printf("FIFO/pipe\n"); break;
        case DT_LNK: printf("symbolic link\n"); break;
        case DT_REG: printf("regular file\n"); break;
        case DT_SOCK: printf("socket\n"); break;
        case DT_UNKNOWN: printf("unknown\n"); break;
        default: printf("invalid (%d)\n", entry->d_type); break;
    }
#endif

#ifdef _DIRENT_HAVE_D_INO
    printf("  d_ino: %lu\n", (unsigned long)entry->d_ino);
#endif

#ifdef _DIRENT_HAVE_D_OFF
    printf("  d_off: %ld\n", (long)entry->d_off);
#endif

#ifdef _DIRENT_HAVE_D_RECLEN
    printf("  d_reclen: %u\n", entry->d_reclen);
#endif
}

static void test_stat_consistency(char const *base_path, char const *filename) {
    char full_path[1024];

    if (strlen(base_path) > 0 && base_path[strlen(base_path) - 1] == ':') {
        snprintf(full_path, sizeof(full_path), "%s%s", base_path, filename);
    } else {
        if (strlen(base_path) > 0) {
            snprintf(full_path, sizeof(full_path), "%s:%s", base_path, filename);
        } else {
            snprintf(full_path, sizeof(full_path), "%s", filename);
        }
    }

    printf("  Testing stat consistency with path: \"%s\"\n", full_path);

    struct stat statbuf;
    int         stat_result = stat(full_path, &statbuf);

    if (stat_result == 0) {
        printf("    Stat successful!\n");
        printf("    File type: %s\n", get_file_type_string(statbuf.st_mode));
        printf("    Size: %ld bytes\n", (long)statbuf.st_size);
        print_permissions(statbuf.st_mode);
        printf("    Links: %ld\n", (long)statbuf.st_nlink);
        printf("    UID: %u, GID: %u\n", statbuf.st_uid, statbuf.st_gid);

        char       time_str[64];
        struct tm *tm_info;

        if (statbuf.st_mtime > 0) {
            tm_info = localtime(&statbuf.st_mtime);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
            printf("    Modified: %s\n", time_str);
        }

        if (statbuf.st_atime > 0) {
            tm_info = localtime(&statbuf.st_atime);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
            printf("    Accessed: %s\n", time_str);
        }

        if (statbuf.st_ctime > 0) {
            tm_info = localtime(&statbuf.st_ctime);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
            printf("    Changed: %s\n", time_str);
        }

    } else {
        printf("    Stat failed! errno: %d (%s)\n", errno, strerror(errno));
        printf("    This indicates inconsistency - readdir returned an entry that can't be stat'd!\n");
    }
}

static bool test_single_directory(char const *dir_path) {
    printf("\n");
    printf("================================================================\n");
    printf("Testing directory: \"%s\"\n", dir_path);
    printf("================================================================\n");

    printf("Opening directory...\n");
    DIR *dir = opendir(dir_path);

    if (!dir) {
        printf("ERROR: Failed to open directory \"%s\"\n", dir_path);
        printf("errno: %d (%s)\n", errno, strerror(errno));
        return false;
    }

    printf("Directory opened successfully\n");
    printf("Directory handle: %p (should be non-NULL)\n", (void *)dir);

    printf("\n=== Reading Directory Entries ===\n");

    struct dirent *entry;
    int            entry_count            = 0;
    int            total_name_length      = 0;
    bool           found_suspicious_names = false;

    while ((entry = readdir(dir)) != NULL) {
        entry_count++;
        print_dirent_info(entry, entry_count);

        int name_len       = strlen(entry->d_name);
        total_name_length += name_len;

        if (strchr(entry->d_name, '/') || strchr(entry->d_name, '\\') || strstr(entry->d_name, "..")) {
            found_suspicious_names = true;
        }

        test_stat_consistency(dir_path, entry->d_name);

        printf("\n");
    }

    printf("=== End of Directory Reached ===\n");
    printf("readdir() returned NULL (expected at end)\n");
    printf("errno after NULL return: %d (%s)\n", errno, errno == 0 ? "no error" : strerror(errno));

    printf("\n=== Directory Summary ===\n");
    printf("Total entries found: %d\n", entry_count);
    if (entry_count > 0) {
        printf("Average filename length: %.1f characters\n", (float)total_name_length / entry_count);
    }
    printf("Suspicious filenames detected: %s\n", found_suspicious_names ? "YES - CHECK FOR PATH LEAKAGE!" : "No");

    printf("\n=== Closing Directory ===\n");
    int close_result = closedir(dir);
    if (close_result == 0) {
        printf("Directory closed successfully\n");
    } else {
        printf("Directory close failed! Return code: %d\n", close_result);
        printf("errno: %d (%s)\n", errno, strerror(errno));
    }

    printf("\n=== Error Handling Test ===\n");
    printf("Attempting to read from closed directory (should fail)...\n");
    struct dirent *should_be_null = readdir(dir);
    if (should_be_null == NULL) {
        printf("Correctly returned NULL for closed directory\n");
        printf("errno: %d (%s)\n", errno, strerror(errno));
    } else {
        printf("ERROR: readdir on closed directory returned: %p\n", (void *)should_be_null);
    }

    printf(
        "\nDirectory \"%s\": %s\n",
        dir_path,
        found_suspicious_names ? "FAILED - found suspicious paths!" : "PASSED"
    );

    return !found_suspicious_names;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <directory_path1> [directory_path2] [...]\n", argv[0]);
        printf("Examples:\n");
        printf("  %s \"APP:\"\n", argv[0]);
        printf("  %s \"SYS:\" \"APP:[SUBDIR]\" \"DATA:\"\n", argv[0]);
        printf("  %s \"HOME:\" \"TMP:\" \"CONFIG:\"\n", argv[0]);
        return 1;
    }

    printf("=== Comprehensive Directory Reading Test ===\n");
    printf("Testing %d director%s for path leakage and consistency...\n", argc - 1, (argc - 1) == 1 ? "y" : "ies");

    bool all_passed   = true;
    int  passed_count = 0;
    int  failed_count = 0;

    for (int i = 1; i < argc; i++) {
        bool result = test_single_directory(argv[i]);
        if (result) {
            passed_count++;
        } else {
            failed_count++;
            all_passed = false;
        }
    }

    printf("\n");
    printf("================================================================\n");
    printf("FINAL SUMMARY\n");
    printf("================================================================\n");
    printf("Directories tested: %d\n", argc - 1);
    printf("Passed: %d\n", passed_count);
    printf("Failed: %d\n", failed_count);
    printf("Overall result: %s\n", all_passed ? "ALL PASSED" : "SOME FAILED");
    printf(
        "Path leakage validation: %s\n",
        all_passed ? "PASSED - no leakage detected" : "FAILED - check output above"
    );

    return all_passed ? 0 : 1;
}
