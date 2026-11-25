#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SCAN_MAX_APS   20
#define WIFI_SOFTAP_SSID    "ESP32-S31-Radio"
#define WIFI_MDNS_HOSTNAME  "esp32s31-radio"

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;
} wifi_ap_entry_t;

typedef struct {
    bool sta_configured;
    bool sta_connecting;
    bool sta_connected;
    char sta_ssid[33];
    char sta_ip[16];
    int8_t sta_rssi;
    uint8_t sta_channel;

    bool ap_active;
    char ap_ssid[33];
    char ap_ip[16];

    char mac_str[18];
    char hostname[32];
} wifi_status_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_connect(const char *ssid, const char *password, bool persist);
esp_err_t wifi_manager_disconnect_and_clear(void);
esp_err_t wifi_manager_scan(wifi_ap_entry_t *out_aps, size_t max_aps, size_t *out_count);
void wifi_manager_get_status(wifi_status_t *out);
bool wifi_manager_is_connected(void);

#ifdef __cplusplus
}
#endif
