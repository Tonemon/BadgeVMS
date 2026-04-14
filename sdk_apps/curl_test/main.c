#include "badgevms/wifi.h"
#include "curl/curl.h"

#include <stdio.h>
#include <stdlib.h>

#include <string.h>

// Example 1: Simple GET request
void simple_get_example() {
    CURL    *curl;
    CURLcode res;

    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/get");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "BadgeVMS-libcurl/1.0");

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        }

        curl_easy_cleanup(curl);
    }
}

// Example 2: POST with JSON data
void post_json_example() {
    CURL              *curl;
    CURLcode           res;
    struct curl_slist *headers = NULL;

    curl = curl_easy_init();
    if (curl) {
        // Set URL
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/post");

        // Set headers
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Set POST data
        char const *json_data = "{\"name\":\"BadgeVMS\",\"value\":42}";
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_data);

        // Perform request
        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        }

        // Cleanup
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

// Example 3: Custom write callback for capturing response
typedef struct {
    char  *memory;
    size_t size;
} MemoryStruct;

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, MemoryStruct *mem) {
    size_t realsize = size * nmemb;
    printf("Callback recieved %u bytes\n", realsize);

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        printf("Not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size              += realsize;
    mem->memory[mem->size]  = 0;

    return realsize;
}

void capture_response_example() {
    CURL        *curl;
    CURLcode     res;
    MemoryStruct chunk;

    chunk.memory = malloc(1);
    chunk.size   = 0;

    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://api.github.com/users/espressif");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "BadgeVMS-libcurl/1.0");
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("Received %lu bytes:\n%s\n", (unsigned long)chunk.size, chunk.memory);
        }

        curl_easy_cleanup(curl);
    }

    free(chunk.memory);
}

// Example 4: HTTPS with authentication
void https_auth_example() {
    CURL    *curl;
    CURLcode res;
    long     response_code;

    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/basic-auth/user/pass");
        curl_easy_setopt(curl, CURLOPT_USERPWD, "user:pass");
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        } else {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            printf("Response code: %ld\n", response_code);
        }

        curl_easy_cleanup(curl);
    }
}

// Example 5: File upload simulation
void upload_example() {
    CURL              *curl;
    CURLcode           res;
    struct curl_slist *headers = NULL;

    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/put");

        // Set headers
        headers = curl_slist_append(headers, "Content-Type: text/plain");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Set PUT method with data
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "This is test data from BadgeVMS");

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

// Example 6: Multiple requests with connection reuse (ESP-IDF handles this automatically)
void multiple_requests_example() {
    CURL    *curl;
    CURLcode res;

    char const *urls[] =
        {"https://httpbin.org/get?page=1", "https://httpbin.org/get?page=2", "https://httpbin.org/get?page=3"};

    curl = curl_easy_init();
    if (curl) {
        for (int i = 0; i < 3; i++) {
            printf("\n--- Request %d ---\n", i + 1);
            curl_easy_setopt(curl, CURLOPT_URL, urls[i]);

            res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                printf("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            }
        }

        curl_easy_cleanup(curl);
    }
}

// Example 7: Cookie jar functionality test
void cookie_jar_example() {
    CURL       *curl;
    CURLcode    res;
    char const *test_cookie_file = "FLASH0:test_cookies.txt";

    printf("Testing cookie jar functionality...\n");

    // Test 1: Manual cookie setting
    curl = curl_easy_init();
    if (curl) {
        printf("Setting manual cookies...\n");
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies");
        curl_easy_setopt(curl, CURLOPT_COOKIE, "manual_cookie=test_value; session_id=abc123");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "BadgeVMS-libcurl/1.0");

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("Manual cookie test failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("Manual cookies sent successfully\n");
        }

        curl_easy_cleanup(curl);
    }

    // Test 2: Cookie jar file saving (CURLOPT_COOKIEJAR)
    printf("\nTesting CURLOPT_COOKIEJAR (save cookies to file)...\n");
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies/set/jar_test/saved_value");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "BadgeVMS-libcurl/1.0");
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR, test_cookie_file); // Save cookies here
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("Cookie jar save test failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("Request completed, cookies should be saved to file\n");

            // Verify file was created
            FILE *verify_file = fopen(test_cookie_file, "r");
            if (verify_file) {
                printf("Cookie file created successfully\n");
                char line[256];
                int  line_count = 0;
                while (fgets(line, sizeof(line), verify_file) && line_count < 5) {
                    printf("   %s", line);
                    line_count++;
                }
                fclose(verify_file);
            } else {
                printf("Cookie file was not created\n");
            }
        }

        curl_easy_cleanup(curl);
    }

    // Test 3: Cookie file loading (CURLOPT_COOKIEFILE)
    printf("\nTesting CURLOPT_COOKIEFILE (load cookies from file)...\n");
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "BadgeVMS-libcurl/1.0");
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, test_cookie_file); // Load cookies from file
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);                  // Show what cookies are being sent

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("Cookie file load test failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("Request completed with loaded cookies\n");
        }

        curl_easy_cleanup(curl);
    }

    // Clean up test file
    if (remove(test_cookie_file) == 0) {
        printf("Test cookie file cleaned up\n");
    }
}

