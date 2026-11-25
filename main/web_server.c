#include "web_server.h"

#include "audio_pipeline.h"
#include "board_hw.h"
#include "bt_a2dp_source.h"
#include "radio_config.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "web_srv";

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

static httpd_handle_t s_server = NULL;

#define WEB_TX_CHUNK_BYTES 256

static esp_err_t send_small_chunks(httpd_req_t *req, const char *data, size_t len)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd >= 0) {
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }

    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > WEB_TX_CHUNK_BYTES) {
            chunk = WEB_TX_CHUNK_BYTES;
        }
        esp_err_t err = httpd_resp_send_chunk(req, data + offset, (ssize_t)chunk);
        if (err != ESP_OK) {
            return err;
        }
        offset += chunk;
        if (offset < len) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root, int status)
{
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc failed");
        return ESP_ERR_NO_MEM;
    }
    if (status == 400) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = send_small_chunks(req, json_str, strlen(json_str));
    free(json_str);
    return err;
}

static esp_err_t send_ok_msg(httpd_req_t *req, bool ok, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    if (msg) {
        cJSON_AddStringToObject(root, "message", msg);
    }
    return send_json(req, root, ok ? 200 : 400);
}

static cJSON *read_json_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > 4096) {
        return NULL;
    }
    char *buf = (char *)malloc(req->content_len + 1);
    if (!buf) {
        return NULL;
    }
    size_t off = 0;
    while (off < req->content_len) {
        int r = httpd_req_recv(req, buf + off, req->content_len - off);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        off += (size_t)r;
    }
    buf[off] = '\0';
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    return json;
}

static const char *state_to_str(player_state_t st)
{
    switch (st) {
    case PLAYER_STATE_STOPPED:    return "stopped";
    case PLAYER_STATE_CONNECTING: return "connecting";
    case PLAYER_STATE_BUFFERING:  return "buffering";
    case PLAYER_STATE_PLAYING:    return "playing";
    case PLAYER_STATE_PAUSED:     return "paused";
    case PLAYER_STATE_ERROR:      return "error";
    default:                      return "stopped";
    }
}

