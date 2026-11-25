#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Waveshare ESP32-S31-Zero Onboard Peripherals */
#define BOARD_WS2812_GPIO       60
#define BOARD_BOOT_BUTTON_GPIO  61

typedef enum {
    BOARD_LED_WIFI_WAIT = 0,  /* Breathing amber: waiting for Wi-Fi / SoftAP setup */
    BOARD_LED_BT_SCANNING,    /* Fast blue pulse: Bluetooth scanning / pairing */
    BOARD_LED_IDLE_READY,     /* Soft cyan: Wi-Fi + BT ready, playback stopped/paused */
    BOARD_LED_STREAMING,      /* Solid emerald green: actively streaming audio over A2DP */
    BOARD_LED_BUFFERING,      /* Gentle purple pulse: connecting/buffering radio stream */
    BOARD_LED_ERROR,          /* Red blink: stream or network error */
} board_led_state_t;

typedef enum {
    BOARD_BTN_SINGLE_CLICK = 0,
    BOARD_BTN_DOUBLE_CLICK,
    BOARD_BTN_LONG_PRESS,
} board_button_event_t;

typedef void (*board_button_cb_t)(board_button_event_t evt);

esp_err_t board_hw_init(board_button_cb_t btn_cb);
void board_hw_set_led_state(board_led_state_t state);
bool board_hw_get_temp_c(float *out_temp_c);

#ifdef __cplusplus
}
#endif
