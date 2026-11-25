#include "wifi_manager.h"
#include "radio_config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_coexist.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "wifi_mgr";

#define MAX_STA_RETRIES_BEFORE_AP 5

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static SemaphoreHandle_t s_lock = NULL;

static wifi_status_t s_status;
static int s_retry_count = 0;
static bool s_manual_disconnect = false;
static uint32_t s_sta_ip_be = 0;

static void enable_softap_fallback(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_APSTA || mode == WIFI_MODE_AP) {
        return;
    }

    wifi_config_t ap_cfg = {0};
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", WIFI_SOFTAP_SSID);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(WIFI_SOFTAP_SSID);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.pmf_cfg.required = false;

    ESP_LOGI(TAG, "Enabling SoftAP setup portal: SSID='%s' -> http://192.168.4.1", WIFI_SOFTAP_SSID);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.ap_active = true;
    snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid), "%s", WIFI_SOFTAP_SSID);
    snprintf(s_status.ap_ip, sizeof(s_status.ap_ip), "192.168.4.1");
    xSemaphoreGive(s_lock);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool configured = s_status.sta_configured;
        xSemaphoreGive(s_lock);
        if (configured && !s_manual_disconnect) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.sta_connected = false;
        s_status.sta_ip[0] = '\0';
        s_status.sta_rssi = 0;
        s_sta_ip_be = 0;
        bool configured = s_status.sta_configured;
        xSemaphoreGive(s_lock);

        if (!configured || s_manual_disconnect) {
            return;
        }

        s_retry_count++;
        if (s_retry_count >= MAX_STA_RETRIES_BEFORE_AP) {
            enable_softap_fallback();
        }
        ESP_LOGW(TAG, "Wi-Fi STA disconnected, retry #%d...", s_retry_count);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_retry_count = 0;

        wifi_ap_record_t ap_info = {0};
        int8_t rssi = 0;
        uint8_t channel = 0;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
            channel = ap_info.primary;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.sta_connecting = false;
        s_status.sta_connected = true;
        s_status.sta_rssi = rssi;
        s_status.sta_channel = channel;
        s_sta_ip_be = event->ip_info.ip.addr;
        snprintf(s_status.sta_ip, sizeof(s_status.sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        xSemaphoreGive(s_lock);

        ESP_LOGI(TAG, "==========================================================");
        ESP_LOGI(TAG, "Wi-Fi Connected! SSID: %s", s_status.sta_ssid);
        ESP_LOGI(TAG, "Desktop WebUI URL: http://%s/  (or http://%s.local/)",
                 s_status.sta_ip, WIFI_MDNS_HOSTNAME);
        ESP_LOGI(TAG, "==========================================================");

        /* Turn off fallback SoftAP once STA has a valid IP so 2.4 GHz airtime stays clean for A2DP */
        wifi_mode_t mode = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_APSTA) {
            esp_wifi_set_mode(WIFI_MODE_STA);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.ap_active = false;
            xSemaphoreGive(s_lock);
        }
    }
}