static const char *codec_to_str(stream_codec_t c)
{
    switch (c) {
    case STREAM_CODEC_MP3: return "MP3";
    case STREAM_CODEC_AAC: return "AAC";
    default:               return "AUTO";
    }
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const size_t gz_len = (size_t)(index_html_gz_end - index_html_gz_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    return send_small_chunks(req, (const char *)index_html_gz_start, gz_len);
}

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    audio_pipeline_status_t ap;
    bt_a2dp_status_t bt;
    wifi_status_t wf;
    uint8_t volume = 75;
    bool muted = false;
    bool autoplay = true;
    bool auto_reconnect = true;

    audio_pipeline_get_status(&ap);
    bt_a2dp_get_status(&bt);
    wifi_manager_get_status(&wf);
    radio_config_get_volume(&volume, &muted);
    radio_config_get_playback_prefs(NULL, NULL, &autoplay);
    radio_config_get_bt_options(&auto_reconnect, NULL, 0);

    cJSON *root = cJSON_CreateObject();

    cJSON *jp = cJSON_AddObjectToObject(root, "player");
    cJSON_AddStringToObject(jp, "state", state_to_str(ap.state));
    cJSON_AddStringToObject(jp, "codec", codec_to_str(ap.codec));
    cJSON_AddNumberToObject(jp, "station_index", ap.station_index);
    cJSON_AddStringToObject(jp, "station_name", ap.station_name);
    cJSON_AddStringToObject(jp, "stream_url", ap.stream_url);
    cJSON_AddStringToObject(jp, "server_name", ap.server_name);
    cJSON_AddStringToObject(jp, "icy_title", ap.icy_title);
    cJSON_AddStringToObject(jp, "error_msg", ap.error_msg);
    cJSON_AddNumberToObject(jp, "sample_rate", ap.sample_rate_in);
    cJSON_AddNumberToObject(jp, "channels", ap.channels_in);
    cJSON_AddNumberToObject(jp, "bitrate_kbps", ap.bitrate_kbps);
    cJSON_AddNumberToObject(jp, "stream_buf_pct", ap.stream_buf_pct);
    cJSON_AddNumberToObject(jp, "pcm_buf_pct", ap.pcm_buf_pct);
    cJSON_AddNumberToObject(jp, "vu_left", ap.vu_left);
    cJSON_AddNumberToObject(jp, "vu_right", ap.vu_right);
    cJSON_AddNumberToObject(jp, "bytes_received", (double)ap.bytes_received);
    cJSON_AddNumberToObject(jp, "frames_decoded", ap.frames_decoded);
    cJSON_AddNumberToObject(jp, "underruns", ap.underruns);
    cJSON_AddNumberToObject(jp, "volume", volume);
    cJSON_AddBoolToObject(jp, "muted", muted);
    cJSON_AddBoolToObject(jp, "autoplay", autoplay);

    cJSON *jb = cJSON_AddObjectToObject(root, "bt");
    cJSON_AddBoolToObject(jb, "initialized", bt.initialized);
    cJSON_AddBoolToObject(jb, "scanning", bt.scanning);
    cJSON_AddBoolToObject(jb, "connecting", bt.connecting);
    cJSON_AddBoolToObject(jb, "connected", bt.connected);
    cJSON_AddBoolToObject(jb, "media_started", bt.media_started);
    cJSON_AddBoolToObject(jb, "avrc_connected", bt.avrc_connected);
    cJSON_AddBoolToObject(jb, "abs_vol", bt_a2dp_peer_supports_abs_vol());
    cJSON_AddBoolToObject(jb, "auto_reconnect", auto_reconnect);
    cJSON_AddStringToObject(jb, "local_bda", bt.local_bda_str);
    cJSON_AddStringToObject(jb, "local_name", bt.local_name);
    cJSON_AddStringToObject(jb, "peer_bda", bt.peer_bda_str);
    cJSON_AddStringToObject(jb, "peer_name", bt.peer_name);
    cJSON_AddStringToObject(jb, "codec_desc", bt.codec_desc);
    cJSON_AddNumberToObject(jb, "sink_delay_ms", bt.sink_delay_tenths_ms / 10.0);

    cJSON *jw = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(jw, "sta_configured", wf.sta_configured);
    cJSON_AddBoolToObject(jw, "sta_connecting", wf.sta_connecting);
    cJSON_AddBoolToObject(jw, "sta_connected", wf.sta_connected);
    cJSON_AddStringToObject(jw, "sta_ssid", wf.sta_ssid);
    cJSON_AddStringToObject(jw, "sta_ip", wf.sta_ip);
    cJSON_AddNumberToObject(jw, "sta_rssi", wf.sta_rssi);
    cJSON_AddBoolToObject(jw, "ap_active", wf.ap_active);
    cJSON_AddStringToObject(jw, "ap_ssid", wf.ap_ssid);
    cJSON_AddStringToObject(jw, "ap_ip", wf.ap_ip);
    cJSON_AddStringToObject(jw, "mac", wf.mac_str);
    cJSON_AddStringToObject(jw, "hostname", wf.hostname);

    cJSON *js = cJSON_AddObjectToObject(root, "sys");
    cJSON_AddNumberToObject(js, "uptime_sec", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(js, "free_sram_kb", (double)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U));
    cJSON_AddNumberToObject(js, "free_psram_kb", (double)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U));
    float temp_c = 0.0f;
    if (board_hw_get_temp_c(&temp_c)) {
        cJSON_AddNumberToObject(js, "temp_c", ((int)(temp_c * 10.0f + 0.5f)) / 10.0);
    } else {
        cJSON_AddNullToObject(js, "temp_c");
    }
    cJSON_AddStringToObject(js, "board", "Waveshare ESP32-S31-Zero");
    cJSON_AddStringToObject(js, "chip", "ESP32-S31 Dual-Core RISC-V @ 320 MHz");

    return send_json(req, root, 200);
}

static cJSON *build_stations_json(void)
{
    int32_t active = 0;
    int32_t count = 0;
    bool autoplay = true;
    radio_config_get_playback_prefs(&active, &count, &autoplay);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "active_station", active);
    cJSON_AddBoolToObject(root, "autoplay", autoplay);
    cJSON_AddNumberToObject(root, "max_stations", RADIO_MAX_STATIONS);
    cJSON *arr = cJSON_AddArrayToObject(root, "stations");
    radio_station_t st = {0};
    for (int i = 0; i < count; i++) {
        if (!radio_config_get_station(i, &st)) {
            continue;
        }
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "index", i);
        cJSON_AddStringToObject(it, "name", st.name);
        cJSON_AddStringToObject(it, "url", st.url);
        cJSON_AddStringToObject(it, "genre", st.genre);
        cJSON_AddItemToArray(arr, it);
    }
    return root;
}

