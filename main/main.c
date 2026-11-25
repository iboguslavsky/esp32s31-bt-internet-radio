#include <stdio.h>
#include "audio_pipeline.h"
#include "board_hw.h"
#include "bt_a2dp_source.h"
#include "esp_coexist.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "radio_config.h"
#include "usb_cli.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "s31_radio";

static void on_board_button(board_button_event_t evt)
{
    switch (evt) {
    case BOARD_BTN_SINGLE_CLICK:
        ESP_LOGI(TAG, "BOOT button click -> Toggle Play/Pause");
        audio_pipeline_toggle_play_pause();
        break;
    case BOARD_BTN_DOUBLE_CLICK:
        ESP_LOGI(TAG, "BOOT button double-click -> Next Station");
        audio_pipeline_next_station();
        break;
    case BOARD_BTN_LONG_PRESS:
        ESP_LOGI(TAG, "BOOT button long-press -> Auto-scan & pair nearest Bluetooth audio sink");
        bt_a2dp_scan_start(12, true);
        break;
    }
}

static void on_bt_avrc_remote(bt_avrc_remote_cmd_t cmd, uint8_t volume_pct)
{
    switch (cmd) {
    case BT_AV_CMD_PLAY:
        audio_pipeline_resume();
        break;
    case BT_AV_CMD_PAUSE:
        audio_pipeline_pause();
        break;
    case BT_AV_CMD_STOP:
        audio_pipeline_stop();
        break;
    case BT_AV_CMD_NEXT:
        audio_pipeline_next_station();
        break;
    case BT_AV_CMD_PREV:
        audio_pipeline_prev_station();
        break;
    case BT_AV_CMD_VOL_CHANGED:
        radio_config_set_volume(volume_pct, false);
        audio_pipeline_set_volume(volume_pct, false);
        break;
    }
}

static void supervisor_task(void *arg)
{
    (void)arg;
    bool autoplay_triggered = false;
    uint8_t last_afh_ch = 0;
    bool last_bt_connected = false;
    uint64_t last_bytes = 0;
    uint32_t last_frames = 0;
    uint32_t stall_ticks = 0;
    radio_config_t *cfg = malloc(sizeof(radio_config_t));
    configASSERT(cfg != NULL);

    while (true) {
        wifi_status_t wf;
        bt_a2dp_status_t bt;
        audio_pipeline_status_t ap;

        wifi_manager_get_status(&wf);
        bt_a2dp_get_status(&bt);
        audio_pipeline_get_status(&ap);
        radio_config_get(cfg);

        /* Apply Classic BT AFH channel blacklist whenever Wi-Fi channel changes or BT connects */
        if (wf.sta_connected && wf.sta_channel >= 1 && wf.sta_channel <= 13 &&
            (wf.sta_channel != last_afh_ch || (bt.connected && !last_bt_connected))) {
            if (bt_a2dp_set_wifi_afh_channel(wf.sta_channel) == ESP_OK) {
                last_afh_ch = wf.sta_channel;
            }
        }
        last_bt_connected = bt.connected;

        /* Trigger initial station stream automatically once Wi-Fi connects if autoplay is enabled */
        if (!autoplay_triggered && wf.sta_connected) {
            autoplay_triggered = true;
            if (cfg->autoplay && ap.state == PLAYER_STATE_STOPPED && cfg->station_count > 0) {
                ESP_LOGI(TAG, "Wi-Fi ready -> Auto-playing station [%ld]: %s",
                         (long)cfg->active_station, cfg->stations[cfg->active_station].name);
                audio_pipeline_play_station(cfg->active_station);
            }
        }

        /* Self-healing stream stall watchdog (50 * 400 ms = 20 s of no progress while buffers empty) */
        if (wf.sta_connected &&
            (ap.state == PLAYER_STATE_PLAYING || ap.state == PLAYER_STATE_BUFFERING)) {
            if (ap.bytes_received == last_bytes && ap.frames_decoded == last_frames &&
                ap.stream_buf_pct == 0 && ap.pcm_buf_pct == 0) {
                if (++stall_ticks >= 50) {
                    stall_ticks = 0;
                    ESP_LOGW(TAG, "Watchdog: stream stalled for 20 s with empty buffers -> reconnecting");
                    if (ap.station_index >= 0) {
                        audio_pipeline_play_station((int)ap.station_index);
                    } else if (ap.stream_url[0] != '\0') {
                        audio_pipeline_play_url(ap.stream_url, ap.station_name);
                    }
                }
            } else {
                stall_ticks = 0;
                last_bytes = ap.bytes_received;
                last_frames = ap.frames_decoded;
            }
        } else {
            stall_ticks = 0;
            last_bytes = ap.bytes_received;
            last_frames = ap.frames_decoded;
        }

        /* Drive onboard WS2812B-1010 RGB LED status */
        if (bt.scanning) {
            board_hw_set_led_state(BOARD_LED_BT_SCANNING);
        } else if (!wf.sta_connected) {
            board_hw_set_led_state(BOARD_LED_WIFI_WAIT);
        } else if (ap.state == PLAYER_STATE_ERROR) {
            board_hw_set_led_state(BOARD_LED_ERROR);
        } else if (ap.state == PLAYER_STATE_CONNECTING || ap.state == PLAYER_STATE_BUFFERING) {
            board_hw_set_led_state(BOARD_LED_BUFFERING);
        } else if (ap.state == PLAYER_STATE_PLAYING && bt.connected) {
            board_hw_set_led_state(BOARD_LED_STREAMING);
        } else {
            board_hw_set_led_state(BOARD_LED_IDLE_READY);
        }

        vTaskDelay(pdMS_TO_TICKS(400));
    }
}

void app_main(void)
{
    esp_log_level_set("BT_L2CAP", ESP_LOG_NONE);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Booting Waveshare ESP32-S31-Zero Classic Bluetooth Internet Radio...");

    ESP_ERROR_CHECK(radio_config_init());
    ESP_ERROR_CHECK(board_hw_init(on_board_button));
    ESP_ERROR_CHECK(audio_pipeline_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(bt_a2dp_source_start(audio_pipeline_a2dp_pcm_cb, on_bt_avrc_remote));
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    ESP_ERROR_CHECK(web_server_start());
    ESP_ERROR_CHECK(usb_cli_start());

    xTaskCreate(supervisor_task, "radio_sup", 4096, NULL, 4, NULL);
}
