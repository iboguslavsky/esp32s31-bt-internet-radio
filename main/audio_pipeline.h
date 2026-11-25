#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAYER_STATE_STOPPED = 0,
    PLAYER_STATE_CONNECTING,
    PLAYER_STATE_BUFFERING,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_PAUSED,
    PLAYER_STATE_ERROR,
} player_state_t;

typedef enum {
    STREAM_CODEC_UNKNOWN = 0,
    STREAM_CODEC_MP3,
    STREAM_CODEC_AAC,
} stream_codec_t;

typedef struct {
    player_state_t state;
    stream_codec_t codec;
    int32_t station_index; /* -1 when playing custom ad-hoc URL */

    char station_name[64];
    char stream_url[256];
    char server_name[64];
    char icy_title[160];
    char error_msg[96];

    uint32_t sample_rate_in;
    uint8_t channels_in;
    uint32_t bitrate_kbps;

    uint8_t stream_buf_pct;
    uint8_t pcm_buf_pct;
    uint8_t vu_left;
    uint8_t vu_right;

    uint64_t bytes_received;
    uint32_t frames_decoded;
    uint32_t underruns;
} audio_pipeline_status_t;

esp_err_t audio_pipeline_init(void);

esp_err_t audio_pipeline_play_station(int station_index);
esp_err_t audio_pipeline_play_url(const char *url, const char *display_name);
esp_err_t audio_pipeline_pause(void);
esp_err_t audio_pipeline_resume(void);
esp_err_t audio_pipeline_toggle_play_pause(void);
esp_err_t audio_pipeline_stop(void);
esp_err_t audio_pipeline_next_station(void);
esp_err_t audio_pipeline_prev_station(void);

void audio_pipeline_set_volume(uint8_t volume_pct, bool muted);
void audio_pipeline_get_status(audio_pipeline_status_t *out);

/* Callback registered directly with Classic Bluetooth A2DP Source */
int32_t audio_pipeline_a2dp_pcm_cb(uint8_t *data, int32_t len);

#ifdef __cplusplus
}
#endif
