#include "board_hw.h"

#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "driver/temperature_sensor.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_hw";

#define RMT_RES_HZ 10000000

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_symbol;
} ws2812_encoder_t;

static rmt_channel_handle_t s_rmt_chan = NULL;
static rmt_encoder_handle_t s_rmt_enc = NULL;
static temperature_sensor_handle_t s_tsens = NULL;
static volatile float s_chip_temp_c = 0.0f;
static volatile bool s_temp_valid = false;
static volatile board_led_state_t s_led_state = BOARD_LED_WIFI_WAIT;
static board_button_cb_t s_btn_cb = NULL;

RMT_ENCODER_FUNC_ATTR
static size_t encode_ws2812(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                            const void *data, size_t data_size,
                            rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *ws = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded = 0;

    if (ws->state == 0) {
        encoded += ws->bytes_encoder->encode(ws->bytes_encoder, channel, data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            *ret_state = state;
            return encoded;
        }
    }

    if (ws->state == 1) {
        encoded += ws->copy_encoder->encode(ws->copy_encoder, channel, &ws->reset_symbol,
                                            sizeof(ws->reset_symbol), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws->state = 0;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
        }
    }

    *ret_state = state;
    return encoded;
}

static esp_err_t delete_ws2812_encoder(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(ws->bytes_encoder);
    rmt_del_encoder(ws->copy_encoder);
    free(ws);
    return ESP_OK;
}

RMT_ENCODER_FUNC_ATTR
static esp_err_t reset_ws2812_encoder(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(ws->bytes_encoder);
    rmt_encoder_reset(ws->copy_encoder);
    ws->state = 0;
    return ESP_OK;
}

static esp_err_t create_ws2812_encoder(rmt_encoder_handle_t *ret_encoder)
{
    esp_err_t ret = ESP_OK;
    ws2812_encoder_t *ws = rmt_alloc_encoder_mem(sizeof(ws2812_encoder_t));
    ESP_RETURN_ON_FALSE(ws != NULL, ESP_ERR_NO_MEM, TAG, "no mem for ws2812 encoder");

    ws->base.encode = encode_ws2812;
    ws->base.del = delete_ws2812_encoder;
    ws->base.reset = reset_ws2812_encoder;
    ws->state = 0;

    const rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 3, /* 0.3 us @ 10 MHz */
            .level1 = 0,
            .duration1 = 9, /* 0.9 us @ 10 MHz */
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 9, /* 0.9 us @ 10 MHz */
            .level1 = 0,
            .duration1 = 3, /* 0.3 us @ 10 MHz */
        },
        .flags.msb_first = 1,
    };
    ESP_GOTO_ON_ERROR(rmt_new_bytes_encoder(&bytes_cfg, &ws->bytes_encoder), fail, TAG, "bytes encoder failed");

    const rmt_copy_encoder_config_t copy_cfg = {};
    ESP_GOTO_ON_ERROR(rmt_new_copy_encoder(&copy_cfg, &ws->copy_encoder), fail, TAG, "copy encoder failed");

    const uint32_t reset_ticks = RMT_RES_HZ / 1000000 * 50 / 2;
    ws->reset_symbol = (rmt_symbol_word_t){
        .level0 = 0,
        .duration0 = reset_ticks,
        .level1 = 0,
        .duration1 = reset_ticks,
    };

    *ret_encoder = &ws->base;
    return ESP_OK;

fail:
    if (ws->bytes_encoder) {
        rmt_del_encoder(ws->bytes_encoder);
    }
    if (ws->copy_encoder) {
        rmt_del_encoder(ws->copy_encoder);
    }
    free(ws);
    return ret;
}

static void ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_rmt_chan || !s_rmt_enc) {
        return;
    }
    const uint8_t grb[3] = {g, r, b};
    const rmt_transmit_config_t tx_cfg = {.loop_count = 0};
    if (rmt_transmit(s_rmt_chan, s_rmt_enc, grb, sizeof(grb), &tx_cfg) == ESP_OK) {
        rmt_tx_wait_all_done(s_rmt_chan, pdMS_TO_TICKS(20));
    }
}

