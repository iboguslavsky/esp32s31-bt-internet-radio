#include "usb_cli.h"

#include "audio_pipeline.h"
#include "board_hw.h"
#include "bt_a2dp_source.h"
#include "radio_config.h"
#include "wifi_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "usb_cli";

static const char *player_state_name(player_state_t s)
{
    switch (s) {
    case PLAYER_STATE_STOPPED:    return "STOPPED";
    case PLAYER_STATE_CONNECTING: return "CONNECTING";
    case PLAYER_STATE_BUFFERING:  return "BUFFERING";
    case PLAYER_STATE_PLAYING:    return "PLAYING";
    case PLAYER_STATE_PAUSED:     return "PAUSED";
    case PLAYER_STATE_ERROR:      return "ERROR";
    default:                      return "UNKNOWN";
    }
}

static const char *codec_name(stream_codec_t c)
{
    switch (c) {
    case STREAM_CODEC_MP3: return "MP3";
    case STREAM_CODEC_AAC: return "AAC/HE-AAC";
    default:               return "Auto";
    }
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    wifi_status_t w;
    bt_a2dp_status_t b;
    audio_pipeline_status_t a;
    uint8_t volume = 75;
    bool muted = false;

    wifi_manager_get_status(&w);
    bt_a2dp_get_status(&b);
    audio_pipeline_get_status(&a);
    radio_config_get_volume(&volume, &muted);

    printf("\n=== ESP32-S31-Zero Classic Bluetooth Internet Radio ===\n");
    if (w.sta_connected) {
        printf("  Wi-Fi STA   : Connected to '%s' (RSSI %d dBm)\n", w.sta_ssid, w.sta_rssi);
        printf("  WebUI URL   : http://%s/  (or http://%s.local/)\n", w.sta_ip, w.hostname);
    } else if (w.sta_configured) {
        printf("  Wi-Fi STA   : Connecting to '%s'...\n", w.sta_ssid);
    } else {
        printf("  Wi-Fi STA   : Not configured (use: wifi set <ssid> <password>)\n");
    }
    if (w.ap_active) {
        printf("  Setup AP    : '%s' -> http://%s/\n", w.ap_ssid, w.ap_ip);
    }

    printf("  BT Source   : %s [%s]\n", b.local_name, b.local_bda_str);
    if (b.connected) {
        printf("  BT Speaker  : CONNECTED -> %s [%s] (%s%s)\n",
               b.peer_name, b.peer_bda_str, b.codec_desc,
               b.media_started ? ", streaming" : ", idle");
    } else if (b.connecting) {
        printf("  BT Speaker  : Connecting to %s [%s]...\n", b.peer_name, b.peer_bda_str);
    } else if (b.scanning) {
        printf("  BT Speaker  : Scanning nearby devices...\n");
    } else {
        printf("  BT Speaker  : Disconnected (use: bt scan / bt connect <index|mac>)\n");
    }

    printf("  Player      : %s | Codec: %s | %lu Hz %u ch | %lu kbps\n",
           player_state_name(a.state), codec_name(a.codec),
           (unsigned long)a.sample_rate_in, a.channels_in, (unsigned long)a.bitrate_kbps);
    printf("  Station     : [%ld] %s\n", (long)a.station_index, a.station_name);
    if (a.icy_title[0]) {
        printf("  Now Playing : %s\n", a.icy_title);
    }
    if (a.error_msg[0]) {
        printf("  Last Notice : %s\n", a.error_msg);
    }
    printf("  Volume      : %u%%%s | Stream Buf: %u%% | PCM Buf: %u%%\n",
           volume, muted ? " (MUTED)" : "", a.stream_buf_pct, a.pcm_buf_pct);
    printf("  Memory      : Internal SRAM %u KB free | Octal PSRAM %u KB free\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U));
    float temp_c = 0.0f;
    if (board_hw_get_temp_c(&temp_c)) {
        printf("  Chip Temp   : %.1f C (%.1f F)\n\n", temp_c, temp_c * 9.0f / 5.0f + 32.0f);
    } else {
        printf("  Chip Temp   : unavailable\n\n");
    }
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        return cmd_status(argc, argv);
    }
    if (strcmp(argv[1], "scan") == 0) {
        printf("Scanning 2.4 GHz Wi-Fi networks...\n");
        wifi_ap_entry_t aps[WIFI_SCAN_MAX_APS];
        size_t count = 0;
        esp_err_t err = wifi_manager_scan(aps, WIFI_SCAN_MAX_APS, &count);
        if (err != ESP_OK) {
            printf("Scan failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("Found %zu networks:\n", count);
        for (size_t i = 0; i < count; i++) {
            printf("  %2zu. %-32s  RSSI: %4d dBm  Ch: %2u  %s\n",
                   i + 1, aps[i].ssid, aps[i].rssi, aps[i].channel,
                   aps[i].authmode == 0 ? "Open" : "Encrypted");
        }
        return 0;
    }
    if (strcmp(argv[1], "set") == 0) {
        if (argc < 3) {
            printf("Usage: wifi set <ssid> [password]\n");
            return 1;
        }
        const char *ssid = argv[2];
        const char *pass = (argc >= 4) ? argv[3] : "";
        esp_err_t err = wifi_manager_connect(ssid, pass, true);
        if (err == ESP_OK) {
            printf("Saved Wi-Fi '%s' and connecting... Watch log or run 'status' for WebUI IP.\n", ssid);
        } else {
            printf("Failed to set Wi-Fi: %s\n", esp_err_to_name(err));
        }
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "clear") == 0) {
        wifi_manager_disconnect_and_clear();
        printf("Cleared Wi-Fi credentials and enabled SoftAP '%s' (http://192.168.4.1).\n", WIFI_SOFTAP_SSID);
        return 0;
    }
    printf("Usage: wifi <status | scan | set <ssid> [password] | clear>\n");
    return 1;
}

static int cmd_bt(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: bt <scan [sec] | devices | connect <index|mac> | disconnect | forget [mac|all]>\n");
        return 1;
    }
    if (strcmp(argv[1], "scan") == 0) {
        int sec = (argc >= 3) ? atoi(argv[2]) : 10;
        esp_err_t err = bt_a2dp_scan_start((uint8_t)sec, false);
        printf("Bluetooth inquiry started for ~%d seconds (%s). Run 'bt devices' to view results.\n",
               sec, esp_err_to_name(err));
        return 0;
    }
    if (strcmp(argv[1], "devices") == 0) {
        bt_device_info_t devs[BT_SCAN_MAX_DEVICES];
        size_t n = bt_a2dp_get_scan_results(devs, BT_SCAN_MAX_DEVICES);
        printf("\nDiscovered Bluetooth Devices (%zu):\n", n);
        for (size_t i = 0; i < n; i++) {
            printf("  [%zu] %s  %-30s  RSSI: %4d dBm  %s%s\n",
                   i, devs[i].bda_str,
                   devs[i].name[0] ? devs[i].name : "(resolving...)",
                   devs[i].rssi,
                   devs[i].is_audio_sink ? "[Audio Sink]" : "[Other]",
                   devs[i].bonded ? " (Paired)" : "");
        }
        bt_device_info_t bonded[BT_BOND_MAX_DEVICES];
        size_t nb = bt_a2dp_get_bonded_devices(bonded, BT_BOND_MAX_DEVICES);
        printf("Bonded/Paired Devices (%zu):\n", nb);
        for (size_t i = 0; i < nb; i++) {
            printf("  * %s  %s\n", bonded[i].bda_str, bonded[i].name);
        }
        printf("\n");
        return 0;
    }
    if (strcmp(argv[1], "connect") == 0) {
        if (argc < 3) {
            printf("Usage: bt connect <scan_index | AA:BB:CC:DD:EE:FF>\n");
            return 1;
        }
        uint8_t bda[6] = {0};
        const char *name = NULL;
        bt_device_info_t devs[BT_SCAN_MAX_DEVICES];
        size_t n = bt_a2dp_get_scan_results(devs, BT_SCAN_MAX_DEVICES);

        if (!bt_parse_bda(argv[2], bda)) {
            char *endp = NULL;
            long idx = strtol(argv[2], &endp, 10);
            if (endp && *endp == '\0' && idx >= 0 && (size_t)idx < n) {
                memcpy(bda, devs[idx].bda, 6);
                name = devs[idx].name;
            } else {
                printf("Invalid MAC or scan index '%s'. Run 'bt scan' then 'bt devices'.\n", argv[2]);
                return 1;
            }
        }
        esp_err_t err = bt_a2dp_connect(bda, name);
        printf("Connecting to %02X:%02X:%02X:%02X:%02X:%02X (%s)\n",
               bda[0], bda[1], bda[2], bda[3], bda[4], bda[5], esp_err_to_name(err));
        return 0;
    }
    if (strcmp(argv[1], "disconnect") == 0) {
        bt_a2dp_disconnect();
        printf("Bluetooth A2DP disconnected.\n");
        return 0;
    }
    if (strcmp(argv[1], "forget") == 0) {
        if (argc < 3 || strcmp(argv[2], "all") == 0) {
            bt_a2dp_forget_all();
            printf("Cleared all paired Bluetooth devices.\n");
            return 0;
        }
        uint8_t bda[6] = {0};
        if (!bt_parse_bda(argv[2], bda)) {
            printf("Invalid MAC address.\n");
            return 1;
        }
        bt_a2dp_forget_device(bda);
        printf("Removed bond for %s.\n", argv[2]);
        return 0;
    }
    printf("Usage: bt <scan [sec] | devices | connect <index|mac> | disconnect | forget [mac|all]>\n");
    return 1;
}