static esp_err_t api_stations_get_handler(httpd_req_t *req)
{
    return send_json(req, build_stations_json(), 200);
}

static esp_err_t api_stations_post_handler(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_ok_msg(req, false, "Invalid JSON payload");
    }

    const cJSON *jact = cJSON_GetObjectItemCaseSensitive(body, "action");
    const char *act = cJSON_IsString(jact) ? jact->valuestring : "";
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (strcmp(act, "add") == 0) {
        const cJSON *jn = cJSON_GetObjectItemCaseSensitive(body, "name");
        const cJSON *ju = cJSON_GetObjectItemCaseSensitive(body, "url");
        const cJSON *jg = cJSON_GetObjectItemCaseSensitive(body, "genre");
        err = radio_config_add_station(
            cJSON_IsString(jn) ? jn->valuestring : NULL,
            cJSON_IsString(ju) ? ju->valuestring : NULL,
            cJSON_IsString(jg) ? jg->valuestring : "Custom",
            NULL);
    } else if (strcmp(act, "update") == 0) {
        const cJSON *ji = cJSON_GetObjectItemCaseSensitive(body, "index");
        const cJSON *jn = cJSON_GetObjectItemCaseSensitive(body, "name");
        const cJSON *ju = cJSON_GetObjectItemCaseSensitive(body, "url");
        const cJSON *jg = cJSON_GetObjectItemCaseSensitive(body, "genre");
        if (cJSON_IsNumber(ji)) {
            err = radio_config_update_station(
                ji->valueint,
                cJSON_IsString(jn) ? jn->valuestring : NULL,
                cJSON_IsString(ju) ? ju->valuestring : NULL,
                cJSON_IsString(jg) ? jg->valuestring : "Custom");
        }
    } else if (strcmp(act, "delete") == 0) {
        const cJSON *ji = cJSON_GetObjectItemCaseSensitive(body, "index");
        if (cJSON_IsNumber(ji)) {
            err = radio_config_delete_station(ji->valueint);
        }
    } else if (strcmp(act, "reset") == 0) {
        err = radio_config_reset_stations_default();
    }

    cJSON_Delete(body);
    if (err != ESP_OK) {
        return send_ok_msg(req, false, esp_err_to_name(err));
    }

    cJSON *resp = build_stations_json();
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp, 200);
}

static esp_err_t api_player_post_handler(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_ok_msg(req, false, "Invalid JSON payload");
    }

    const cJSON *jact = cJSON_GetObjectItemCaseSensitive(body, "action");
    const char *act = cJSON_IsString(jact) ? jact->valuestring : "";
    esp_err_t err = ESP_OK;

    if (strcmp(act, "play") == 0) {
        const cJSON *ji = cJSON_GetObjectItemCaseSensitive(body, "index");
        if (cJSON_IsNumber(ji)) {
            err = audio_pipeline_play_station(ji->valueint);
        } else {
            err = audio_pipeline_resume();
        }
    } else if (strcmp(act, "play_url") == 0) {
        const cJSON *ju = cJSON_GetObjectItemCaseSensitive(body, "url");
        const cJSON *jn = cJSON_GetObjectItemCaseSensitive(body, "name");
        err = audio_pipeline_play_url(
            cJSON_IsString(ju) ? ju->valuestring : NULL,
            cJSON_IsString(jn) ? jn->valuestring : "Direct URL Stream");
    } else if (strcmp(act, "pause") == 0) {
        err = audio_pipeline_pause();
    } else if (strcmp(act, "resume") == 0) {
        err = audio_pipeline_resume();
    } else if (strcmp(act, "toggle") == 0) {
        err = audio_pipeline_toggle_play_pause();
    } else if (strcmp(act, "stop") == 0) {
        err = audio_pipeline_stop();
    } else if (strcmp(act, "next") == 0) {
        err = audio_pipeline_next_station();
    } else if (strcmp(act, "prev") == 0) {
        err = audio_pipeline_prev_station();
    } else if (strcmp(act, "volume") == 0) {
        uint8_t vol = 75;
        bool muted = false;
        radio_config_get_volume(&vol, &muted);
        const cJSON *jv = cJSON_GetObjectItemCaseSensitive(body, "volume");
        const cJSON *jm = cJSON_GetObjectItemCaseSensitive(body, "muted");
        if (cJSON_IsNumber(jv)) {
            int v = jv->valueint;
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            vol = (uint8_t)v;
        }
        if (cJSON_IsBool(jm)) {
            muted = cJSON_IsTrue(jm);
        }
        radio_config_set_volume(vol, muted);
        audio_pipeline_set_volume(vol, muted);
        bt_a2dp_set_volume(muted ? 0 : vol);
    } else if (strcmp(act, "autoplay") == 0) {
        int32_t active = 0;
        radio_config_get_playback_prefs(&active, NULL, NULL);
        const cJSON *ja = cJSON_GetObjectItemCaseSensitive(body, "autoplay");
        if (cJSON_IsBool(ja)) {
            err = radio_config_set_playback_prefs(active, cJSON_IsTrue(ja));
        }
    } else {
        err = ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(body);
    return send_ok_msg(req, err == ESP_OK, err == ESP_OK ? "ok" : esp_err_to_name(err));
}