/* Lightweight RFC 6762 mDNS responder for esp32s31-radio.local (UDP 224.0.0.251:5353) */
static void mdns_responder_task(void *arg)
{
    (void)arg;
    uint8_t rx_buf[512];
    uint8_t tx_buf[256];

    while (true) {
        if (s_sta_ip_be == 0) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in bind_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(5353),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        struct ip_mreq mreq = {0};
        mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
        mreq.imr_interface.s_addr = s_sta_ip_be;
        setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

        struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        const size_t host_len = strlen(WIFI_MDNS_HOSTNAME);

        while (s_sta_ip_be != 0) {
            struct sockaddr_in src_addr;
            socklen_t addr_len = sizeof(src_addr);
            int n = recvfrom(sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&src_addr, &addr_len);
            if (n < 18) {
                continue;
            }
            /* Only process standard mDNS queries (QR bit == 0) */
            if ((rx_buf[2] & 0x80) != 0) {
                continue;
            }
            uint16_t qdcount = ((uint16_t)rx_buf[4] << 8) | rx_buf[5];
            if (qdcount == 0) {
                continue;
            }

            /* Parse first question QNAME for <WIFI_MDNS_HOSTNAME>.local */
            int pos = 12;
            if (pos + 1 + (int)host_len + 7 + 4 > n) {
                continue;
            }
            if (rx_buf[pos] != host_len) {
                continue;
            }
            bool match = true;
            for (size_t i = 0; i < host_len; i++) {
                if (tolower((unsigned char)rx_buf[pos + 1 + i]) != tolower((unsigned char)WIFI_MDNS_HOSTNAME[i])) {
                    match = false;
                    break;
                }
            }
            int local_pos = pos + 1 + (int)host_len;
            if (!match || rx_buf[local_pos] != 5) {
                continue;
            }
            const char *loc = "local";
            for (int i = 0; i < 5; i++) {
                if (tolower((unsigned char)rx_buf[local_pos + 1 + i]) != loc[i]) {
                    match = false;
                    break;
                }
            }
            int end_pos = local_pos + 6;
            if (!match || rx_buf[end_pos] != 0) {
                continue;
            }
            if (end_pos + 5 > n) {
                continue;
            }
            uint16_t qtype = ((uint16_t)rx_buf[end_pos + 1] << 8) | rx_buf[end_pos + 2];
            if (qtype != 1 && qtype != 255) { /* A (1) or ANY (255) */
                continue;
            }

            /* Build authoritative mDNS A-record response */
            size_t idx = 0;
            tx_buf[idx++] = rx_buf[0];
            tx_buf[idx++] = rx_buf[1];
            tx_buf[idx++] = 0x84; /* QR=1, AA=1 */
            tx_buf[idx++] = 0x00;
            tx_buf[idx++] = 0x00;
            tx_buf[idx++] = 0x00; /* QDCOUNT=0 */
            tx_buf[idx++] = 0x00;
            tx_buf[idx++] = 0x01; /* ANCOUNT=1 */
            tx_buf[idx++] = 0x00;
            tx_buf[idx++] = 0x00;
            tx_buf[idx++] = 0x00;
            tx_buf[idx++] = 0x00;

            tx_buf[idx++] = (uint8_t)host_len;
            memcpy(&tx_buf[idx], WIFI_MDNS_HOSTNAME, host_len);
            idx += host_len;
            tx_buf[idx++] = 5;
            memcpy(&tx_buf[idx], "local", 5);
            idx += 5;
            tx_buf[idx++] = 0;

            tx_buf[idx++] = 0x00;
            tx_buf[idx++] = 0x01; /* TYPE A */
            tx_buf[idx++] = 0x80;
            tx_buf[idx++] = 0x01; /* CLASS IN + cache-flush */
            tx_buf[idx++] = 0x00;
            tx_buf[idx++] = 0x00;
            tx_buf[idx++] = 0x00;
            tx_buf[idx++] = 120;  /* TTL 120s */
            tx_buf[idx++] = 0x00;
            tx_buf[idx++] = 0x04; /* RDLENGTH 4 */
            uint32_t ip = s_sta_ip_be;
            memcpy(&tx_buf[idx], &ip, 4);
            idx += 4;

            struct sockaddr_in mcast_dst = {
                .sin_family = AF_INET,
                .sin_port = htons(5353),
                .sin_addr.s_addr = inet_addr("224.0.0.251"),
            };
            sendto(sock, tx_buf, idx, 0, (struct sockaddr *)&mcast_dst, sizeof(mcast_dst));
        }

        close(sock);
    }
}

