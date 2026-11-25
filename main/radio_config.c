#include "radio_config.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "radio_cfg";
static const char *NVS_NS = "s31_radio";

#define RADIO_LEGACY_MAX_STATIONS 16

static radio_config_t s_cfg;
static SemaphoreHandle_t s_lock = NULL;

/* Worldwide low-bitrate (35-96 kbps) AAC / HE-AAC & low-rate MP3 streams.
 * Optimized for ESP32-S31 single-antenna 2.4 GHz Wi-Fi + Classic BT A2DP coex. */
static const radio_station_t k_default_stations[] = {
    /* North America */
    {
        .name = "KCSM Jazz 91.1 FM (San Francisco, US)",
        .url = "http://ice5.securenetsystems.net/KCSM",
        .genre = "US / 64k AAC",
    },
    {
        .name = "WBGO 88.3 FM - Public Jazz (Newark, US)",
        .url = "http://ais-sa8.cdnstream1.com/3629_64.aac",
        .genre = "US / 64k AAC+",
    },
    {
        .name = "Jazz24 - KNKX 88.5 FM (Seattle, US)",
        .url = "http://knkx-live-a.edge.audiocdn.com/6285_64k",
        .genre = "US / 64k AAC+",
    },
    {
        .name = "KUVO Jazz 89.3 FM (Denver, US)",
        .url = "http://kuvo-ice.streamguys1.com/kuvo-aac-64",
        .genre = "US / 65k AAC+",
    },
    {
        .name = "WZUM 88.1 FM - Pittsburgh Jazz (US)",
        .url = "http://pubmusic.streamguys1.com/wzum-aac",
        .genre = "US / 64k AAC+",
    },
    {
        .name = "WDCB 90.9 FM (Chicago, US)",
        .url = "http://wdcb-ice.streamguys1.com/mobile-AAC",
        .genre = "US / 96k AAC",
    },
    {
        .name = "The Jazz Groove - West (US)",
        .url = "http://west-aac-64.streamthejazzgroove.com/stream",
        .genre = "US / 64k AAC",
    },
    {
        .name = "SmoothJazz.com Global (US)",
        .url = "http://smoothjazz.cdnstream1.com/2585_64.aac",
        .genre = "US / 64k AAC+",
    },
    /* Europe */
    {
        .name = "Radio Swiss Jazz (Switzerland)",
        .url = "http://stream.srg-ssr.ch/srgssr/rsj/aac/96",
        .genre = "CH / 88k AAC",
    },
    {
        .name = "Jazz Radio - Classic Jazz (France)",
        .url = "http://jazz-wr01.ice.infomaniak.ch/jazz-wr01-64.aac",
        .genre = "FR / 64k AAC+",
    },
    {
        .name = "Jazz Radio - Lounge (France)",
        .url = "http://jazzlounge.ice.infomaniak.ch/jazzlounge-high.aac",
        .genre = "FR / 64k AAC+",
    },
    {
        .name = "NRK Jazz (Norway)",
        .url = "http://cdn0-47115-liveicecast0.dna.contentdelivery.net/jazz_aac_l",
        .genre = "NO / 63k AAC+",
    },
    {
        .name = "Jazz London Radio (UK)",
        .url = "http://radio.canstream.co.uk:8075/live.aac",
        .genre = "UK / 64k AAC+",
    },
    {
        .name = "RMC Jazz - Monte Carlo (Italy)",
        .url = "http://edge.radiomontecarlo.net/rmcweb019",
        .genre = "IT / 96k AAC",
    },
    {
        .name = "0N Jazz (Germany)",
        .url = "http://0n-jazz.radionetz.de/0n-jazz.aac",
        .genre = "DE / 64k AAC+",
    },
    {
        .name = "Klassik Radio - Dinner Jazz (Germany)",
        .url = "http://stream.klassikradio.de/dinnerjazz/aac-64",
        .genre = "DE / 64k AAC+",
    },
    {
        .name = "Sublime Soul & Jazz (Netherlands)",
        .url = "http://playerservices.streamtheworld.com/api/livestream-redirect/SUBLIMEAAC.aac",
        .genre = "NL / 96k AAC+",
    },
    {
        .name = "RFM Jazzy (Portugal)",
        .url = "http://playerservices.streamtheworld.com/api/livestream-redirect/RFM_JAZZYAAC.aac",
        .genre = "PT / 64k AAC+",
    },
    {
        .name = "RMF Smooth Jazz (Poland)",
        .url = "http://31.192.216.8/SJAZZ",
        .genre = "PL / 64k AAC+",
    },
    /* Asia-Pacific, South America & Africa */
    {
        .name = "ABC Jazz (Australia)",
        .url = "http://abc.streamguys1.com/live/abcjazz/icecast.audio",
        .genre = "AU / 56k AAC+",
    },
    {
        .name = "5MBS 99.9 FM Jazz (Adelaide, AU)",
        .url = "http://stream04.sigile.net/5mbs-64-aac",
        .genre = "AU / 64k AAC+",
    },
    {
        .name = "Digital Jazz (Brazil)",
        .url = "http://stm01.virtualcast.com.br:8190/live",
        .genre = "BR / 48k AAC+",
    },
    {
        .name = "Fine Music Radio 101.3 (Cape Town, ZA)",
        .url = "http://edge.iono.fm/xice/fmr_live_low.aac",
        .genre = "ZA / 35k AAC+",
    },
    /* SomaFM (64k AAC) & News */
    {
        .name = "SomaFM - Sonic Universe (Nu-Jazz)",
        .url = "http://ice2.somafm.com/sonicuniverse-64-aac",
        .genre = "Nu-Jazz / 64k AAC",
    },
    {
        .name = "SomaFM - Groove Salad (Ambient/Jazz)",
        .url = "http://ice4.somafm.com/groovesalad-64-aac",
        .genre = "Ambient / 64k AAC",
    },
    {
        .name = "SomaFM - Secret Agent (Spy Jazz)",
        .url = "http://ice2.somafm.com/secretagent-64-aac",
        .genre = "Lounge / 64k AAC",
    },
    {
        .name = "BBC World Service (UK)",
        .url = "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service",
        .genre = "News / 56k MP3",
    },
};