static esp_err_t api_bt_devices_get_handler(httpd_req_t *req)
{
    bt_a2dp_status_t st;
    bt_a2dp_get_status(&st);

    bt_device_info_t disc[BT_SCAN_MAX_DEVICES];
    size_t nd = bt_a2dp_get_scan_results(disc, BT_SCAN_MAX_DEVICES);

    bt_device_info_t bond[BT_BOND_MAX_DEVICES];
    size_t nb = bt_a2dp_get_bonded_devices(bond, BT_BOND_MAX_DEVICES);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "scanning", st.scanning);
    cJSON_AddStringToObject(root, "connected_bda", st.connected ? st.peer_bda_str : "");

    cJSON *jdisc = cJSON_AddArrayToObject(root, "discovered");
    for (size_t i = 0; i < nd; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "bda", disc[i].bda_str);
        cJSON_AddStringToObject(d, "name", disc[i].name);
        cJSON_AddNumberToObject(d, "rssi", disc[i].rssi);
        cJSON_AddBoolToObject(d, "is_audio_sink", disc[i].is_audio_sink);
        cJSON_AddBoolToObject(d, "bonded", disc[i].bonded);
        cJSON_AddItemToArray(jdisc, d);
    }

    cJSON *jbond = cJSON_AddArrayToObject(root, "bonded");
    for (size_t i = 0; i < nb; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "bda", bond[i].bda_str);
        cJSON_AddStringToObject(d, "name", bond[i].name);
        cJSON_AddItemToArray(jbond, d);
    }

    return send_json(req, root, 200);
}

