#include "audio_pipeline.h"
#include "bt_a2dp_source.h"
#include "radio_config.h"
#include "wifi_manager.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "aacdec.h"
#include "esp_coexist.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "minimp3.h"

static const char *TAG = "audio_pipe";

#define STREAM_RING_BYTES       (2 * 1024 * 1024) /* 2 MB PSRAM: ~128 s @ 128 kbps */
#define STREAM_PREBUFFER_BYTES  (32 * 1024)       /* ~2 s @ 128 kbps for fast tune-in */
#define STREAM_BURST_HIGH_BYTES (1536 * 1024)     /* Pause HTTP read at 75% if server bursts ahead */
#define STREAM_BURST_LOW_BYTES  (1280 * 1024)     /* Resume burst refill at ~62.5% */
#define PCM_RING_BYTES          (2 * 1024 * 1024) /* 2 MB PSRAM: ~11.9 s @ 44.1 kHz stereo */
#define PCM_PREBUFFER_BYTES     (64 * 1024)       /* ~360 ms initial prebuffer for snappy tune-in */
#define PCM_REBUFFER_BYTES      (352 * 1024)      /* ~2.0 s clean rebuffer target after an underrun */
#define PCM_PLL_LOW_BYTES       (600 * 1024)      /* ~3.5 s: below this, micro-PLL slows by -0.15% */
#define PCM_PLL_HIGH_BYTES      (1050 * 1024)     /* ~6.1 s: above this, micro-PLL speeds up by +0.15% */
#define PCM_HIGH_WATERMARK      (1536 * 1024)     /* 75% cap (~8.9 s max decoded reserve) */
#define TARGET_SAMPLE_RATE      44100U
#define DEC_INBUF_SIZE          16384

typedef struct {
    uint8_t *buf;
    size_t cap;
    volatile size_t head;
    volatile size_t tail;
} ring_buffer_t;

typedef struct {
    int metaint;
    int br_kbps;
    stream_codec_t codec_hint;
    bool is_playlist;
    char server_name[64];
} http_hdr_ctx_t;

static ring_buffer_t s_stream_ring;
static ring_buffer_t s_pcm_ring;
static SemaphoreHandle_t s_state_lock = NULL;

static audio_pipeline_status_t s_status;
static volatile uint32_t s_epoch = 0;
static volatile bool s_desired_play = false;
static volatile bool s_paused = false;
static volatile bool s_pcm_primed = false;
static volatile bool s_pcm_rebuffering = false;
static volatile uint8_t s_volume_pct = 75;
static volatile bool s_muted = false;

/* Q15 smoothed gain state */
static int32_t s_current_gain_q15 = 24576;

/* Resampler state across frames */
typedef struct {
    uint32_t in_rate;
    uint32_t phase_q16;
    int16_t prev_l;
    int16_t prev_r;
    bool initialized;
} resampler_state_t;

static esp_err_t ring_init(ring_buffer_t *rb, size_t capacity, bool prefer_psram)
{
    uint32_t caps = prefer_psram ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                 : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    rb->buf = (uint8_t *)heap_caps_malloc(capacity, caps);
    if (!rb->buf) {
        rb->buf = (uint8_t *)heap_caps_malloc(capacity, MALLOC_CAP_8BIT);
    }
    if (!rb->buf) {
        return ESP_ERR_NO_MEM;
    }
    rb->cap = capacity;
    rb->head = 0;
    rb->tail = 0;
    return ESP_OK;
}

static void ring_clear(ring_buffer_t *rb)
{
    __atomic_store_n(&rb->head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rb->tail, 0, __ATOMIC_RELEASE);
}

static inline size_t ring_available_read(const ring_buffer_t *rb)
{
    size_t h = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    size_t t = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    return (h >= t) ? (h - t) : (rb->cap - t + h);
}

static inline size_t ring_available_write(const ring_buffer_t *rb)
{
    return (rb->cap - 1U) - ring_available_read(rb);
}

/* Lock-free SPSC write: never disables interrupts during PSRAM memcpy */
static size_t ring_write(ring_buffer_t *rb, const uint8_t *src, size_t len)
{
    if (!src || len == 0) {
        return 0;
    }
    size_t h = __atomic_load_n(&rb->head, __ATOMIC_RELAXED);
    size_t t = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    size_t used = (h >= t) ? (h - t) : (rb->cap - t + h);
    size_t space = (rb->cap - 1U) - used;
    if (len > space) {
        len = space;
    }
    if (len == 0) {
        return 0;
    }
    size_t first = rb->cap - h;
    if (first > len) {
        first = len;
    }
    memcpy(rb->buf + h, src, first);
    if (len > first) {
        memcpy(rb->buf, src + first, len - first);
    }
    size_t new_h = h + len;
    if (new_h >= rb->cap) {
        new_h -= rb->cap;
    }
    __atomic_store_n(&rb->head, new_h, __ATOMIC_RELEASE);
    return len;
}

/* Lock-free SPSC read: never disables interrupts during PSRAM memcpy */
static size_t ring_read(ring_buffer_t *rb, uint8_t *dst, size_t len)
{
    if (!dst || len == 0) {
        return 0;
    }
    size_t h = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    size_t t = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
    size_t avail = (h >= t) ? (h - t) : (rb->cap - t + h);
    if (len > avail) {
        len = avail;
    }
    if (len == 0) {
        return 0;
    }
    size_t first = rb->cap - t;
    if (first > len) {
        first = len;
    }
    memcpy(dst, rb->buf + t, first);
    if (len > first) {
        memcpy(dst + first, rb->buf, len - first);
    }
    size_t new_t = t + len;
    if (new_t >= rb->cap) {
        new_t -= rb->cap;
    }
    __atomic_store_n(&rb->tail, new_t, __ATOMIC_RELEASE);
    return len;
}

