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

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WIFI_DISABLED,
    WIFI_ENABLED,
    WIFI_ASK,
} wifi_status_t;

typedef enum {
    WIFI_ERROR,
    WIFI_DISCONNECTED,
    WIFI_CONNECTED,
    WIFI_ERROR_WRONG_CREDENTIALS,
} wifi_connection_status_t;

// From ESP-IDF 5.5
typedef enum {
    WIFI_CIPHER_TYPE_NONE,
    WIFI_CIPHER_TYPE_WEP40,
    WIFI_CIPHER_TYPE_WEP104,
    WIFI_CIPHER_TYPE_TKIP,
    WIFI_CIPHER_TYPE_CCMP,
    WIFI_CIPHER_TYPE_TKIP_CCMP,
    WIFI_CIPHER_TYPE_AES_CMAC128,
    WIFI_CIPHER_TYPE_SMS4,
    WIFI_CIPHER_TYPE_GCMP,
    WIFI_CIPHER_TYPE_GCMP256,
    WIFI_CIPHER_TYPE_AES_GMAC128,
    WIFI_CIPHER_TYPE_AES_GMAC256,
    WIFI_CIPHER_TYPE_UNKNOWN,
} wifi_cipher_type_t;

// From ESP-IDF 5.5
typedef enum {
    WIFI_AUTH_NONE,
    WIFI_AUTH_OPEN,
    WIFI_AUTH_WEP,
    WIFI_AUTH_WPA_PSK,
    WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_WPA2_ENTERPRISE,
    WIFI_AUTH_WPA3_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK,
    WIFI_AUTH_WAPI_PSK,
    WIFI_AUTH_OWE,
    WIFI_AUTH_WPA3_ENT_192,
    WIFI_AUTH_DPP,
    WIFI_AUTH_WPA3_ENTERPRISE,
    WIFI_AUTH_WPA2_WPA3_ENTERPRISE,
    WIFI_AUTH_WPA_ENTERPRISE,
} wifi_auth_mode_t;

typedef enum {
    WIFI_MODE_NONE = 0,
    WIFI_MODE_11B  = 1 << 0, /**< 802.11b */
    WIFI_MODE_11G  = 1 << 1, /**< 802.11g */
    WIFI_MODE_11N  = 1 << 2, /**< 802.11n */
    WIFI_MODE_LR   = 1 << 3, /**< Low Rate */
    WIFI_MODE_11A  = 1 << 4, /**< 802.11a */
    WIFI_MODE_11AC = 1 << 5, /**< 802.11ac */
    WIFI_MODE_11AX = 1 << 6, /**< 802.11ax */
} wifi_connection_mode_t;

typedef struct wifi_station *wifi_station_handle;
typedef uint8_t              mac_address_t[6];

wifi_status_t wifi_get_status();

wifi_connection_status_t wifi_get_connection_status();
wifi_station_handle      wifi_get_connection_station();
wifi_connection_status_t wifi_connect();
wifi_connection_status_t wifi_disconnect();

void wifi_scan_free_station(wifi_station_handle station);

void                wifi_scan_start(void);
int                 wifi_scan_get_num_results();
int                 wifi_scan_get_cached_num_results(void);
wifi_station_handle wifi_scan_get_result(int num);

char const      *wifi_station_get_ssid(wifi_station_handle station);
mac_address_t   *wifi_station_get_bssid(wifi_station_handle station);
int              wifi_station_get_primary_channel(wifi_station_handle station);
int              wifi_station_get_secondary_channel(wifi_station_handle station);
int              wifi_station_get_rssi(wifi_station_handle station);
wifi_auth_mode_t wifi_station_get_mode(wifi_station_handle station);
bool             wifi_station_wps(wifi_station_handle station);
bool             wifi_set_connection_parameters(char const *ssid, char const *password);
void             wifi_set_hostname(char const *hostname);
void             wifi_set_mac_randomization(bool enabled);
void             wifi_set_enabled(bool enabled);

/* Access-point mode — badge broadcasts its own WiFi network.
 * Uses APSTA so an existing STA connection is preserved.
 * The AP interface is always at 192.168.4.1 with DHCP;
 * DHCP automatically advertises 192.168.4.1 as the DNS server.
 * Pass an empty string for password to create an open network. */
bool wifi_start_ap(const char *ssid, const char *password);
void wifi_stop_ap(void);
