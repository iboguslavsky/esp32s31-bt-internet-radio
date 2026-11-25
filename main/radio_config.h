#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADIO_MAX_STATIONS      32
#define RADIO_NAME_MAX_LEN      64
#define RADIO_URL_MAX_LEN       256
#define RADIO_GENRE_MAX_LEN     32
#define RADIO_WIFI_SSID_MAX     33
#define RADIO_WIFI_PASS_MAX     65
#define RADIO_BT_NAME_MAX       64
#define RADIO_BT_PIN_MAX        17

typedef struct {
    char name[RADIO_NAME_MAX_LEN];
    char url[RADIO_URL_MAX_LEN];
    char genre[RADIO_GENRE_MAX_LEN];
} radio_station_t;

typedef struct {
    char wifi_ssid[RADIO_WIFI_SSID_MAX];
    char wifi_pass[RADIO_WIFI_PASS_MAX];

    uint8_t bt_target_bda[6];
    bool bt_has_target;
    char bt_target_name[RADIO_BT_NAME_MAX];
    bool bt_auto_reconnect;
    char bt_pin[RADIO_BT_PIN_MAX];

    uint8_t volume;         /* 0 .. 100 */
    bool muted;
    bool autoplay;
    int32_t active_station; /* 0 .. station_count - 1 */

    int32_t station_count;
    radio_station_t stations[RADIO_MAX_STATIONS];
} radio_config_t;

esp_err_t radio_config_init(void);
void radio_config_get(radio_config_t *out);
void radio_config_get_bt_pin(char *out, size_t out_sz);
bool radio_config_get_bt_target(uint8_t out_bda[6], char *out_name, size_t name_sz, bool *out_auto_reconnect);
void radio_config_get_bt_options(bool *out_auto_reconnect, char *out_pin, size_t pin_sz);
void radio_config_get_volume(uint8_t *out_vol, bool *out_muted);
void radio_config_get_playback_prefs(int32_t *out_active, int32_t *out_count, bool *out_autoplay);
bool radio_config_get_station(int index, radio_station_t *out_station);
bool radio_config_get_wifi(char *out_ssid, size_t ssid_sz, char *out_pass, size_t pass_sz);

esp_err_t radio_config_set_wifi(const char *ssid, const char *password);
esp_err_t radio_config_clear_wifi(void);

esp_err_t radio_config_set_bt_target(const uint8_t bda[6], const char *name);
esp_err_t radio_config_clear_bt_target(void);
esp_err_t radio_config_set_bt_options(bool auto_reconnect, const char *pin);

esp_err_t radio_config_set_volume(uint8_t volume, bool muted);
esp_err_t radio_config_set_playback_prefs(int32_t active_station, bool autoplay);

esp_err_t radio_config_add_station(const char *name, const char *url, const char *genre, int *out_index);
esp_err_t radio_config_update_station(int index, const char *name, const char *url, const char *genre);
esp_err_t radio_config_delete_station(int index);
esp_err_t radio_config_reset_stations_default(void);

#ifdef __cplusplus
}
#endif