static bool url_has_playlist_ext(const char *url)
{
    if (!url) {
        return false;
    }
    const char *q = strchr(url, '?');
    size_t len = q ? (size_t)(q - url) : strlen(url);
    if (len >= 4 && strncasecmp(url + len - 4, ".pls", 4) == 0) {
        return true;
    }
    if (len >= 4 && strncasecmp(url + len - 4, ".m3u", 4) == 0) {
        return true;
    }
    return false;
}

static bool extract_url_from_playlist(const char *text, char *out_url, size_t out_sz)
{
    if (!text || !out_url || out_sz == 0) {
        return false;
    }
    const char *line = text;
    while (*line) {
        while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') {
            line++;
        }
        if (*line == '\0') {
            break;
        }
        const char *eol = strpbrk(line, "\r\n");
        size_t len = eol ? (size_t)(eol - line) : strlen(line);

        if (len > 7 && line[0] != '#') {
            const char *http = NULL;
            if (strncasecmp(line, "File", 4) == 0) {
                const char *eq = memchr(line, '=', len);
                if (eq && (size_t)(eq - line + 1) < len) {
                    http = eq + 1;
                    len -= (size_t)(http - line);
                }
            } else if (strncasecmp(line, "http://", 7) == 0 || strncasecmp(line, "https://", 8) == 0) {
                http = line;
            }

            if (http && (strncasecmp(http, "http://", 7) == 0 || strncasecmp(http, "https://", 8) == 0)) {
                while (len > 0 && (http[len - 1] == ' ' || http[len - 1] == '\t')) {
                    len--;
                }
                if (len < out_sz) {
                    memcpy(out_url, http, len);
                    out_url[len] = '\0';
                    return true;
                }
            }
        }
        if (!eol) {
            break;
        }
        line = eol + 1;
    }
    return false;
}

static void parse_icy_metadata(const char *meta, size_t len)
{
    if (!meta || len == 0) {
        return;
    }
    char tmp[512];
    size_t copy_len = len < (sizeof(tmp) - 1) ? len : (sizeof(tmp) - 1);
    memcpy(tmp, meta, copy_len);
    tmp[copy_len] = '\0';

    const char *key = "StreamTitle='";
    char *start = strstr(tmp, key);
    if (!start) {
        return;
    }
    start += strlen(key);
    char *end = strstr(start, "';");
    if (!end) {
        end = strchr(start, '\'');
    }
    if (!end) {
        return;
    }
    *end = '\0';

    /* Trim leading/trailing spaces */
    while (*start == ' ') {
        start++;
    }
    size_t tlen = strlen(start);
    while (tlen > 0 && (start[tlen - 1] == ' ' || start[tlen - 1] == '-')) {
        start[--tlen] = '\0';
    }

    if (start[0] != '\0') {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        if (strcmp(s_status.icy_title, start) != 0) {
            snprintf(s_status.icy_title, sizeof(s_status.icy_title), "%s", start);
            ESP_LOGI(TAG, "ICY Now Playing: %s", s_status.icy_title);
        }
        xSemaphoreGive(s_state_lock);
    }
}

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    http_hdr_ctx_t *ctx = (http_hdr_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && ctx && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            ctx->metaint = atoi(evt->header_value);
        } else if (strcasecmp(evt->header_key, "icy-br") == 0) {
            ctx->br_kbps = atoi(evt->header_value);
        } else if (strcasecmp(evt->header_key, "icy-name") == 0) {
            snprintf(ctx->server_name, sizeof(ctx->server_name), "%s", evt->header_value);
        } else if (strcasecmp(evt->header_key, "content-type") == 0) {
            if (strcasestr(evt->header_value, "aac") || strcasestr(evt->header_value, "mp4")) {
                ctx->codec_hint = STREAM_CODEC_AAC;
            } else if (strcasestr(evt->header_value, "mpeg") || strcasestr(evt->header_value, "mp3")) {
                ctx->codec_hint = STREAM_CODEC_MP3;
            } else if (strcasestr(evt->header_value, "scpls") ||
                       strcasestr(evt->header_value, "mpegurl") ||
                       strcasestr(evt->header_value, "x-pls")) {
                ctx->is_playlist = true;
            }
        }
    }
    return ESP_OK;
}

