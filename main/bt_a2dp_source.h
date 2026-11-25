#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BT_SCAN_MAX_DEVICES 24
#define BT_BOND_MAX_DEVICES 16

typedef enum {
    BT_AV_CMD_PLAY = 0,
    BT_AV_CMD_PAUSE,
    BT_AV_CMD_STOP,
    BT_AV_CMD_NEXT,
    BT_AV_CMD_PREV,
    BT_AV_CMD_VOL_CHANGED,
} bt_avrc_remote_cmd_t;

typedef int32_t (*bt_a2dp_pcm_data_cb_t)(uint8_t *data, int32_t len);
typedef void (*bt_avrc_remote_cb_t)(bt_avrc_remote_cmd_t cmd, uint8_t volume_pct);

typedef struct {
    uint8_t bda[6];
    char bda_str[18];
    char name[64];
    int8_t rssi;
    uint32_t cod;
    bool is_audio_sink;
    bool bonded;
} bt_device_info_t;

typedef struct {
    bool initialized;
    bool scanning;
    bool connecting;
    bool connected;
    bool media_started;
    bool avrc_connected;

    char local_bda_str[18];
    char local_name[32];

    uint8_t peer_bda[6];
    char peer_bda_str[18];
    char peer_name[64];

    uint8_t negotiated_bitpool;
    char codec_desc[32];
    uint16_t sink_delay_tenths_ms;
} bt_a2dp_status_t;

esp_err_t bt_a2dp_source_start(bt_a2dp_pcm_data_cb_t pcm_cb, bt_avrc_remote_cb_t remote_cb);

esp_err_t bt_a2dp_scan_start(uint8_t duration_sec, bool auto_connect_first_sink);
esp_err_t bt_a2dp_scan_stop(void);

esp_err_t bt_a2dp_connect(const uint8_t bda[6], const char *name);
esp_err_t bt_a2dp_disconnect(void);
esp_err_t bt_a2dp_forget_device(const uint8_t bda[6]);
esp_err_t bt_a2dp_forget_all(void);

esp_err_t bt_a2dp_set_volume(uint8_t volume_pct);
esp_err_t bt_a2dp_set_wifi_afh_channel(uint8_t wifi_ch);
bool bt_a2dp_peer_supports_abs_vol(void);

void bt_a2dp_get_status(bt_a2dp_status_t *out);
size_t bt_a2dp_get_scan_results(bt_device_info_t *out_list, size_t max_items);
size_t bt_a2dp_get_bonded_devices(bt_device_info_t *out_list, size_t max_items);

bool bt_parse_bda(const char *str, uint8_t out_bda[6]);
char *bt_format_bda(const uint8_t bda[6], char *buf, size_t buf_sz);

#ifdef __cplusplus
}
#endif