static int cmd_play(int argc, char **argv)
{
    if (argc < 2) {
        audio_pipeline_resume();
        printf("Playback resumed.\n");
        return 0;
    }
    if (strncmp(argv[1], "http://", 7) == 0 || strncmp(argv[1], "https://", 8) == 0) {
        audio_pipeline_play_url(argv[1], (argc >= 3) ? argv[2] : "CLI Stream");
        printf("Streaming URL: %s\n", argv[1]);
        return 0;
    }
    int idx = atoi(argv[1]);
    if (audio_pipeline_play_station(idx) == ESP_OK) {
        printf("Playing preset station [%d].\n", idx);
        return 0;
    }
    printf("Invalid station index %d. Run 'stations' to list presets.\n", idx);
    return 1;
}

static int cmd_pause(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    audio_pipeline_toggle_play_pause();
    printf("Toggled Play/Pause.\n");
    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    audio_pipeline_stop();
    printf("Playback stopped.\n");
    return 0;
}

static int cmd_next(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    audio_pipeline_next_station();
    return 0;
}

static int cmd_prev(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    audio_pipeline_prev_station();
    return 0;
}

static int cmd_vol(int argc, char **argv)
{
    uint8_t cur_vol = 75;
    bool cur_muted = false;
    radio_config_get_volume(&cur_vol, &cur_muted);
    if (argc < 2) {
        printf("Current volume: %u%%%s\n", cur_vol, cur_muted ? " (muted)" : "");
        return 0;
    }
    if (strcmp(argv[1], "mute") == 0) {
        bool m = !cur_muted;
        radio_config_set_volume(cur_vol, m);
        audio_pipeline_set_volume(cur_vol, m);
        bt_a2dp_set_volume(m ? 0 : cur_vol);
        printf("Mute: %s\n", m ? "ON" : "OFF");
        return 0;
    }
    int v = atoi(argv[1]);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    radio_config_set_volume((uint8_t)v, false);
    audio_pipeline_set_volume((uint8_t)v, false);
    bt_a2dp_set_volume((uint8_t)v);
    printf("Volume set to %d%%\n", v);
    return 0;
}