// Example 8: Cookie persistence across requests
void cookie_persistence_example() {
    CURL    *curl;
    CURLcode res;

    printf("Testing cookie persistence across requests...\n");

    curl = curl_easy_init();
    if (curl) {
        // First request - this should set cookies from the server
        printf("Making first request to set cookies...\n");
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies/set/persistent_cookie/esp32_value");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "BadgeVMS-libcurl/1.0");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // Follow redirects

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("First request failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("First request completed - cookies should be stored\n");
        }

        // Second request - should automatically send stored cookies
        printf("\nMaking second request - should send stored cookies...\n");
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies");

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("Second request failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("Second request completed - stored cookies should be visible in response\n");
        }

        curl_easy_cleanup(curl);
    }
}

// Example 9: Proxy configuration test (should fail gracefully)
void proxy_stub_example() {
    CURL    *curl;
    CURLcode res;

    printf("Testing proxy configuration (should show not implemented warnings)...\n");

    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/get");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "BadgeVMS-libcurl/1.0");

        // These should all return CURLE_UNSUPPORTED_PROTOCOL
        printf("Testing HTTP proxy setting...\n");
        res = curl_easy_setopt(curl, CURLOPT_PROXY, "http://proxy.example.com:8080");
        if (res != CURLE_OK) {
            printf("Proxy setting correctly returned error: %s\n", curl_easy_strerror(res));
        }

        printf("Testing proxy authentication...\n");
        res = curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, "user:pass");
        if (res != CURLE_OK) {
            printf("Proxy auth correctly returned error: %s\n", curl_easy_strerror(res));
        }

        printf("Testing proxy type setting...\n");
        res = curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
        if (res != CURLE_OK) {
            printf("Proxy type correctly returned error: %s\n", curl_easy_strerror(res));
        }

        printf("Testing proxy port setting...\n");
        res = curl_easy_setopt(curl, CURLOPT_PROXYPORT, 1080L);
        if (res != CURLE_OK) {
            printf("Proxy port correctly returned error: %s\n", curl_easy_strerror(res));
        }

        printf("Testing proxy auth type...\n");
        res = curl_easy_setopt(curl, CURLOPT_PROXYAUTH, CURLAUTH_DIGEST);
        if (res != CURLE_OK) {
            printf("Proxy auth type correctly returned error: %s\n", curl_easy_strerror(res));
        }

        printf("Testing HTTP auth type (should work)...\n");
        res = curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        if (res == CURLE_OK) {
            printf("HTTP auth type setting works correctly\n");
        }

        // Test unimplemented options
        printf("Testing unimplemented range option...\n");
        res = curl_easy_setopt(curl, CURLOPT_RANGE, "0-1023");
        if (res != CURLE_OK) {
            printf("Range option correctly returned error: %s\n", curl_easy_strerror(res));
        }

        printf("Testing unimplemented referer option...\n");
        res = curl_easy_setopt(curl, CURLOPT_REFERER, "https://example.com");
        if (res != CURLE_OK) {
            printf("Referer option correctly returned error: %s\n", curl_easy_strerror(res));
        }

        curl_easy_cleanup(curl);
    }
}

