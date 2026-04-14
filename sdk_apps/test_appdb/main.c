#include <stdio.h>

#include <badgevms/application.h>

int main(int argc, char *argv[]) {
    printf("Application Management Testing\n");

    printf("Creating applications...\n");

    application_t *app1 = application_create(
        "com_example_myapp",
        "My Test App",
        "Example Developer",
        "1.0.0",
        NULL,
        APPLICATION_SOURCE_UNKNOWN
    );
    if (app1) {
        printf("SUCCESS: Created app1 (com_example_myapp)\n");
    } else {
        printf("INFO: app1 already exists or failed to create\n");
        app1 = application_get("com_example_myapp");
    }

    application_t *app2 = application_create(
        "com_badgehub_calculator",
        "Calculator",
        "BadgeHub Team",
        "2.1.3",
        "python",
        APPLICATION_SOURCE_BADGEHUB
    );
    if (app2) {
        printf("SUCCESS: Created app2 (com_badgehub_calculator)\n");
    } else {
        printf("INFO: app2 already exists or failed to create\n");
        app2 = application_get("com_badgehub_calculator");
    }

    application_t *app3 = application_create(
        "com_example_testapp",
        "Test Application",
        "Test Author",
        "0.1.0",
        "javascript",
        APPLICATION_SOURCE_UNKNOWN
    );
    if (app3) {
        printf("SUCCESS: Created app3 (com_example_testapp)\n");
    } else {
        printf("INFO: app3 already exists or failed to create\n");
        app3 = application_get("com_example_testapp");
    }
    printf("\n");

    printf("Testing property setters...\n");
    if (app1) {
        if (application_set_metadata(app1, "metadata.json")) {
            printf("SUCCESS: Set metadata for app1\n");
        }
        if (application_set_binary_path(app1, "myapp.bin")) {
            printf("SUCCESS: Set binary path for app1\n");
        }
    }

    if (app2) {
        if (application_set_version(app2, "2.1.4")) {
            printf("SUCCESS: Updated version for app2 (2.1.3 -> 2.1.4)\n");
        }
        if (application_set_author(app2, "BadgeHub Community")) {
            printf("SUCCESS: Updated author for app2\n");
        }
        if (application_set_interpreter(app2, "python3")) {
            printf("SUCCESS: Updated interpreter for app2\n");
        }
    }

    if (app3) {
        if (application_set_metadata(app3, "[config]app.conf")) {
            printf("SUCCESS: Set nested metadata path for app3\n");
        }
        if (application_set_binary_path(app3, "[scripts]main.js")) {
            printf("SUCCESS: Set nested binary path for app3\n");
        }
    }
    printf("\n");

    printf("Listing all applications...\n");
    application_t          *first_app;
    application_list_handle list = application_list(&first_app);

    if (list) {
        int            count   = 0;
        application_t *current = first_app;
        while (current) {
            count++;
            printf(
                "  App %d: %s (%s) v%s by %s\n",
                count,
                current->name ?: "Unknown",
                current->unique_identifier ?: "No ID",
                current->version ?: "Unknown",
                current->author ?: "Unknown"
            );
            printf("    Source: %d, Interpreter: %s\n", current->source, current->interpreter ?: "native");
            printf("    Metadata: %s, Binary: %s\n", current->metadata_file ?: "none", current->binary_path ?: "none");
            printf("    Install Path: %s\n", current->installed_path ?: "none");
            current = application_list_get_next(list);
        }
        application_list_close(list);
        printf("SUCCESS: Listed %d applications\n", count);
    } else {
        printf("WARNING: No applications found or list failed\n");
    }
    printf("\n");

    printf("Testing application retrieval...\n");
    application_t *found_app = application_get("com_badgehub_calculator");
    if (found_app) {
        printf("SUCCESS: Found calculator app by ID\n");
        printf("  Name: %s\n", found_app->name ?: "Unknown");
        printf("  Version: %s\n", found_app->version ?: "Unknown");
        printf("  Author: %s\n", found_app->author ?: "Unknown");
        printf("  Install Path: %s\n", found_app->installed_path ?: "Unknown");
        application_free(found_app);
    } else {
        printf("WARNING: Could not find calculator app by ID\n");
    }

    application_t *missing_app = application_get("com_nonexistent_app");
    if (!missing_app) {
        printf("SUCCESS: Correctly returned NULL for non-existent app\n");
    } else {
        printf("ERROR: Found app that shouldn't exist\n");
        application_free(missing_app);
    }
    printf("\n");

    printf("Testing file creation...\n");
    if (app1) {
        FILE *test_file = application_create_file(app1, "test.txt");
        if (test_file) {
            printf("SUCCESS: Created file for app1\n");
            fclose(test_file);
        } else {
            printf("INFO: File creation not implemented)\n");
        }
    }
    printf("\n");

    printf("Testing application launch...\n");
    pid_t pid = application_launch("com_example_myapp");
    if (pid > 0) {
        printf("SUCCESS: Launched app with PID %d\n", pid);
    } else {
        printf("INFO: Launch failed or not implemented (PID: %d)\n", pid);
    }
    printf("\n");

    printf("Testing duplicate application creation...\n");
    application_t *duplicate = application_create(
        "com_example_myapp",
        "Duplicate App",
        "Someone Else",
        "2.0.0",
        NULL,
        APPLICATION_SOURCE_UNKNOWN
    );
    if (!duplicate) {
        printf("SUCCESS: Correctly rejected duplicate application\n");
    } else {
        printf("ERROR: Allowed duplicate application creation\n");
        application_free(duplicate);
    }
    printf("\n");

    printf("Testing application destruction...\n");
    if (app3 && application_destroy(app3)) {
        printf("SUCCESS: Destroyed test application\n");
        application_free(app3); // Free the memory
        app3 = NULL;            // Don't try to free it later
    } else {
        printf("WARNING: Failed to destroy test application\n");
    }

    application_t *destroyed_app = application_get("com_example_testapp");
    if (!destroyed_app) {
        printf("SUCCESS: Confirmed application was destroyed\n");
    } else {
        printf("ERROR: Application still exists after destruction\n");
        application_free(destroyed_app);
    }
    printf("\n");

    printf("Testing error cases...\n");

    application_t *null_id_app =
        application_create(NULL, "Bad App", "Bad Author", "1.0", NULL, APPLICATION_SOURCE_UNKNOWN);
    if (!null_id_app) {
        printf("SUCCESS: Correctly rejected NULL identifier\n");
    } else {
        printf("ERROR: Allowed NULL identifier\n");
        application_free(null_id_app);
    }

    if (!application_set_version(NULL, "1.0")) {
        printf("SUCCESS: Correctly rejected NULL application in set_version\n");
    } else {
        printf("ERROR: Allowed NULL application in set_version\n");
    }

    if (app1 && !application_set_metadata(app1, "invalid/path")) {
        printf("SUCCESS: Correctly rejected invalid path\n");
    } else {
        printf("WARNING: Path validation might not be working properly\n");
    }
    printf("\n");

    printf("Testing persistence...\n");
    if (app1) {
        // Change a property
        if (application_set_version(app1, "1.0.1")) {
            printf("Changed app1 version to 1.0.1\n");

            // Free the current reference
            application_free(app1);

            // Reload from disk
            application_t *reloaded = application_get("com_example_myapp");
            if (reloaded && reloaded->version && strcmp(reloaded->version, "1.0.1") == 0) {
                printf("SUCCESS: Version change was persisted\n");
                application_free(reloaded);
            } else {
                printf("ERROR: Version change was not persisted\n");
                if (reloaded)
                    application_free(reloaded);
            }
            app1 = NULL; // Already freed
        }
    }
    printf("\n");

    printf("Cleaning up...\n");
    if (app1) {
        application_free(app1);
        printf("Freed app1\n");
    }
    if (app2) {
        application_free(app2);
        printf("Freed app2\n");
    }
    if (app3) {
        application_free(app3);
        printf("Freed app3\n");
    }

    printf("SUCCESS: All tests completed\n");
    return 0;
}