static int cmd_stations(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "add") == 0) {
        if (argc < 4) {
            printf("Usage: stations add \"Station Name\" <http(s)://url> [genre]\n");
            return 1;
        }
        int idx = -1;
        esp_err_t err = radio_config_add_station(argv[2], argv[3], (argc >= 5) ? argv[4] : "Custom", &idx);
        if (err == ESP_OK) {
            printf("Added station [%d] %s\n", idx, argv[2]);
            return 0;
        }
        printf("Failed to add station: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (argc >= 3 && strcmp(argv[1], "del") == 0) {
        int idx = atoi(argv[2]);
        if (radio_config_delete_station(idx) == ESP_OK) {
            printf("Deleted station [%d]\n", idx);
            return 0;
        }
        printf("Invalid station index.\n");
        return 1;
    }

    int32_t active = 0;
    int32_t count = 0;
    radio_config_get_playback_prefs(&active, &count, NULL);
    printf("\nStation Presets (%ld/%d):\n", (long)count, RADIO_MAX_STATIONS);
    radio_station_t st = {0};
    for (int i = 0; i < count; i++) {
        if (radio_config_get_station(i, &st)) {
            printf("  %c [%2d] %-34s | %-18s | %s\n",
                   (i == active) ? '*' : ' ',
                   i, st.name, st.genre, st.url);
        }
    }
    printf("\n");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Rebooting ESP32-S31-Zero...\n");
    esp_restart();
    return 0;
}

esp_err_t usb_cli_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "s31-radio> ";
    repl_cfg.max_cmdline_length = 384;
    repl_cfg.task_stack_size = 6144;

    esp_console_dev_usb_serial_jtag_config_t hw_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl));

    esp_console_register_help_command();

    const esp_console_cmd_t cmds[] = {
        {.command = "status",   .help = "Show Wi-Fi WebUI URL, Bluetooth sink, and radio playback status", .func = cmd_status},
        {.command = "wifi",     .help = "Configure Wi-Fi: wifi <scan | set <ssid> [pass] | status | clear>", .func = cmd_wifi},
        {.command = "bt",       .help = "Control Bluetooth A2DP: bt <scan | devices | connect <id|mac> | disconnect | forget>", .func = cmd_bt},
        {.command = "play",     .help = "Play preset index or stream URL: play [index | http(s)://...]", .func = cmd_play},
        {.command = "pause",    .help = "Toggle Play / Pause", .func = cmd_pause},
        {.command = "stop",     .help = "Stop radio stream", .func = cmd_stop},
        {.command = "next",     .help = "Switch to next station preset", .func = cmd_next},
        {.command = "prev",     .help = "Switch to previous station preset", .func = cmd_prev},
        {.command = "vol",      .help = "Get/set volume or mute: vol [0..100 | mute]", .func = cmd_vol},
        {.command = "stations", .help = "List/add/delete presets: stations [add \"Name\" <url> | del <idx>]", .func = cmd_stations},
        {.command = "reboot",   .help = "Restart the board", .func = cmd_reboot},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "USB-C CLI active. Type 'help' or 'wifi set <ssid> <password>' for initial setup.");
    return ESP_OK;
}