// Example 10: Custom header callback to see cookie headers
static size_t header_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    char  *header   = (char *)contents;

    // Print Set-Cookie headers to show cookie jar is working
    if (realsize >= 11 && strncasecmp(header, "Set-Cookie:", 11) == 0) {
        printf("Server set cookie: %.*s", (int)realsize, header);
    }
    // Print Cookie headers to show we're sending cookies
    else if (realsize >= 7 && strncasecmp(header, "Cookie:", 7) == 0) {
        printf("We sent cookies: %.*s", (int)realsize, header);
    }

    return realsize;
}

void cookie_visibility_example() {
    CURL    *curl;
    CURLcode res;

    printf("Testing cookie header visibility...\n");

    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies/set/test_visibility/working");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "BadgeVMS-libcurl/1.0");
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L); // Enable verbose mode

        printf("First request - should see Set-Cookie header:\n");
        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("Request failed: %s\n", curl_easy_strerror(res));
        }

        printf("\nSecond request - should see Cookie header being sent:\n");
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L); // Don't follow redirects this time

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("Request failed: %s\n", curl_easy_strerror(res));
        }

        curl_easy_cleanup(curl);
    }
}

// Example 11: File-based cookie persistence test
void cookie_file_persistence_example() {
    CURL       *curl;
    CURLcode    res;
    char const *cookie_file = "FLASH0:cookies.txt";

    printf("Testing file-based cookie persistence...\n");

    // First session - set up cookies and save to file
    printf("\n=== Session 1: Setting cookies and saving to file ===\n");
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies/set/file_test/persistent_value");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "BadgeVMS-libcurl/1.0");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookie_file); // Save cookies to file
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("Session 1 failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("Session 1 completed - cookies saved to: %s\n", cookie_file);
        }

        curl_easy_cleanup(curl);
    }

    printf("\n=== Checking cookie file contents ===\n");
    FILE *file = fopen(cookie_file, "r");
    if (file) {
        char line[256];
        while (fgets(line, sizeof(line), file)) {
            printf("Cookie file: %s", line);
        }
        fclose(file);
    } else {
        printf("Could not read cookie file\n");
    }

    // Second session - load cookies from file and use them
    printf("\n=== Session 2: Loading cookies from file ===\n");
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "BadgeVMS-libcurl/1.0");
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookie_file); // Load cookies from file
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("Session 2 failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("Session 2 completed - should show loaded cookies in response\n");
        }

        curl_easy_cleanup(curl);
    }

    // Clean up test file
    remove(cookie_file);
    printf("\nCookie file test completed and cleaned up\n");
}
void main() {
    // Initialize (global init - no-op on ESP-IDF but maintains compatibility)

    wifi_connect();
    curl_global_init(0);

    printf("=== ESP-CURL Examples ===\n\n");

    printf("1. Simple GET request:\n");
    simple_get_example();

    printf("\n2. POST with JSON:\n");
    post_json_example();

    printf("\n3. Capture response in memory:\n");
    capture_response_example();

    printf("\n4. HTTPS with authentication:\n");
    https_auth_example();

    printf("\n5. File upload (PUT):\n");
    upload_example();

    printf("\n6. Multiple requests:\n");
    multiple_requests_example();

    printf("\n7. Cookie jar functionality:\n");
    cookie_jar_example();

    printf("\n8. Cookie persistence across requests:\n");
    cookie_persistence_example();

    printf("\n9. Proxy configuration stubs:\n");
    proxy_stub_example();

    printf("\n10. Cookie header visibility:\n");
    cookie_visibility_example();

    // Cleanup (no-op on ESP-IDF but maintains compatibility)
    curl_global_cleanup();
    wifi_disconnect();
}