static void hw_worker_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;

    int stable_btn = 1;
    int raw_prev = 1;
    int debounce_cnt = 0;
    int64_t press_start_us = 0;
    bool long_fired = false;
    int pending_clicks = 0;
    int64_t last_release_us = 0;

    while (true) {
        /* 1. Poll BOOT button (GPIO61, active-low) at 20 ms cadence */
        int raw = gpio_get_level(BOARD_BOOT_BUTTON_GPIO);
        if (raw == raw_prev) {
            if (debounce_cnt < 2) {
                debounce_cnt++;
            }
        } else {
            raw_prev = raw;
            debounce_cnt = 0;
        }

        int64_t now_us = esp_timer_get_time();
        if (debounce_cnt >= 2 && raw != stable_btn) {
            stable_btn = raw;
            if (stable_btn == 0) {
                press_start_us = now_us;
                long_fired = false;
            } else {
                if (!long_fired) {
                    pending_clicks++;
                    last_release_us = now_us;
                }
            }
        }

        if (stable_btn == 0 && !long_fired && (now_us - press_start_us) >= 2500000LL) {
            long_fired = true;
            pending_clicks = 0;
            if (s_btn_cb) {
                s_btn_cb(BOARD_BTN_LONG_PRESS);
            }
        }

        if (pending_clicks > 0 && stable_btn == 1 && (now_us - last_release_us) >= 320000LL) {
            int clicks = pending_clicks;
            pending_clicks = 0;
            if (s_btn_cb) {
                s_btn_cb(clicks >= 2 ? BOARD_BTN_DOUBLE_CLICK : BOARD_BTN_SINGLE_CLICK);
            }
        }

        /* 2. Update WS2812B-1010 RGB status LED (subtle brightness <= 28) */
        if ((tick % 3) == 0) {
            uint32_t phase = (tick / 3) % 40;
            uint8_t wave = (phase < 20) ? (uint8_t)(phase + 4) : (uint8_t)(44 - phase);
            switch (s_led_state) {
            case BOARD_LED_WIFI_WAIT:
                ws2812_set_rgb(wave, (uint8_t)(wave / 3), 0);
                break;
            case BOARD_LED_BT_SCANNING:
                ws2812_set_rgb(0, 0, ((tick / 6) & 1) ? 28 : 3);
                break;
            case BOARD_LED_IDLE_READY:
                ws2812_set_rgb(0, 10, 14);
                break;
            case BOARD_LED_STREAMING:
                ws2812_set_rgb(0, 20, 4);
                break;
            case BOARD_LED_BUFFERING:
                ws2812_set_rgb((uint8_t)(wave / 2), 0, wave);
                break;
            case BOARD_LED_ERROR:
                ws2812_set_rgb(((tick / 8) & 1) ? 26 : 0, 0, 0);
                break;
            }
        }

        /* 3. Sample internal chip temperature every ~2 s (100 * 20 ms) */
        if (s_tsens && (tick % 100) == 0) {
            float t = 0.0f;
            if (temperature_sensor_get_celsius(s_tsens, &t) == ESP_OK) {
                s_chip_temp_c = t;
                s_temp_valid = true;
                if (t >= 80.0f && (tick % 500) == 0) {
                    ESP_LOGW(TAG, "High chip temperature: %.1f C", t);
                }
            }
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t board_hw_init(board_button_cb_t btn_cb)
{
    s_btn_cb = btn_cb;

    const gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    const rmt_tx_channel_config_t chan_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = BOARD_WS2812_GPIO,
        .mem_block_symbols = 48,
        .resolution_hz = RMT_RES_HZ,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&chan_cfg, &s_rmt_chan);
    if (err == ESP_OK) {
        err = create_ws2812_encoder(&s_rmt_enc);
        if (err == ESP_OK) {
            rmt_enable(s_rmt_chan);
            ws2812_set_rgb(12, 4, 0);
            ESP_LOGI(TAG, "Initialized WS2812B LED (GPIO%d) + BOOT button (GPIO%d)",
                     BOARD_WS2812_GPIO, BOARD_BOOT_BUTTON_GPIO);
        }
    } else {
        ESP_LOGW(TAG, "RMT WS2812B init skipped (%s)", esp_err_to_name(err));
    }

    const temperature_sensor_config_t tsens_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    err = temperature_sensor_install(&tsens_cfg, &s_tsens);
    if (err == ESP_OK) {
        err = temperature_sensor_enable(s_tsens);
        if (err == ESP_OK) {
            float t = 0.0f;
            if (temperature_sensor_get_celsius(s_tsens, &t) == ESP_OK) {
                s_chip_temp_c = t;
                s_temp_valid = true;
                ESP_LOGI(TAG, "Internal temperature sensor enabled (%.1f C)", t);
            }
        } else {
            ESP_LOGW(TAG, "Temperature sensor enable failed (%s)", esp_err_to_name(err));
            s_tsens = NULL;
        }
    } else {
        ESP_LOGW(TAG, "Temperature sensor install failed (%s)", esp_err_to_name(err));
        s_tsens = NULL;
    }

    xTaskCreate(hw_worker_task, "board_hw", 3072, NULL, 4, NULL);
    return ESP_OK;
}

void board_hw_set_led_state(board_led_state_t state)
{
    s_led_state = state;
}

bool board_hw_get_temp_c(float *out_temp_c)
{
    if (!s_temp_valid) {
        return false;
    }
    if (out_temp_c) {
        *out_temp_c = s_chip_temp_c;
    }
    return true;
}