static void copy_str(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_sz, "%s", src);
}

static void load_default_stations_locked(void)
{
    int count = (int)(sizeof(k_default_stations) / sizeof(k_default_stations[0]));
    if (count > RADIO_MAX_STATIONS) {
        count = RADIO_MAX_STATIONS;
    }
    s_cfg.station_count = count;
    memset(s_cfg.stations, 0, sizeof(s_cfg.stations));
    memcpy(s_cfg.stations, k_default_stations, (size_t)count * sizeof(radio_station_t));
    if (s_cfg.active_station < 0 || s_cfg.active_station >= s_cfg.station_count) {
        s_cfg.active_station = 0;
    }
}

static esp_err_t save_locked(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, "cfg_v5", &s_cfg, sizeof(s_cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob failed: %s", esp_err_to_name(err));
    }
    return err;
}

static bool load_legacy_blob(nvs_handle_t h, const char *key, void *buf, size_t expected_sz)
{
    size_t sz = expected_sz;
    return (nvs_get_blob(h, key, buf, &sz) == ESP_OK && sz == expected_sz);
}

esp_err_t radio_config_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.bt_auto_reconnect = true;
    copy_str(s_cfg.bt_pin, sizeof(s_cfg.bt_pin), "0000");
    s_cfg.volume = 75;
    s_cfg.muted = false;
    s_cfg.autoplay = true;
    s_cfg.active_station = 0;
    load_default_stations_locked();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t sz = sizeof(s_cfg);
        radio_config_t *loaded = (radio_config_t *)calloc(1, sizeof(radio_config_t));
        bool loaded_v5 = (loaded && nvs_get_blob(h, "cfg_v5", loaded, &sz) == ESP_OK && sz == sizeof(*loaded));
        bool loaded_old = false;
        if (!loaded_v5 && loaded) {
            const size_t legacy_sz = offsetof(radio_config_t, stations) +
                                     (size_t)RADIO_LEGACY_MAX_STATIONS * sizeof(radio_station_t);
            loaded_old = load_legacy_blob(h, "cfg_v4", loaded, legacy_sz) ||
                         load_legacy_blob(h, "cfg_v3", loaded, legacy_sz) ||
                         load_legacy_blob(h, "cfg_v2", loaded, legacy_sz) ||
                         load_legacy_blob(h, "cfg_v1", loaded, legacy_sz);
        }
        if (loaded_v5 && loaded->station_count >= 0 && loaded->station_count <= RADIO_MAX_STATIONS) {
            s_cfg = *loaded;
            if (s_cfg.volume > 100) {
                s_cfg.volume = 100;
            }
            if (s_cfg.station_count == 0) {
                s_cfg.active_station = 0;
                load_default_stations_locked();
            }
            if (s_cfg.active_station < 0 || s_cfg.active_station >= s_cfg.station_count) {
                s_cfg.active_station = 0;
            }
            ESP_LOGI(TAG, "Loaded config from NVS (%ld stations, Wi-Fi='%s', BT='%s')",
                     (long)s_cfg.station_count,
                     s_cfg.wifi_ssid[0] ? s_cfg.wifi_ssid : "<unconfigured>",
                     s_cfg.bt_has_target ? s_cfg.bt_target_name : "<none>");
        } else if (loaded_old) {
            /* Preserve Wi-Fi, BT speaker, volume, autoplay, and PIN from legacy 16-slot blob */
            memcpy(&s_cfg, loaded, offsetof(radio_config_t, stations));
            if (s_cfg.volume > 100) {
                s_cfg.volume = 100;
            }
            s_cfg.active_station = 0;
            load_default_stations_locked();
            ESP_LOGI(TAG, "Migrating legacy NVS config (Wi-Fi='%s', BT='%s') to v5 (%ld worldwide low-rate stations)",
                     s_cfg.wifi_ssid[0] ? s_cfg.wifi_ssid : "<unconfigured>",
                     s_cfg.bt_has_target ? s_cfg.bt_target_name : "<none>",
                     (long)s_cfg.station_count);
        }
        free(loaded);
        nvs_close(h);
        if (loaded_old) {
            if (save_locked() == ESP_OK) {
                nvs_handle_t hw;
                if (nvs_open(NVS_NS, NVS_READWRITE, &hw) == ESP_OK) {
                    nvs_erase_key(hw, "cfg_v4");
                    nvs_erase_key(hw, "cfg_v3");
                    nvs_erase_key(hw, "cfg_v2");
                    nvs_erase_key(hw, "cfg_v1");
                    nvs_commit(hw);
                    nvs_close(hw);
                }
            }
        }
    } else {
        ESP_LOGI(TAG, "Initializing default configuration in NVS");
        save_locked();
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void radio_config_get(radio_config_t *out)
{
    if (!out) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

void radio_config_get_bt_pin(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out, out_sz, "%s", s_cfg.bt_pin[0] ? s_cfg.bt_pin : "0000");
    xSemaphoreGive(s_lock);
}

bool radio_config_get_bt_target(uint8_t out_bda[6], char *out_name, size_t name_sz, bool *out_auto_reconnect)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool has = s_cfg.bt_has_target;
    if (out_bda) {
        memcpy(out_bda, s_cfg.bt_target_bda, 6);
    }
    if (out_name && name_sz > 0) {
        snprintf(out_name, name_sz, "%s", s_cfg.bt_target_name);
    }
    if (out_auto_reconnect) {
        *out_auto_reconnect = s_cfg.bt_auto_reconnect;
    }
    xSemaphoreGive(s_lock);
    return has;
}