static bool write_stream_with_abort(const uint8_t *data, size_t len, uint32_t my_epoch)
{
    size_t offset = 0;
    while (offset < len) {
        if (s_epoch != my_epoch || !s_desired_play) {
            return false;
        }
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        size_t written = ring_write(&s_stream_ring, data + offset, len - offset);
        offset += written;
        if (offset < len) {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
    return true;
}

/* Task 1 (Core 0): HTTP / HTTPS Icecast/Shoutcast Stream Fetcher */
static void http_fetcher_task(void *arg)
{
    (void)arg;
    uint8_t *rx_chunk = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rx_chunk) {
        rx_chunk = (uint8_t *)malloc(4096);
    }
    char meta_buf[4081];

    while (true) {
        if (!s_desired_play || s_paused) {
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }
        if (!wifi_manager_is_connected()) {
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            if (s_desired_play) {
                s_status.state = PLAYER_STATE_CONNECTING;
                snprintf(s_status.error_msg, sizeof(s_status.error_msg), "Waiting for Wi-Fi connection...");
            }
            xSemaphoreGive(s_state_lock);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        uint32_t my_epoch = s_epoch;
        char current_url[RADIO_URL_MAX_LEN] = {0};

        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        snprintf(current_url, sizeof(current_url), "%s", s_status.stream_url);
        s_status.state = PLAYER_STATE_CONNECTING;
        s_status.error_msg[0] = '\0';
        xSemaphoreGive(s_state_lock);

        if (current_url[0] == '\0') {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        /* Allow up to 2 hops for playlist (.pls / .m3u) resolution */
        for (int pl_hop = 0; pl_hop < 2 && s_epoch == my_epoch && s_desired_play; pl_hop++) {
            http_hdr_ctx_t hdr_ctx = {0};
            hdr_ctx.is_playlist = url_has_playlist_ext(current_url);

            esp_http_client_config_t http_cfg = {
                .url = current_url,
                .event_handler = http_event_cb,
                .user_data = &hdr_ctx,
                .timeout_ms = 20000,
                .buffer_size = 4096,
                .buffer_size_tx = 1024,
                .max_redirection_count = 5,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .user_agent = "ESP32-S31-Radio/1.0",
                .keep_alive_enable = true,
            };

            esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
            if (!client) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            }

            esp_http_client_set_header(client, "Icy-MetaData", "1");
            esp_http_client_set_header(client, "Accept", "*/*");

            ESP_LOGI(TAG, "Opening radio stream: %s", current_url);
            esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
            esp_err_t err = esp_http_client_open(client, 0);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Stream open failed: %s", esp_err_to_name(err));
                xSemaphoreTake(s_state_lock, portMAX_DELAY);
                if (s_epoch == my_epoch && s_desired_play) {
                    s_status.state = PLAYER_STATE_ERROR;
                    snprintf(s_status.error_msg, sizeof(s_status.error_msg), "Connect failed (%s)", esp_err_to_name(err));
                }
                xSemaphoreGive(s_state_lock);
                esp_http_client_cleanup(client);
                vTaskDelay(pdMS_TO_TICKS(2000));
                break;
            }

            int64_t content_len = esp_http_client_fetch_headers(client);
            (void)content_len;
            int status_code = esp_http_client_get_status_code(client);

            /* Handle manual redirect if server sent Location on ICY/HTTP */
            if (status_code == 301 || status_code == 302 || status_code == 303 ||
                status_code == 307 || status_code == 308) {
                if (esp_http_client_set_redirection(client) == ESP_OK) {
                    esp_http_client_close(client);
                    err = esp_http_client_open(client, 0);
                    if (err == ESP_OK) {
                        esp_http_client_fetch_headers(client);
                        status_code = esp_http_client_get_status_code(client);
                    }
                }
            }

            if (status_code < 200 || status_code >= 300) {
                ESP_LOGW(TAG, "HTTP status %d from stream", status_code);
                xSemaphoreTake(s_state_lock, portMAX_DELAY);
                if (s_epoch == my_epoch && s_desired_play) {
                    s_status.state = PLAYER_STATE_ERROR;
                    snprintf(s_status.error_msg, sizeof(s_status.error_msg), "HTTP error %d", status_code);
                }
                xSemaphoreGive(s_state_lock);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                vTaskDelay(pdMS_TO_TICKS(2500));
                break;
            }

            /* Resolve .pls / .m3u playlist if needed */
            if (hdr_ctx.is_playlist && pl_hop == 0) {
                char pl_buf[2048] = {0};
                int r = esp_http_client_read(client, pl_buf, sizeof(pl_buf) - 1);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                if (r > 0) {
                    pl_buf[r] = '\0';
                    char resolved[RADIO_URL_MAX_LEN] = {0};
                    if (extract_url_from_playlist(pl_buf, resolved, sizeof(resolved))) {
                        ESP_LOGI(TAG, "Resolved playlist -> %s", resolved);
                        snprintf(current_url, sizeof(current_url), "%s", resolved);
                        continue;
                    }
                }
                xSemaphoreTake(s_state_lock, portMAX_DELAY);
                s_status.state = PLAYER_STATE_ERROR;
                snprintf(s_status.error_msg, sizeof(s_status.error_msg), "Could not parse playlist URL");
                xSemaphoreGive(s_state_lock);
                vTaskDelay(pdMS_TO_TICKS(2500));
                break;
            }

            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            if (hdr_ctx.codec_hint != STREAM_CODEC_UNKNOWN) {
                s_status.codec = hdr_ctx.codec_hint;
            }
            if (hdr_ctx.br_kbps > 0) {
                s_status.bitrate_kbps = (uint32_t)hdr_ctx.br_kbps;
            }
            if (hdr_ctx.server_name[0] != '\0') {
                snprintf(s_status.server_name, sizeof(s_status.server_name), "%s", hdr_ctx.server_name);
            }
            s_status.state = PLAYER_STATE_BUFFERING;
            xSemaphoreGive(s_state_lock);

            ESP_LOGI(TAG, "Stream connected (codec_hint=%d, icy-metaint=%d, br=%d kbps)",
                     (int)hdr_ctx.codec_hint, hdr_ctx.metaint, hdr_ctx.br_kbps);

            int metaint = hdr_ctx.metaint;
            int bytes_until_meta = metaint;
            int meta_remaining = -1; /* -1 = audio mode, 0 = reading len byte, >0 = reading meta bytes */
            size_t meta_pos = 0;
            size_t meta_total = 0;

            bool burst_paused = false;
            esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
            while (s_epoch == my_epoch && s_desired_play && !s_paused) {
                size_t buffered = ring_available_read(&s_stream_ring);
                if (!burst_paused && buffered >= STREAM_BURST_HIGH_BYTES) {
                    burst_paused = true;
                } else if (burst_paused && buffered <= STREAM_BURST_LOW_BYTES) {
                    burst_paused = false;
                }

                if (burst_paused) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }

                int r = esp_http_client_read(client, (char *)rx_chunk, 4096);
                if (r <= 0) {
                    ESP_LOGW(TAG, "Stream read ended (%d), reconnecting...", r);
                    break;
                }

                xSemaphoreTake(s_state_lock, portMAX_DELAY);
                s_status.bytes_received += (uint64_t)r;
                xSemaphoreGive(s_state_lock);

                if (metaint <= 0) {
                    if (!write_stream_with_abort(rx_chunk, (size_t)r, my_epoch)) {
                        break;
                    }
                    continue;
                }

                /* Demux inline ICY metadata cleanly */
                int idx = 0;
                while (idx < r) {
                    if (meta_remaining == -1) {
                        int avail = r - idx;
                        int take = avail < bytes_until_meta ? avail : bytes_until_meta;
                        if (take > 0) {
                            if (!write_stream_with_abort(rx_chunk + idx, (size_t)take, my_epoch)) {
                                idx = r;
                                break;
                            }
                            idx += take;
                            bytes_until_meta -= take;
                        }
                        if (bytes_until_meta == 0) {
                            meta_remaining = 0;
                        }
                    } else if (meta_remaining == 0) {
                        uint8_t len_blocks = rx_chunk[idx++];
                        meta_total = (size_t)len_blocks * 16U;
                        meta_pos = 0;
                        if (meta_total == 0) {
                            meta_remaining = -1;
                            bytes_until_meta = metaint;
                        } else {
                            meta_remaining = (int)meta_total;
                        }
                    } else {
                        int avail = r - idx;
                        int take = avail < meta_remaining ? avail : meta_remaining;
                        if (meta_pos + (size_t)take < sizeof(meta_buf)) {
                            memcpy(meta_buf + meta_pos, rx_chunk + idx, (size_t)take);
                        }
                        meta_pos += (size_t)take;
                        idx += take;
                        meta_remaining -= take;
                        if (meta_remaining == 0) {
                            parse_icy_metadata(meta_buf, meta_pos < sizeof(meta_buf) ? meta_pos : (sizeof(meta_buf) - 1));
                            meta_remaining = -1;
                            bytes_until_meta = metaint;
                        }
                    }
                }
            }

            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            if (s_epoch == my_epoch && s_desired_play && !s_paused) {
                vTaskDelay(pdMS_TO_TICKS(1200));
            }
            break;
        }
    }
}