static esp_err_t api_bt_action_post_handler(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_ok_msg(req, false, "Invalid JSON payload");
    }

    const cJSON *jact = cJSON_GetObjectItemCaseSensitive(body, "action");
    const char *act = cJSON_IsString(jact) ? jact->valuestring : "";
    esp_err_t err = ESP_OK;

    if (strcmp(act, "scan") == 0) {
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(body, "duration");
        int dur = cJSON_IsNumber(jd) ? jd->valueint : 10;
        err = bt_a2dp_scan_start((uint8_t)dur, false);
    } else if (strcmp(act, "stop_scan") == 0) {
        err = bt_a2dp_scan_stop();
    } else if (strcmp(act, "connect") == 0) {
        const cJSON *jbda = cJSON_GetObjectItemCaseSensitive(body, "bda");
        const cJSON *jname = cJSON_GetObjectItemCaseSensitive(body, "name");
        uint8_t bda[6] = {0};
        if (cJSON_IsString(jbda) && bt_parse_bda(jbda->valuestring, bda)) {
            err = bt_a2dp_connect(bda, cJSON_IsString(jname) ? jname->valuestring : NULL);
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
    } else if (strcmp(act, "disconnect") == 0) {
        err = bt_a2dp_disconnect();
    } else if (strcmp(act, "forget") == 0) {
        const cJSON *jbda = cJSON_GetObjectItemCaseSensitive(body, "bda");
        uint8_t bda[6] = {0};
        if (cJSON_IsString(jbda) && bt_parse_bda(jbda->valuestring, bda)) {
            err = bt_a2dp_forget_device(bda);
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
    } else if (strcmp(act, "forget_all") == 0) {
        err = bt_a2dp_forget_all();
    } else if (strcmp(act, "options") == 0) {
        bool cur_ar = true;
        char cur_pin[RADIO_BT_PIN_MAX] = {0};
        radio_config_get_bt_options(&cur_ar, cur_pin, sizeof(cur_pin));
        const cJSON *jar = cJSON_GetObjectItemCaseSensitive(body, "auto_reconnect");
        const cJSON *jpin = cJSON_GetObjectItemCaseSensitive(body, "pin");
        bool ar = cJSON_IsBool(jar) ? cJSON_IsTrue(jar) : cur_ar;
        const char *pin = cJSON_IsString(jpin) ? jpin->valuestring : cur_pin;
        err = radio_config_set_bt_options(ar, pin);
    } else {
        err = ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(body);
    return send_ok_msg(req, err == ESP_OK, err == ESP_OK ? "ok" : esp_err_to_name(err));
}

static esp_err_t api_wifi_scan_get_handler(httpd_req_t *req)
{
    wifi_ap_entry_t aps[WIFI_SCAN_MAX_APS];
    size_t count = 0;
    esp_err_t err = wifi_manager_scan(aps, WIFI_SCAN_MAX_APS, &count);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON *arr = cJSON_AddArrayToObject(root, "aps");
    if (err == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "ssid", aps[i].ssid);
            cJSON_AddNumberToObject(it, "rssi", aps[i].rssi);
            cJSON_AddNumberToObject(it, "channel", aps[i].channel);
            cJSON_AddBoolToObject(it, "secure", aps[i].authmode != 0);
            cJSON_AddItemToArray(arr, it);
        }
    }
    return send_json(req, root, 200);
}

static esp_err_t api_wifi_connect_post_handler(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_ok_msg(req, false, "Invalid JSON payload");
    }

    const cJSON *jact = cJSON_GetObjectItemCaseSensitive(body, "action");
    const char *act = cJSON_IsString(jact) ? jact->valuestring : "connect";
    esp_err_t err = ESP_OK;

    if (strcmp(act, "clear") == 0) {
        err = wifi_manager_disconnect_and_clear();
    } else {
        const cJSON *jssid = cJSON_GetObjectItemCaseSensitive(body, "ssid");
        const cJSON *jpass = cJSON_GetObjectItemCaseSensitive(body, "password");
        err = wifi_manager_connect(
            cJSON_IsString(jssid) ? jssid->valuestring : NULL,
            cJSON_IsString(jpass) ? jpass->valuestring : "",
            true);
    }

    cJSON_Delete(body);
    return send_ok_msg(req, err == ESP_OK, err == ESP_OK ? "Connecting to Wi-Fi..." : esp_err_to_name(err));
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.task_priority = 5;
    config.core_id = 1;
    config.max_open_sockets = 5;
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;
    config.send_wait_timeout = 3;
    config.recv_wait_timeout = 3;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/",                 .method = HTTP_GET,  .handler = root_get_handler},
        {.uri = "/api/status",       .method = HTTP_GET,  .handler = api_status_get_handler},
        {.uri = "/api/stations",     .method = HTTP_GET,  .handler = api_stations_get_handler},
        {.uri = "/api/stations",     .method = HTTP_POST, .handler = api_stations_post_handler},
        {.uri = "/api/player",       .method = HTTP_POST, .handler = api_player_post_handler},
        {.uri = "/api/bt/devices",   .method = HTTP_GET,  .handler = api_bt_devices_get_handler},
        {.uri = "/api/bt/action",    .method = HTTP_POST, .handler = api_bt_action_post_handler},
        {.uri = "/api/wifi/scan",    .method = HTTP_GET,  .handler = api_wifi_scan_get_handler},
        {.uri = "/api/wifi/connect", .method = HTTP_POST, .handler = api_wifi_connect_post_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    ESP_LOGI(TAG, "Desktop WebUI HTTP server listening on port 80");
    return ESP_OK;
}