esp_err_t wifi_manager_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    memset(&s_status, 0, sizeof(s_status));
    snprintf(s_status.hostname, sizeof(s_status.hostname), "%s", WIFI_MDNS_HOSTNAME);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, WIFI_MDNS_HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_status.mac_str, sizeof(s_status.mac_str),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    char wifi_ssid[RADIO_WIFI_SSID_MAX] = {0};
    char wifi_pass[RADIO_WIFI_PASS_MAX] = {0};
    if (radio_config_get_wifi(wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass))) {
        wifi_config_t sta_cfg = {0};
        memcpy(sta_cfg.sta.ssid, wifi_ssid, strnlen(wifi_ssid, sizeof(sta_cfg.sta.ssid)));
        memcpy(sta_cfg.sta.password, wifi_pass, strnlen(wifi_pass, sizeof(sta_cfg.sta.password)));
        sta_cfg.sta.threshold.authmode = wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

        s_status.sta_configured = true;
        s_status.sta_connecting = true;
        snprintf(s_status.sta_ssid, sizeof(s_status.sta_ssid), "%s", wifi_ssid);

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        /* Unconfigured: start APSTA so both USB-C Serial CLI and SoftAP WebUI work immediately */
        wifi_config_t ap_cfg = {0};
        snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", WIFI_SOFTAP_SSID);
        ap_cfg.ap.ssid_len = (uint8_t)strlen(WIFI_SOFTAP_SSID);
        ap_cfg.ap.channel = 1;
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

        s_status.ap_active = true;
        snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid), "%s", WIFI_SOFTAP_SSID);
        snprintf(s_status.ap_ip, sizeof(s_status.ap_ip), "192.168.4.1");

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "No Wi-Fi configured yet. Configure via USB-C CLI ('wifi set <ssid> <pass>') or SoftAP '%s' (http://192.168.4.1)",
                 WIFI_SOFTAP_SSID);
    }

    /* Use 20 MHz OFDM-only (no slow 1 Mbps 802.11b) + MIN_MODEM PS for AP-friendly BT coex */
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
    if (esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX) != ESP_OK) {
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    }
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);

    xTaskCreate(mdns_responder_task, "mdns_s31", 4096, NULL, 3, NULL);
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password, bool persist)
{
    if (!ssid || ssid[0] == '\0' || strlen(ssid) >= 33) {
        return ESP_ERR_INVALID_ARG;
    }
    if (persist) {
        esp_err_t err = radio_config_set_wifi(ssid, password ? password : "");
        if (err != ESP_OK) {
            return err;
        }
    }

    s_manual_disconnect = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(80));

    wifi_config_t sta_cfg = {0};
    memcpy(sta_cfg.sta.ssid, ssid, strnlen(ssid, sizeof(sta_cfg.sta.ssid)));
    if (password) {
        memcpy(sta_cfg.sta.password, password, strnlen(password, sizeof(sta_cfg.sta.password)));
    }
    sta_cfg.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.sta_configured = true;
    s_status.sta_connecting = true;
    s_status.sta_connected = false;
    s_status.sta_ip[0] = '\0';
    s_sta_ip_be = 0;
    snprintf(s_status.sta_ssid, sizeof(s_status.sta_ssid), "%s", ssid);
    xSemaphoreGive(s_lock);

    s_retry_count = 0;
    s_manual_disconnect = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    return esp_wifi_connect();
}

esp_err_t wifi_manager_disconnect_and_clear(void)
{
    s_manual_disconnect = true;
    esp_wifi_disconnect();
    radio_config_clear_wifi();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.sta_configured = false;
    s_status.sta_connecting = false;
    s_status.sta_connected = false;
    s_status.sta_ssid[0] = '\0';
    s_status.sta_ip[0] = '\0';
    s_sta_ip_be = 0;
    xSemaphoreGive(s_lock);

    enable_softap_fallback();
    return ESP_OK;
}

esp_err_t wifi_manager_scan(wifi_ap_entry_t *out_aps, size_t max_aps, size_t *out_count)
{
    if (!out_aps || max_aps == 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        return ESP_OK;
    }
    if (ap_num > WIFI_SCAN_MAX_APS) {
        ap_num = WIFI_SCAN_MAX_APS;
    }

    wifi_ap_record_t records[WIFI_SCAN_MAX_APS];
    err = esp_wifi_scan_get_ap_records(&ap_num, records);
    if (err != ESP_OK) {
        return err;
    }

    size_t count = 0;
    for (uint16_t i = 0; i < ap_num && count < max_aps; i++) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }
        /* Deduplicate by SSID keeping strongest RSSI */
        bool dup = false;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(out_aps[j].ssid, (const char *)records[i].ssid) == 0) {
                if (records[i].rssi > out_aps[j].rssi) {
                    out_aps[j].rssi = records[i].rssi;
                    out_aps[j].channel = records[i].primary;
                    out_aps[j].authmode = (uint8_t)records[i].authmode;
                }
                dup = true;
                break;
            }
        }
        if (!dup) {
            snprintf(out_aps[count].ssid, sizeof(out_aps[count].ssid), "%s", (const char *)records[i].ssid);
            out_aps[count].rssi = records[i].rssi;
            out_aps[count].channel = records[i].primary;
            out_aps[count].authmode = (uint8_t)records[i].authmode;
            count++;
        }
    }
    *out_count = count;
    return ESP_OK;
}

void wifi_manager_get_status(wifi_status_t *out)
{
    if (!out) {
        return;
    }
    wifi_ap_record_t ap_info = {0};
    bool update_rssi = false;
    int8_t live_rssi = 0;
    if (s_status.sta_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        live_rssi = ap_info.rssi;
        update_rssi = true;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (update_rssi) {
        s_status.sta_rssi = live_rssi;
    }
    *out = s_status;
    xSemaphoreGive(s_lock);
}

bool wifi_manager_is_connected(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool conn = s_status.sta_connected;
    xSemaphoreGive(s_lock);
    return conn;
}