static stream_codec_t probe_bitstream_codec(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == 0xFF && (buf[i + 1] & 0xF0) == 0xF0) {
            uint8_t layer = (buf[i + 1] >> 1) & 0x03;
            /* ADTS AAC always has layer == 00; MP3 Layer I/II/III has layer == 01/10/11 */
            if (layer == 0x00 && (buf[i + 1] & 0x06) == 0x00) {
                return STREAM_CODEC_AAC;
            }
            if (layer != 0x00 && (buf[i + 1] & 0xE0) == 0xE0) {
                return STREAM_CODEC_MP3;
            }
        }
    }
    return STREAM_CODEC_MP3;
}

static size_t resample_and_push_pcm(resampler_state_t *rs,
                                    const int16_t *in_pcm,
                                    size_t frames_in,
                                    int channels,
                                    uint32_t sample_rate,
                                    int16_t *out_scratch,
                                    size_t out_scratch_frames,
                                    uint32_t my_epoch)
{
    if (frames_in == 0 || sample_rate < 8000 || sample_rate > 96000) {
        return 0;
    }

    if (!rs->initialized || rs->in_rate != sample_rate) {
        rs->in_rate = sample_rate;
        rs->phase_q16 = 0;
        rs->prev_l = in_pcm[0];
        rs->prev_r = (channels >= 2) ? in_pcm[1] : in_pcm[0];
        rs->initialized = true;
    }

    /* Compute VU peak meters */
    int32_t peak_l = 0;
    int32_t peak_r = 0;
    for (size_t i = 0; i < frames_in; i++) {
        int32_t l = (channels >= 2) ? in_pcm[i * 2] : in_pcm[i];
        int32_t r = (channels >= 2) ? in_pcm[i * 2 + 1] : l;
        if (l < 0) l = -l;
        if (r < 0) r = -r;
        if (l > peak_l) peak_l = l;
        if (r > peak_r) peak_r = r;
    }
    uint8_t vu_l = (uint8_t)((peak_l * 100) / 32768);
    uint8_t vu_r = (uint8_t)((peak_r * 100) / 32768);
    if (vu_l > 100) vu_l = 100;
    if (vu_r > 100) vu_r = 100;

    /* Determine target digital gain in Q15 (32768 = 1.0x) */
    int32_t target_gain_q15 = 32768;
    if (s_muted || s_volume_pct == 0) {
        target_gain_q15 = 0;
    } else if (!bt_a2dp_peer_supports_abs_vol()) {
        /* Perceptual quadratic volume curve when Bluetooth sink lacks AVRCP Absolute Volume */
        uint32_t v = s_volume_pct;
        target_gain_q15 = (int32_t)((v * v * 32768U) / 10000U);
    }

    uint32_t step_q16 = (uint32_t)(((uint64_t)sample_rate << 16) / TARGET_SAMPLE_RATE);
    if (s_pcm_primed) {
        size_t pcm_used = ring_available_read(&s_pcm_ring);
        if (pcm_used < PCM_PLL_LOW_BYTES) {
            /* Micro-PLL (-0.15%, ~2.5 cents): silently rebuilds cushion & cancels clock drift */
            step_q16 = (uint32_t)(((uint64_t)step_q16 * 9985ULL) / 10000ULL);
        } else if (pcm_used > PCM_PLL_HIGH_BYTES) {
            /* Micro-PLL (+0.15%): gently bleeds excess if studio clock runs slightly fast */
            step_q16 = (uint32_t)(((uint64_t)step_q16 * 10015ULL) / 10000ULL);
        }
    }
    size_t out_idx = 0;
    size_t total_written_bytes = 0;

    while (true) {
        uint32_t idx = rs->phase_q16 >> 16;
        uint32_t frac = rs->phase_q16 & 0xFFFFU;
        if (idx >= frames_in) {
            rs->phase_q16 -= (uint32_t)(frames_in << 16);
            int16_t last_l = (channels >= 2) ? in_pcm[(frames_in - 1) * 2] : in_pcm[frames_in - 1];
            int16_t last_r = (channels >= 2) ? in_pcm[(frames_in - 1) * 2 + 1] : last_l;
            rs->prev_l = last_l;
            rs->prev_r = last_r;
            break;
        }

        int32_t s0_l, s0_r, s1_l, s1_r;
        if (idx == 0) {
            s0_l = rs->prev_l;
            s0_r = rs->prev_r;
        } else {
            s0_l = (channels >= 2) ? in_pcm[(idx - 1) * 2] : in_pcm[idx - 1];
            s0_r = (channels >= 2) ? in_pcm[(idx - 1) * 2 + 1] : s0_l;
        }
        s1_l = (channels >= 2) ? in_pcm[idx * 2] : in_pcm[idx];
        s1_r = (channels >= 2) ? in_pcm[idx * 2 + 1] : s1_l;

        int32_t samp_l = s0_l + (((s1_l - s0_l) * (int32_t)frac) >> 16);
        int32_t samp_r = s0_r + (((s1_r - s0_r) * (int32_t)frac) >> 16);

        /* Smooth single-sample gain ramp to eliminate volume-step zipper noise */
        if (s_current_gain_q15 < target_gain_q15) {
            s_current_gain_q15 += 16;
            if (s_current_gain_q15 > target_gain_q15) s_current_gain_q15 = target_gain_q15;
        } else if (s_current_gain_q15 > target_gain_q15) {
            s_current_gain_q15 -= 16;
            if (s_current_gain_q15 < target_gain_q15) s_current_gain_q15 = target_gain_q15;
        }

        if (s_current_gain_q15 != 32768) {
            samp_l = (samp_l * s_current_gain_q15) >> 15;
            samp_r = (samp_r * s_current_gain_q15) >> 15;
        }
        if (samp_l > 32767) samp_l = 32767;
        if (samp_l < -32768) samp_l = -32768;
        if (samp_r > 32767) samp_r = 32767;
        if (samp_r < -32768) samp_r = -32768;

        out_scratch[out_idx * 2] = (int16_t)samp_l;
        out_scratch[out_idx * 2 + 1] = (int16_t)samp_r;
        out_idx++;
        rs->phase_q16 += step_q16;

        if (out_idx >= out_scratch_frames) {
            size_t bytes_to_push = out_idx * 4U;
            size_t pushed = 0;
            while (pushed < bytes_to_push && s_epoch == my_epoch && s_desired_play) {
                size_t space = ring_available_write(&s_pcm_ring) & ~3U;
                if (space == 0) {
                    vTaskDelay(pdMS_TO_TICKS(8));
                    continue;
                }
                size_t chunk = (bytes_to_push - pushed) < space ? (bytes_to_push - pushed) : space;
                chunk &= ~3U;
                pushed += ring_write(&s_pcm_ring, ((const uint8_t *)out_scratch) + pushed, chunk);
            }
            total_written_bytes += pushed;
            out_idx = 0;
        }
    }

    if (out_idx > 0 && s_epoch == my_epoch && s_desired_play) {
        size_t bytes_to_push = out_idx * 4U;
        size_t pushed = 0;
        while (pushed < bytes_to_push && s_epoch == my_epoch && s_desired_play) {
            size_t space = ring_available_write(&s_pcm_ring) & ~3U;
            if (space == 0) {
                vTaskDelay(pdMS_TO_TICKS(8));
                continue;
            }
            size_t chunk = (bytes_to_push - pushed) < space ? (bytes_to_push - pushed) : space;
            chunk &= ~3U;
            pushed += ring_write(&s_pcm_ring, ((const uint8_t *)out_scratch) + pushed, chunk);
        }
        total_written_bytes += pushed;
    }

    size_t prime_target = s_pcm_rebuffering ? PCM_REBUFFER_BYTES : PCM_PREBUFFER_BYTES;
    if (!s_pcm_primed && ring_available_read(&s_pcm_ring) >= prime_target) {
        s_pcm_primed = true;
        s_pcm_rebuffering = false;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_status.vu_left = s_muted ? 0 : vu_l;
    s_status.vu_right = s_muted ? 0 : vu_r;
    if (s_pcm_primed && s_desired_play && !s_paused) {
        s_status.state = PLAYER_STATE_PLAYING;
    }
    xSemaphoreGive(s_state_lock);

    return total_written_bytes;
}

/* Task 2 (Core 1): Real-time MP3 / HE-AAC Decoder + 44.1 kHz Resampler */
static void audio_decoder_task(void *arg)
{
    (void)arg;
    uint8_t *inbuf = (uint8_t *)heap_caps_malloc(DEC_INBUF_SIZE, MALLOC_CAP_8BIT);
    int16_t *raw_pcm = (int16_t *)heap_caps_malloc(4096 * sizeof(int16_t), MALLOC_CAP_8BIT);
    int16_t *resamp_pcm = (int16_t *)heap_caps_malloc(2048 * 2 * sizeof(int16_t), MALLOC_CAP_8BIT);
    mp3dec_t *mp3d = (mp3dec_t *)heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_8BIT);

    mp3dec_init(mp3d);
    HAACDecoder aacd = AACInitDecoder();
    resampler_state_t rs = {0};

    size_t fill = 0;
    size_t skip_bytes = 0;
    uint32_t local_epoch = 0xFFFFFFFFU;
    int consecutive_errors = 0;
    bool stream_primed = false;

    while (true) {
        if (!s_desired_play || s_paused) {
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_status.vu_left = 0;
            s_status.vu_right = 0;
            xSemaphoreGive(s_state_lock);
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        if (local_epoch != s_epoch) {
            local_epoch = s_epoch;
            fill = 0;
            skip_bytes = 0;
            consecutive_errors = 0;
            stream_primed = false;
            memset(&rs, 0, sizeof(rs));
            mp3dec_init(mp3d);
            if (aacd) {
                AACFlushCodec(aacd);
            } else {
                aacd = AACInitDecoder();
            }
        }

        if (!stream_primed) {
            if (ring_available_read(&s_stream_ring) < STREAM_PREBUFFER_BYTES) {
                vTaskDelay(pdMS_TO_TICKS(25));
                continue;
            }
            stream_primed = true;
        }

        /* Watermark pacing: yield cleanly when PCM SRAM buffer is >= 75% full */
        if (ring_available_read(&s_pcm_ring) >= PCM_HIGH_WATERMARK) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        /* Drain remaining oversized ID3v2 header bytes if any */
        if (skip_bytes > 0) {
            uint8_t drain[512];
            size_t want = skip_bytes < sizeof(drain) ? skip_bytes : sizeof(drain);
            size_t got = ring_read(&s_stream_ring, drain, want);
            skip_bytes -= got;
            if (got == 0) {
                vTaskDelay(pdMS_TO_TICKS(15));
            }
            continue;
        }

        /* Top up linear decode buffer from PSRAM stream ringbuffer */
        if (fill < DEC_INBUF_SIZE) {
            size_t got = ring_read(&s_stream_ring, inbuf + fill, DEC_INBUF_SIZE - fill);
            fill += got;
        }

        if (fill < 2048) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        /* Strip leading ID3v2 tag cleanly */
        if (fill >= 10 && memcmp(inbuf, "ID3", 3) == 0) {
            size_t tag_len = 10U +
                             (((size_t)(inbuf[6] & 0x7F)) << 21) +
                             (((size_t)(inbuf[7] & 0x7F)) << 14) +
                             (((size_t)(inbuf[8] & 0x7F)) << 7) +
                             ((size_t)(inbuf[9] & 0x7F));
            if (tag_len <= fill) {
                memmove(inbuf, inbuf + tag_len, fill - tag_len);
                fill -= tag_len;
            } else {
                skip_bytes = tag_len - fill;
                fill = 0;
            }
            continue;
        }

        stream_codec_t codec;
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        codec = s_status.codec;
        xSemaphoreGive(s_state_lock);

        if (codec == STREAM_CODEC_UNKNOWN) {
            codec = probe_bitstream_codec(inbuf, fill);
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_status.codec = codec;
            xSemaphoreGive(s_state_lock);
        }

        if (codec == STREAM_CODEC_AAC) {
            int sync = AACFindSyncWord(inbuf, (int)fill);
            if (sync < 0) {
                /* Keep last byte in case syncword straddles boundary */
                inbuf[0] = inbuf[fill - 1];
                fill = 1;
                continue;
            }
            if (sync > 0) {
                memmove(inbuf, inbuf + sync, fill - (size_t)sync);
                fill -= (size_t)sync;
                if (fill < 1536) {
                    continue;
                }
            }

            unsigned char *read_ptr = inbuf;
            int bytes_left = (int)fill;
            int err = AACDecode(aacd, &read_ptr, &bytes_left, raw_pcm);
            size_t consumed = (size_t)(read_ptr - inbuf);

            if (err == ERR_AAC_INDATA_UNDERFLOW) {
                if (fill >= DEC_INBUF_SIZE) {
                    memmove(inbuf, inbuf + 1, fill - 1);
                    fill -= 1;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                continue;
            } else if (err != ERR_AAC_NONE) {
                size_t step = consumed > 0 ? consumed : 1;
                if (step > fill) step = fill;
                memmove(inbuf, inbuf + step, fill - step);
                fill -= step;
                if (++consecutive_errors > 40) {
                    /* Stream might actually be MP3 mislabeled; switch codec */
                    consecutive_errors = 0;
                    AACFlushCodec(aacd);
                    xSemaphoreTake(s_state_lock, portMAX_DELAY);
                    s_status.codec = STREAM_CODEC_MP3;
                    xSemaphoreGive(s_state_lock);
                }
                continue;
            }

            consecutive_errors = 0;
            if (consumed > 0 && consumed <= fill) {
                memmove(inbuf, inbuf + consumed, fill - consumed);
                fill -= consumed;
            }

            AACFrameInfo fi = {0};
            AACGetLastFrameInfo(aacd, &fi);
            if (fi.outputSamps > 0 && fi.nChans >= 1 && fi.sampRateOut > 0) {
                size_t frames = (size_t)fi.outputSamps / (size_t)fi.nChans;
                xSemaphoreTake(s_state_lock, portMAX_DELAY);
                s_status.sample_rate_in = (uint32_t)fi.sampRateOut;
                s_status.channels_in = (uint8_t)fi.nChans;
                if (fi.bitRate > 0 && s_status.bitrate_kbps == 0) {
                    s_status.bitrate_kbps = (uint32_t)(fi.bitRate / 1000);
                }
                s_status.frames_decoded++;
                xSemaphoreGive(s_state_lock);

                resample_and_push_pcm(&rs, raw_pcm, frames, fi.nChans,
                                      (uint32_t)fi.sampRateOut, resamp_pcm, 2048, local_epoch);
                if ((s_status.frames_decoded & 1U) == 0) {
                    vTaskDelay(1);
                }
            }
        } else {
            mp3dec_frame_info_t info = {0};
            int samples = mp3dec_decode_frame(mp3d, inbuf, (int)fill, raw_pcm, &info);
            if (info.frame_bytes > 0 && (size_t)info.frame_bytes <= fill) {
                memmove(inbuf, inbuf + info.frame_bytes, fill - (size_t)info.frame_bytes);
                fill -= (size_t)info.frame_bytes;
            } else if (samples == 0) {
                memmove(inbuf, inbuf + 1, fill - 1);
                fill -= 1;
            }

            if (samples > 0 && info.channels >= 1 && info.hz > 0) {
                consecutive_errors = 0;
                xSemaphoreTake(s_state_lock, portMAX_DELAY);
                s_status.sample_rate_in = (uint32_t)info.hz;
                s_status.channels_in = (uint8_t)info.channels;
                if (info.bitrate_kbps > 0) {
                    s_status.bitrate_kbps = (uint32_t)info.bitrate_kbps;
                }
                s_status.frames_decoded++;
                xSemaphoreGive(s_state_lock);

                resample_and_push_pcm(&rs, raw_pcm, (size_t)samples, info.channels,
                                      (uint32_t)info.hz, resamp_pcm, 2048, local_epoch);
                if ((s_status.frames_decoded & 1U) == 0) {
                    vTaskDelay(1);
                }
            } else if (++consecutive_errors > 80) {
                consecutive_errors = 0;
                xSemaphoreTake(s_state_lock, portMAX_DELAY);
                s_status.codec = STREAM_CODEC_AAC;
                xSemaphoreGive(s_state_lock);
            }
        }
    }
}

int32_t audio_pipeline_a2dp_pcm_cb(uint8_t *data, int32_t len)
{
    static uint32_t s_empty_streak = 0;

    if (!data || len <= 0) {
        return 0;
    }
    if (!s_desired_play || s_paused || !s_pcm_primed) {
        s_empty_streak = 0;
        memset(data, 0, (size_t)len);
        return len;
    }

    size_t aligned_len = ((size_t)len) & ~3U;
    size_t got = ring_read(&s_pcm_ring, data, aligned_len);
    if (got < aligned_len) {
        memset(data + got, 0, (size_t)len - got);
        s_status.underruns++;
        if (got == 0 && ++s_empty_streak >= 8) {
            /* Cleanly re-prime PCM buffer to ~2.0 s instead of sputtering at 0% */
            s_pcm_rebuffering = true;
            s_pcm_primed = false;
            s_empty_streak = 0;
        }
    } else {
        if (got < (size_t)len) {
            memset(data + got, 0, (size_t)len - got);
        }
        s_empty_streak = 0;
    }
    return len;
}

static void start_stream_internal(int station_index, const char *url, const char *name)
{
    s_desired_play = false;
    s_paused = false;
    s_pcm_primed = false;
    s_pcm_rebuffering = false;
    s_epoch++;
    vTaskDelay(pdMS_TO_TICKS(30));
    ring_clear(&s_stream_ring);
    ring_clear(&s_pcm_ring);

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_status.state = PLAYER_STATE_CONNECTING;
    s_status.codec = STREAM_CODEC_UNKNOWN;
    s_status.station_index = station_index;
    snprintf(s_status.station_name, sizeof(s_status.station_name), "%s", name ? name : "Custom Stream");
    snprintf(s_status.stream_url, sizeof(s_status.stream_url), "%s", url);
    s_status.server_name[0] = '\0';
    s_status.icy_title[0] = '\0';
    s_status.error_msg[0] = '\0';
    s_status.sample_rate_in = 0;
    s_status.channels_in = 0;
    s_status.bitrate_kbps = 0;
    s_status.bytes_received = 0;
    s_status.frames_decoded = 0;
    s_status.underruns = 0;
    s_status.vu_left = 0;
    s_status.vu_right = 0;
    xSemaphoreGive(s_state_lock);

    s_desired_play = true;
}

esp_err_t audio_pipeline_init(void)
{
    s_state_lock = xSemaphoreCreateMutex();
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = PLAYER_STATE_STOPPED;
    s_status.station_index = 0;

    ESP_ERROR_CHECK(ring_init(&s_stream_ring, STREAM_RING_BYTES, true));
    ESP_ERROR_CHECK(ring_init(&s_pcm_ring, PCM_RING_BYTES, true));

    uint8_t vol = 75;
    bool muted = false;
    int32_t active = 0;
    int32_t count = 0;
    radio_config_get_volume(&vol, &muted);
    radio_config_get_playback_prefs(&active, &count, NULL);

    s_volume_pct = vol;
    s_muted = muted;
    s_status.station_index = active;
    radio_station_t st = {0};
    if (count > 0 && radio_config_get_station((int)active, &st)) {
        snprintf(s_status.station_name, sizeof(s_status.station_name), "%s", st.name);
        snprintf(s_status.stream_url, sizeof(s_status.stream_url), "%s", st.url);
    }

    /* Core 0: Wi-Fi/LwIP/BT_CTRL/Bluedroid. Core 1: I/O-bound radio_http (prio 7) > CPU-bound radio_dec (prio 5) & httpd (prio 5) */
    xTaskCreatePinnedToCore(http_fetcher_task, "radio_http", 16384, NULL, 7, NULL, 1);
    xTaskCreatePinnedToCore(audio_decoder_task, "radio_dec", 24576, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "Audio pipeline ready (2 MB PSRAM stream + 2 MB PSRAM PCM with micro-PLL & total-buffer coex)");
    return ESP_OK;
}

esp_err_t audio_pipeline_play_station(int station_index)
{
    radio_station_t st = {0};
    if (!radio_config_get_station(station_index, &st)) {
        return ESP_ERR_INVALID_ARG;
    }
    bool autoplay = true;
    radio_config_get_playback_prefs(NULL, NULL, &autoplay);
    radio_config_set_playback_prefs(station_index, autoplay);
    start_stream_internal(station_index, st.url, st.name);
    return ESP_OK;
}

esp_err_t audio_pipeline_play_url(const char *url, const char *display_name)
{
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    start_stream_internal(-1, url, (display_name && display_name[0]) ? display_name : "Direct Stream");
    return ESP_OK;
}

esp_err_t audio_pipeline_pause(void)
{
    if (!s_desired_play) {
        return ESP_OK;
    }
    s_paused = true;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_status.state = PLAYER_STATE_PAUSED;
    s_status.vu_left = 0;
    s_status.vu_right = 0;
    xSemaphoreGive(s_state_lock);
    return ESP_OK;
}

esp_err_t audio_pipeline_resume(void)
{
    if (!s_desired_play) {
        int32_t active = 0;
        radio_config_get_playback_prefs(&active, NULL, NULL);
        return audio_pipeline_play_station((int)active);
    }
    if (s_paused) {
        s_epoch++;
        s_pcm_primed = false;
        ring_clear(&s_stream_ring);
        ring_clear(&s_pcm_ring);
        s_paused = false;
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_status.state = PLAYER_STATE_CONNECTING;
        xSemaphoreGive(s_state_lock);
    }
    return ESP_OK;
}

esp_err_t audio_pipeline_toggle_play_pause(void)
{
    if (!s_desired_play || s_paused) {
        return audio_pipeline_resume();
    }
    return audio_pipeline_pause();
}

esp_err_t audio_pipeline_stop(void)
{
    s_desired_play = false;
    s_paused = false;
    s_pcm_primed = false;
    s_pcm_rebuffering = false;
    s_epoch++;
    ring_clear(&s_stream_ring);
    ring_clear(&s_pcm_ring);

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_status.state = PLAYER_STATE_STOPPED;
    s_status.icy_title[0] = '\0';
    s_status.vu_left = 0;
    s_status.vu_right = 0;
    xSemaphoreGive(s_state_lock);
    return ESP_OK;
}

esp_err_t audio_pipeline_next_station(void)
{
    int32_t active = 0;
    int32_t count = 0;
    radio_config_get_playback_prefs(&active, &count, NULL);
    if (count <= 0) {
        return ESP_FAIL;
    }
    int next = (int)((active + 1) % count);
    return audio_pipeline_play_station(next);
}

esp_err_t audio_pipeline_prev_station(void)
{
    int32_t active = 0;
    int32_t count = 0;
    radio_config_get_playback_prefs(&active, &count, NULL);
    if (count <= 0) {
        return ESP_FAIL;
    }
    int prev = (int)((active - 1 + count) % count);
    return audio_pipeline_play_station(prev);
}

void audio_pipeline_set_volume(uint8_t volume_pct, bool muted)
{
    if (volume_pct > 100) {
        volume_pct = 100;
    }
    s_volume_pct = volume_pct;
    s_muted = muted;
}

void audio_pipeline_get_status(audio_pipeline_status_t *out)
{
    if (!out) {
        return;
    }
    size_t s_used = ring_available_read(&s_stream_ring);
    size_t p_used = ring_available_read(&s_pcm_ring);

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_status.stream_buf_pct = (uint8_t)((s_used * 100U) / STREAM_RING_BYTES);
    s_status.pcm_buf_pct = (uint8_t)((p_used * 100U) / PCM_RING_BYTES);
    *out = s_status;
    xSemaphoreGive(s_state_lock);
}