void radio_config_get_volume(uint8_t *out_vol, bool *out_muted)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (out_vol) {
        *out_vol = s_cfg.volume;
    }
    if (out_muted) {
        *out_muted = s_cfg.muted;
    }
    xSemaphoreGive(s_lock);
}

void radio_config_get_bt_options(bool *out_auto_reconnect, char *out_pin, size_t pin_sz)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (out_auto_reconnect) {
        *out_auto_reconnect = s_cfg.bt_auto_reconnect;
    }
    if (out_pin && pin_sz > 0) {
        snprintf(out_pin, pin_sz, "%s", s_cfg.bt_pin);
    }
    xSemaphoreGive(s_lock);
}

void radio_config_get_playback_prefs(int32_t *out_active, int32_t *out_count, bool *out_autoplay)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (out_active) {
        *out_active = s_cfg.active_station;
    }
    if (out_count) {
        *out_count = s_cfg.station_count;
    }
    if (out_autoplay) {
        *out_autoplay = s_cfg.autoplay;
    }
    xSemaphoreGive(s_lock);
}

bool radio_config_get_station(int index, radio_station_t *out_station)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = (index >= 0 && index < s_cfg.station_count);
    if (ok && out_station) {
        *out_station = s_cfg.stations[index];
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool radio_config_get_wifi(char *out_ssid, size_t ssid_sz, char *out_pass, size_t pass_sz)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool configured = (s_cfg.wifi_ssid[0] != '\0');
    if (out_ssid && ssid_sz > 0) {
        snprintf(out_ssid, ssid_sz, "%s", s_cfg.wifi_ssid);
    }
    if (out_pass && pass_sz > 0) {
        snprintf(out_pass, pass_sz, "%s", s_cfg.wifi_pass);
    }
    xSemaphoreGive(s_lock);
    return configured;
}

esp_err_t radio_config_set_wifi(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0' || strlen(ssid) >= RADIO_WIFI_SSID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy_str(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), ssid);
    copy_str(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), password ? password : "");
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t radio_config_clear_wifi(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.wifi_ssid[0] = '\0';
    s_cfg.wifi_pass[0] = '\0';
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t radio_config_set_bt_target(const uint8_t bda[6], const char *name)
{
    if (!bda) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_cfg.bt_target_bda, bda, 6);
    s_cfg.bt_has_target = true;
    if (name && name[0] != '\0') {
        copy_str(s_cfg.bt_target_name, sizeof(s_cfg.bt_target_name), name);
    } else if (s_cfg.bt_target_name[0] == '\0') {
        snprintf(s_cfg.bt_target_name, sizeof(s_cfg.bt_target_name),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    }
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t radio_config_clear_bt_target(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_cfg.bt_target_bda, 0, sizeof(s_cfg.bt_target_bda));
    s_cfg.bt_has_target = false;
    s_cfg.bt_target_name[0] = '\0';
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t radio_config_set_bt_options(bool auto_reconnect, const char *pin)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.bt_auto_reconnect = auto_reconnect;
    if (pin && pin[0] != '\0') {
        copy_str(s_cfg.bt_pin, sizeof(s_cfg.bt_pin), pin);
    }
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t radio_config_set_volume(uint8_t volume, bool muted)
{
    if (volume > 100) {
        volume = 100;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.volume = volume;
    s_cfg.muted = muted;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t radio_config_set_playback_prefs(int32_t active_station, bool autoplay)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (active_station >= 0 && active_station < s_cfg.station_count) {
        s_cfg.active_station = active_station;
    }
    s_cfg.autoplay = autoplay;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t radio_config_add_station(const char *name, const char *url, const char *genre, int *out_index)
{
    if (!name || name[0] == '\0' || !url || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cfg.station_count >= RADIO_MAX_STATIONS) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    int idx = (int)s_cfg.station_count;
    copy_str(s_cfg.stations[idx].name, sizeof(s_cfg.stations[idx].name), name);
    copy_str(s_cfg.stations[idx].url, sizeof(s_cfg.stations[idx].url), url);
    copy_str(s_cfg.stations[idx].genre, sizeof(s_cfg.stations[idx].genre),
             (genre && genre[0]) ? genre : "Custom");
    s_cfg.station_count++;
    if (out_index) {
        *out_index = idx;
    }
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t radio_config_update_station(int index, const char *name, const char *url, const char *genre)
{
    if (!name || name[0] == '\0' || !url || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (index < 0 || index >= s_cfg.station_count) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    copy_str(s_cfg.stations[index].name, sizeof(s_cfg.stations[index].name), name);
    copy_str(s_cfg.stations[index].url, sizeof(s_cfg.stations[index].url), url);
    copy_str(s_cfg.stations[index].genre, sizeof(s_cfg.stations[index].genre),
             (genre && genre[0]) ? genre : "Custom");
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t radio_config_delete_station(int index)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (index < 0 || index >= s_cfg.station_count) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = index; i + 1 < s_cfg.station_count; i++) {
        s_cfg.stations[i] = s_cfg.stations[i + 1];
    }
    s_cfg.station_count--;
    memset(&s_cfg.stations[s_cfg.station_count], 0, sizeof(radio_station_t));
    if (s_cfg.active_station >= s_cfg.station_count) {
        s_cfg.active_station = (s_cfg.station_count > 0) ? (s_cfg.station_count - 1) : 0;
    }
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t radio_config_reset_stations_default(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    load_default_stations_locked();
    s_cfg.active_station = 0;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);
    return err;
}
