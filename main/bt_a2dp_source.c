#include "bt_a2dp_source.h"
#include "radio_config.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

static const char *TAG = "bt_a2dp";

#define LOCAL_BT_NAME "ESP32-S31-Radio"

#define APP_RC_CT_TL_GET_CAPS         0
#define APP_RC_CT_TL_RN_VOL_CHANGE    1
#define APP_RC_CT_TL_SET_ABS_VOL      2

static SemaphoreHandle_t s_lock = NULL;
static TimerHandle_t s_hb_timer = NULL;

static bt_a2dp_pcm_data_cb_t s_pcm_cb = NULL;
static bt_avrc_remote_cb_t s_remote_cb = NULL;

static bt_a2dp_status_t s_st;
static bt_device_info_t s_scan_devs[BT_SCAN_MAX_DEVICES];
static size_t s_scan_count = 0;

static bool s_auto_connect_on_scan = false;
static bool s_user_disconnected = false;
static bool s_media_starting = false;
static bool s_acl_connected = false;
static uint32_t s_connect_timeout_ticks = 0;
static uint32_t s_reconnect_backoff_ticks = 0;
static uint32_t s_reconnect_attempts = 0;
static esp_avrc_rn_evt_cap_mask_t s_peer_rn_caps = {0};

char *bt_format_bda(const uint8_t bda[6], char *buf, size_t buf_sz)
{
    if (!bda || !buf || buf_sz < 18) {
        return NULL;
    }
    snprintf(buf, buf_sz, "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return buf;
}

bool bt_parse_bda(const char *str, uint8_t out_bda[6])
{
    if (!str || !out_bda) {
        return false;
    }
    unsigned int b[6] = {0};
    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out_bda[i] = (uint8_t)b[i];
    }
    return true;
}

static bool is_bda_bonded(const uint8_t bda[6])
{
    int num = esp_bt_gap_get_bond_device_num();
    if (num <= 0) {
        return false;
    }
    if (num > BT_BOND_MAX_DEVICES) {
        num = BT_BOND_MAX_DEVICES;
    }
    esp_bd_addr_t list[BT_BOND_MAX_DEVICES];
    if (esp_bt_gap_get_bond_device_list(&num, list) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < num; i++) {
        if (memcmp(list[i], bda, 6) == 0) {
            return true;
        }
    }
    return false;
}

static bool extract_eir_name(uint8_t *eir, char *out_name, size_t out_sz)
{
    if (!eir || !out_name || out_sz == 0) {
        return false;
    }
    uint8_t len = 0;
    uint8_t *p = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
    if (!p) {
        p = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
    }
    if (!p || len == 0) {
        return false;
    }
    if (len >= out_sz) {
        len = (uint8_t)(out_sz - 1);
    }
    memcpy(out_name, p, len);
    out_name[len] = '\0';
    return true;
}

static void record_discovered_device(esp_bt_gap_cb_param_t *param)
{
    uint32_t cod = 0;
    int8_t rssi = -99;
    char name[64] = {0};

    for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *p = param->disc_res.prop + i;
        switch (p->type) {
        case ESP_BT_GAP_DEV_PROP_COD:
            cod = *(uint32_t *)(p->val);
            break;
        case ESP_BT_GAP_DEV_PROP_RSSI:
            rssi = *(int8_t *)(p->val);
            break;
        case ESP_BT_GAP_DEV_PROP_EIR:
            extract_eir_name((uint8_t *)p->val, name, sizeof(name));
            break;
        case ESP_BT_GAP_DEV_PROP_BDNAME:
            if (name[0] == '\0' && p->len > 0) {
                size_t copy_len = (size_t)p->len < (sizeof(name) - 1) ? (size_t)p->len : (sizeof(name) - 1);
                memcpy(name, p->val, copy_len);
                name[copy_len] = '\0';
            }
            break;
        default:
            break;
        }
    }

    bool audio_sink = false;
    if (esp_bt_gap_is_valid_cod(cod)) {
        uint32_t srvc = esp_bt_gap_get_cod_srvc(cod);
        uint32_t major = esp_bt_gap_get_cod_major_dev(cod);
        if ((srvc & ESP_BT_COD_SRVC_RENDERING) || (srvc & ESP_BT_COD_SRVC_AUDIO) ||
            major == ESP_BT_COD_MAJOR_DEV_AV) {
            audio_sink = true;
        }
    }

    bool need_rnr = false;
    bool trigger_auto_connect = false;
    uint8_t auto_bda[6] = {0};
    char auto_name[64] = {0};

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = -1;
    for (size_t i = 0; i < s_scan_count; i++) {
        if (memcmp(s_scan_devs[i].bda, param->disc_res.bda, 6) == 0) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0 && s_scan_count < BT_SCAN_MAX_DEVICES) {
        idx = (int)s_scan_count++;
        memset(&s_scan_devs[idx], 0, sizeof(bt_device_info_t));
        memcpy(s_scan_devs[idx].bda, param->disc_res.bda, 6);
        bt_format_bda(param->disc_res.bda, s_scan_devs[idx].bda_str, sizeof(s_scan_devs[idx].bda_str));
        need_rnr = (name[0] == '\0');
    }
    if (idx >= 0) {
        if (rssi != -99) {
            s_scan_devs[idx].rssi = rssi;
        }
        if (cod != 0) {
            s_scan_devs[idx].cod = cod;
            s_scan_devs[idx].is_audio_sink = audio_sink;
        }
        if (name[0] != '\0') {
            snprintf(s_scan_devs[idx].name, sizeof(s_scan_devs[idx].name), "%s", name);
        }
        s_scan_devs[idx].bonded = is_bda_bonded(param->disc_res.bda);

        if (s_auto_connect_on_scan && s_scan_devs[idx].is_audio_sink) {
            s_auto_connect_on_scan = false;
            trigger_auto_connect = true;
            memcpy(auto_bda, s_scan_devs[idx].bda, 6);
            snprintf(auto_name, sizeof(auto_name), "%s",
                     s_scan_devs[idx].name[0] ? s_scan_devs[idx].name : s_scan_devs[idx].bda_str);
        }
    }
    xSemaphoreGive(s_lock);

    if (need_rnr && !trigger_auto_connect) {
        esp_bt_gap_read_remote_name(param->disc_res.bda);
    }

    if (trigger_auto_connect) {
        ESP_LOGI(TAG, "Auto-selecting discovered audio sink: %s", auto_name);
        esp_bt_gap_cancel_discovery();
        bt_a2dp_connect(auto_bda, auto_name);
    }
}

static void configure_preferred_sbc(esp_a2d_conn_hdl_t conn_hdl, const esp_a2d_mcc_t *sink_caps)
{
    if (!sink_caps || sink_caps->type != ESP_A2D_MCT_SBC) {
        return;
    }
    const esp_a2d_cie_sbc_t *caps = &sink_caps->cie.sbc_info;
    esp_a2d_mcc_t pref = {0};
    pref.type = ESP_A2D_MCT_SBC;

    /* Prefer 44.1 kHz (matches resampler output) */
    if (caps->samp_freq & ESP_A2D_SBC_CIE_SF_44K) {
        pref.cie.sbc_info.samp_freq = ESP_A2D_SBC_CIE_SF_44K;
    } else if (caps->samp_freq & ESP_A2D_SBC_CIE_SF_48K) {
        pref.cie.sbc_info.samp_freq = ESP_A2D_SBC_CIE_SF_48K;
    } else {
        return;
    }

    /* Prefer Joint Stereo -> Stereo -> Dual -> Mono */
    const char *mode_label = "Joint Stereo";
    if (caps->ch_mode & ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO) {
        pref.cie.sbc_info.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
        mode_label = "Joint Stereo";
    } else if (caps->ch_mode & ESP_A2D_SBC_CIE_CH_MODE_STEREO) {
        pref.cie.sbc_info.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_STEREO;
        mode_label = "Stereo";
    } else if (caps->ch_mode & ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL) {
        pref.cie.sbc_info.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL;
        mode_label = "Dual Ch";
    } else {
        pref.cie.sbc_info.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_MONO;
        mode_label = "Mono";
    }

    if (caps->block_len & ESP_A2D_SBC_CIE_BLOCK_LEN_16) {
        pref.cie.sbc_info.block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    } else if (caps->block_len & ESP_A2D_SBC_CIE_BLOCK_LEN_12) {
        pref.cie.sbc_info.block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_12;
    } else if (caps->block_len & ESP_A2D_SBC_CIE_BLOCK_LEN_8) {
        pref.cie.sbc_info.block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_8;
    } else {
        pref.cie.sbc_info.block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_4;
    }

    pref.cie.sbc_info.num_subbands = (caps->num_subbands & ESP_A2D_SBC_CIE_NUM_SUBBANDS_8)
                                         ? ESP_A2D_SBC_CIE_NUM_SUBBANDS_8
                                         : ESP_A2D_SBC_CIE_NUM_SUBBANDS_4;
    pref.cie.sbc_info.alloc_mthd = (caps->alloc_mthd & ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS)
                                       ? ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS
                                       : ESP_A2D_SBC_CIE_ALLOC_MTHD_SNR;

    uint8_t min_bp = caps->min_bitpool > 2 ? caps->min_bitpool : 2;
    /* Cap max bitpool at 38 (~245 kbps) to reduce 2.4 GHz airtime contention with Wi-Fi */
    uint8_t max_bp = caps->max_bitpool < 38 ? caps->max_bitpool : 38;
    if (max_bp < min_bp) {
        max_bp = min_bp;
    }
    pref.cie.sbc_info.min_bitpool = min_bp;
    pref.cie.sbc_info.max_bitpool = max_bp;

    if (esp_a2d_source_set_pref_mcc(conn_hdl, &pref) == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.negotiated_bitpool = max_bp;
        snprintf(s_st.codec_desc, sizeof(s_st.codec_desc), "SBC 44.1k %s (BP %u)", mode_label, max_bp);
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "Negotiated A2DP codec: SBC 44.1kHz %s, bitpool [%u..%u]", mode_label, min_bp, max_bp);
    }
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        record_discovered_device(param);
        break;

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.scanning = (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED);
        if (!s_st.scanning) {
            s_auto_connect_on_scan = false;
        }
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "Bluetooth inquiry %s (%zu devices found)",
                 s_st.scanning ? "started" : "finished", s_scan_count);
        break;

    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS &&
            param->read_rmt_name.rmt_name[0] != '\0') {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (size_t i = 0; i < s_scan_count; i++) {
                if (memcmp(s_scan_devs[i].bda, param->read_rmt_name.bda, 6) == 0) {
                    snprintf(s_scan_devs[i].name, sizeof(s_scan_devs[i].name),
                             "%.*s", (int)(sizeof(s_scan_devs[i].name) - 1),
                             (const char *)param->read_rmt_name.rmt_name);
                }
            }
            if (memcmp(s_st.peer_bda, param->read_rmt_name.bda, 6) == 0) {
                snprintf(s_st.peer_name, sizeof(s_st.peer_name),
                         "%.*s", (int)(sizeof(s_st.peer_name) - 1),
                         (const char *)param->read_rmt_name.rmt_name);
                radio_config_set_bt_target(s_st.peer_bda, s_st.peer_name);
            }
            xSemaphoreGive(s_lock);
        }
        break;

    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Bluetooth pairing/auth succeeded: %s", param->auth_cmpl.device_name);
            if (param->auth_cmpl.device_name[0] != '\0') {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (memcmp(s_st.peer_bda, param->auth_cmpl.bda, 6) == 0) {
                    snprintf(s_st.peer_name, sizeof(s_st.peer_name),
                             "%.*s", (int)(sizeof(s_st.peer_name) - 1),
                             (const char *)param->auth_cmpl.device_name);
                }
                xSemaphoreGive(s_lock);
                radio_config_set_bt_target(param->auth_cmpl.bda, (const char *)param->auth_cmpl.device_name);
            }
        } else {
            ESP_LOGW(TAG, "Bluetooth auth failed (status=%d)", param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_PIN_REQ_EVT: {
        char pin_str[RADIO_BT_PIN_MAX] = {0};
        radio_config_get_bt_pin(pin_str, sizeof(pin_str));
        esp_bt_pin_code_t pin = {0};
        if (param->pin_req.min_16_digit) {
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin);
        } else {
            uint8_t len = (uint8_t)strlen(pin_str);
            if (len > 16) {
                len = 16;
            }
            memcpy(pin, pin_str, len);
            ESP_LOGI(TAG, "Replying legacy BT PIN (%u digits)", len);
            esp_bt_gap_pin_reply(param->pin_req.bda, true, len, pin);
        }
        break;
    }

    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "SSP numeric comparison %06" PRIu32 " -> auto-confirming", param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey notification: %06" PRIu32, param->key_notif.passkey);
        break;

    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        if (param->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS) {
            s_acl_connected = true;
        }
        break;

    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        s_acl_connected = false;
        break;

    default:
        break;
    }
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        esp_a2d_connection_state_t state = param->conn_stat.state;
        if (state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_st.connecting = true;
            xSemaphoreGive(s_lock);
        } else if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            char bda_s[18];
            bt_format_bda(param->conn_stat.remote_bda, bda_s, sizeof(bda_s));

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_st.connecting = false;
            s_st.connected = true;
            s_st.media_started = false;
            s_media_starting = false;
            memcpy(s_st.peer_bda, param->conn_stat.remote_bda, 6);
            snprintf(s_st.peer_bda_str, sizeof(s_st.peer_bda_str), "%s", bda_s);
            if (s_st.peer_name[0] == '\0') {
                snprintf(s_st.peer_name, sizeof(s_st.peer_name), "%s", bda_s);
            }
            if (s_st.codec_desc[0] == '\0') {
                snprintf(s_st.codec_desc, sizeof(s_st.codec_desc), "SBC 44.1k Stereo");
            }
            xSemaphoreGive(s_lock);

            ESP_LOGI(TAG, "A2DP Sink Connected: %s [%s]", s_st.peer_name, bda_s);
            esp_bt_gap_read_remote_name(param->conn_stat.remote_bda);

            /* Immediately check source readiness to start A2DP media transport */
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGW(TAG, "A2DP Sink Disconnected (reason=%d)", param->conn_stat.disc_rsn);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_st.connecting = false;
            s_st.connected = false;
            s_st.media_started = false;
            s_st.avrc_connected = false;
            s_media_starting = false;
            xSemaphoreGive(s_lock);
        }
        break;
    }

    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
            param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            s_media_starting = true;
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        } else if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START) {
            s_media_starting = false;
            if (param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_st.media_started = true;
                xSemaphoreGive(s_lock);
                ESP_LOGI(TAG, "A2DP audio transport active (44.1 kHz 16-bit Stereo)");

                uint8_t vol = 75;
                bool muted = false;
                radio_config_get_volume(&vol, &muted);
                bt_a2dp_set_volume(muted ? 0 : vol);
            }
        } else if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND &&
                   param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_st.media_started = false;
            xSemaphoreGive(s_lock);
        }
        break;

    case ESP_A2D_AUDIO_STATE_EVT:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.media_started = (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED);
        xSemaphoreGive(s_lock);
        break;

    case ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT:
        configure_preferred_sbc(param->a2d_report_snk_codec_caps_stat.conn_hdl,
                                &param->a2d_report_snk_codec_caps_stat.mcc);
        break;

    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.sink_delay_tenths_ms = param->a2d_report_delay_value_stat.delay_value;
        xSemaphoreGive(s_lock);
        break;

    default:
        break;
    }
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.avrc_connected = param->conn_stat.connected;
        xSemaphoreGive(s_lock);
        if (param->conn_stat.connected) {
            esp_avrc_ct_send_get_rn_capabilities_cmd(APP_RC_CT_TL_GET_CAPS);
            uint8_t vol = 75;
            bool muted = false;
            radio_config_get_volume(&vol, &muted);
            bt_a2dp_set_volume(muted ? 0 : vol);
        } else {
            s_peer_rn_caps.bits = 0;
        }
        break;

    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        s_peer_rn_caps.bits = param->get_rn_caps_rsp.evt_set.bits;
        if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST,
                                               &s_peer_rn_caps, ESP_AVRC_RN_VOLUME_CHANGE)) {
            esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_VOL_CHANGE,
                                                       ESP_AVRC_RN_VOLUME_CHANGE, 0);
        }
        break;

    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            uint8_t raw_vol = param->change_ntf.event_parameter.volume; /* 0..127 */
            uint8_t pct = (uint8_t)((raw_vol * 100U + 63U) / 127U);
            if (pct > 100) {
                pct = 100;
            }
            ESP_LOGI(TAG, "Speaker reported AVRCP volume: %u%%", pct);
            if (s_remote_cb) {
                s_remote_cb(BT_AV_CMD_VOL_CHANGED, pct);
            }
            if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST,
                                                   &s_peer_rn_caps, ESP_AVRC_RN_VOLUME_CHANGE)) {
                esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_VOL_CHANGE,
                                                           ESP_AVRC_RN_VOLUME_CHANGE, 0);
            }
        }
        break;

    default:
        break;
    }
}

static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    if (event == ESP_AVRC_TG_PASSTHROUGH_CMD_EVT) {
        /* Trigger only on button press (key_state == 0) */
        if (param->psth_cmd.key_state == ESP_AVRC_PT_CMD_STATE_PRESSED && s_remote_cb) {
            ESP_LOGI(TAG, "Received headphone AVRCP button: 0x%02x", param->psth_cmd.key_code);
            switch (param->psth_cmd.key_code) {
            case ESP_AVRC_PT_CMD_PLAY:
                s_remote_cb(BT_AV_CMD_PLAY, 0);
                break;
            case ESP_AVRC_PT_CMD_PAUSE:
                s_remote_cb(BT_AV_CMD_PAUSE, 0);
                break;
            case ESP_AVRC_PT_CMD_STOP:
                s_remote_cb(BT_AV_CMD_STOP, 0);
                break;
            case ESP_AVRC_PT_CMD_FORWARD:
            case ESP_AVRC_PT_CMD_CHAN_UP:
                s_remote_cb(BT_AV_CMD_NEXT, 0);
                break;
            case ESP_AVRC_PT_CMD_BACKWARD:
            case ESP_AVRC_PT_CMD_CHAN_DOWN:
                s_remote_cb(BT_AV_CMD_PREV, 0);
                break;
            default:
                break;
            }
        }
    }
}

static int32_t internal_pcm_cb(uint8_t *data, int32_t len)
{
    if (!data || len <= 0) {
        return 0;
    }
    if (s_pcm_cb) {
        return s_pcm_cb(data, len);
    }
    memset(data, 0, (size_t)len);
    return len;
}

static void heartbeat_timer_cb(TimerHandle_t tmr)
{
    (void)tmr;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool scanning = s_st.scanning;
    bool connected = s_st.connected;
    bool connecting = s_st.connecting;
    bool media_started = s_st.media_started;
    xSemaphoreGive(s_lock);

    if (connected) {
        s_connect_timeout_ticks = 0;
        s_reconnect_backoff_ticks = 0;
        s_reconnect_attempts = 0;
        if (!media_started && !s_media_starting) {
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        }
        return;
    }

    if (connecting) {
        if (++s_connect_timeout_ticks >= 3) {
            s_connect_timeout_ticks = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_st.connecting = false;
            xSemaphoreGive(s_lock);
        }
        return;
    }

    if (scanning || s_user_disconnected || s_acl_connected) {
        return;
    }

    /* Back off auto-reconnect (30s interval, max 3 automatic retries) so paging doesn't starve Wi-Fi */
    if (++s_reconnect_backoff_ticks < 5) {
        return;
    }
    s_reconnect_backoff_ticks = 0;
    if (s_reconnect_attempts >= 3) {
        return;
    }

    uint8_t target_bda[6] = {0};
    char target_name[RADIO_BT_NAME_MAX] = {0};
    bool auto_reconn = false;
    if (radio_config_get_bt_target(target_bda, target_name, sizeof(target_name), &auto_reconn) && auto_reconn) {
        s_reconnect_attempts++;
        ESP_LOGI(TAG, "Auto-reconnecting to saved Bluetooth sink (%lu/3): %s",
                 (unsigned long)s_reconnect_attempts, target_name);
        bt_a2dp_connect(target_bda, target_name);
    }
}

esp_err_t bt_a2dp_source_start(bt_a2dp_pcm_data_cb_t pcm_cb, bt_avrc_remote_cb_t remote_cb)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    s_pcm_cb = pcm_cb;
    s_remote_cb = remote_cb;
    memset(&s_st, 0, sizeof(s_st));
    snprintf(s_st.local_name, sizeof(s_st.local_name), "%s", LOCAL_BT_NAME);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    bd_cfg.ssp_en = true;
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* Enable Secure Simple Pairing (Just Works / Auto-Confirm) + Legacy PIN fallback */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    esp_bt_pin_code_t pin_code = {0};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin_code);

    esp_bt_gap_set_device_name(LOCAL_BT_NAME);
    esp_bt_gap_register_callback(gap_cb);

    /* Initialize AVRCP Controller (volume) & AVRCP Target (headphone buttons) BEFORE A2DP */
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    ESP_ERROR_CHECK(esp_avrc_ct_register_callback(avrc_ct_cb));

    ESP_ERROR_CHECK(esp_avrc_tg_init());
    ESP_ERROR_CHECK(esp_avrc_tg_register_callback(avrc_tg_cb));

    esp_avrc_psth_bit_mask_t cmd_set = {0};
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_PLAY);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_PAUSE);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_STOP);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_FORWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_BACKWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_CHAN_UP);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_CHAN_DOWN);
    esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &cmd_set);

    ESP_ERROR_CHECK(esp_a2d_source_init());
    ESP_ERROR_CHECK(esp_a2d_register_callback(a2d_cb));
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(internal_pcm_cb));

    /* Allow bonded headphones/speakers to reconnect to us as well */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    const uint8_t *own_bda = esp_bt_dev_get_address();
    if (own_bda) {
        bt_format_bda(own_bda, s_st.local_bda_str, sizeof(s_st.local_bda_str));
    }

    uint8_t target_bda[6] = {0};
    char target_name[RADIO_BT_NAME_MAX] = {0};
    bool auto_reconn = false;
    bool has_target = radio_config_get_bt_target(target_bda, target_name, sizeof(target_name), &auto_reconn);
    if (has_target) {
        memcpy(s_st.peer_bda, target_bda, 6);
        bt_format_bda(target_bda, s_st.peer_bda_str, sizeof(s_st.peer_bda_str));
        snprintf(s_st.peer_name, sizeof(s_st.peer_name), "%s", target_name);
    }
    s_st.initialized = true;

    ESP_LOGI(TAG, "Classic Bluetooth Orca BR/EDR A2DP Source initialized (MAC: %s)", s_st.local_bda_str);

    s_hb_timer = xTimerCreate("bt_hb", pdMS_TO_TICKS(6000), pdTRUE, NULL, heartbeat_timer_cb);
    xTimerStart(s_hb_timer, 0);

    if (has_target && auto_reconn) {
        bt_a2dp_connect(target_bda, target_name);
    }

    return ESP_OK;
}

esp_err_t bt_a2dp_scan_start(uint8_t duration_sec, bool auto_connect_first_sink)
{
    if (duration_sec < 3) {
        duration_sec = 8;
    }
    if (duration_sec > 30) {
        duration_sec = 30;
    }
    /* Convert seconds to 1.28s inquiry units */
    uint8_t inq_len = (uint8_t)((duration_sec * 100 + 64) / 128);
    if (inq_len < 2) {
        inq_len = 2;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_scan_count = 0;
    memset(s_scan_devs, 0, sizeof(s_scan_devs));
    s_auto_connect_on_scan = auto_connect_first_sink;
    xSemaphoreGive(s_lock);

    esp_bt_gap_cancel_discovery();
    return esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, inq_len, 0);
}

esp_err_t bt_a2dp_scan_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_auto_connect_on_scan = false;
    xSemaphoreGive(s_lock);
    return esp_bt_gap_cancel_discovery();
}

esp_err_t bt_a2dp_connect(const uint8_t bda[6], const char *name)
{
    if (!bda) {
        return ESP_ERR_INVALID_ARG;
    }
    s_user_disconnected = false;
    esp_bt_gap_cancel_discovery();

    char resolved_name[64] = {0};
    if (name && name[0] != '\0') {
        snprintf(resolved_name, sizeof(resolved_name), "%s", name);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (resolved_name[0] == '\0') {
        for (size_t i = 0; i < s_scan_count; i++) {
            if (memcmp(s_scan_devs[i].bda, bda, 6) == 0 && s_scan_devs[i].name[0] != '\0') {
                snprintf(resolved_name, sizeof(resolved_name), "%s", s_scan_devs[i].name);
                break;
            }
        }
    }
    bool need_disconnect = s_st.connected && memcmp(s_st.peer_bda, bda, 6) != 0;
    uint8_t old_bda[6];
    memcpy(old_bda, s_st.peer_bda, 6);

    memcpy(s_st.peer_bda, bda, 6);
    bt_format_bda(bda, s_st.peer_bda_str, sizeof(s_st.peer_bda_str));
    snprintf(s_st.peer_name, sizeof(s_st.peer_name), "%s",
             resolved_name[0] ? resolved_name : s_st.peer_bda_str);
    s_st.connecting = true;
    s_connect_timeout_ticks = 0;
    xSemaphoreGive(s_lock);

    radio_config_set_bt_target(bda, resolved_name);

    if (need_disconnect) {
        esp_a2d_source_disconnect(old_bda);
    }

    uint8_t target[6];
    memcpy(target, bda, 6);
    ESP_LOGI(TAG, "Connecting A2DP Source to %s [%s]...", s_st.peer_name, s_st.peer_bda_str);
    return esp_a2d_source_connect(target);
}

esp_err_t bt_a2dp_disconnect(void)
{
    s_user_disconnected = true;
    uint8_t target[6] = {0};
    bool was_conn = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    was_conn = s_st.connected || s_st.connecting;
    memcpy(target, s_st.peer_bda, 6);
    s_st.connecting = false;
    xSemaphoreGive(s_lock);

    if (!was_conn) {
        return ESP_OK;
    }
    if (s_st.media_started) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
    }
    return esp_a2d_source_disconnect(target);
}

esp_err_t bt_a2dp_forget_device(const uint8_t bda[6])
{
    if (!bda) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t target_bda[6] = {0};
    if (radio_config_get_bt_target(target_bda, NULL, 0, NULL) && memcmp(target_bda, bda, 6) == 0) {
        bt_a2dp_disconnect();
        radio_config_clear_bt_target();
    }
    uint8_t copy[6];
    memcpy(copy, bda, 6);
    return esp_bt_gap_remove_bond_device(copy);
}

esp_err_t bt_a2dp_forget_all(void)
{
    bt_a2dp_disconnect();
    radio_config_clear_bt_target();

    int num = esp_bt_gap_get_bond_device_num();
    if (num <= 0) {
        return ESP_OK;
    }
    if (num > BT_BOND_MAX_DEVICES) {
        num = BT_BOND_MAX_DEVICES;
    }
    esp_bd_addr_t list[BT_BOND_MAX_DEVICES];
    if (esp_bt_gap_get_bond_device_list(&num, list) == ESP_OK) {
        for (int i = 0; i < num; i++) {
            esp_bt_gap_remove_bond_device(list[i]);
        }
    }
    return ESP_OK;
}

esp_err_t bt_a2dp_set_volume(uint8_t volume_pct)
{
    if (volume_pct > 100) {
        volume_pct = 100;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool avrc_conn = s_st.avrc_connected;
    xSemaphoreGive(s_lock);

    if (!avrc_conn) {
        return ESP_OK;
    }
    uint8_t avrc_vol = (uint8_t)((volume_pct * 127U + 50U) / 100U);
    return esp_avrc_ct_send_set_absolute_volume_cmd(APP_RC_CT_TL_SET_ABS_VOL, avrc_vol);
}

bool bt_a2dp_peer_supports_abs_vol(void)
{
    return s_st.avrc_connected &&
           esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST,
                                              &s_peer_rn_caps, ESP_AVRC_RN_VOLUME_CHANGE);
}

void bt_a2dp_get_status(bt_a2dp_status_t *out)
{
    if (!out) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}

size_t bt_a2dp_get_scan_results(bt_device_info_t *out_list, size_t max_items)
{
    if (!out_list || max_items == 0) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_scan_count < max_items ? s_scan_count : max_items;
    memcpy(out_list, s_scan_devs, n * sizeof(bt_device_info_t));
    xSemaphoreGive(s_lock);
    return n;
}

size_t bt_a2dp_get_bonded_devices(bt_device_info_t *out_list, size_t max_items)
{
    if (!out_list || max_items == 0) {
        return 0;
    }
    int num = esp_bt_gap_get_bond_device_num();
    if (num <= 0) {
        return 0;
    }
    if (num > BT_BOND_MAX_DEVICES) {
        num = BT_BOND_MAX_DEVICES;
    }
    esp_bd_addr_t list[BT_BOND_MAX_DEVICES];
    if (esp_bt_gap_get_bond_device_list(&num, list) != ESP_OK) {
        return 0;
    }

    uint8_t target_bda[6] = {0};
    char target_name[RADIO_BT_NAME_MAX] = {0};
    bool has_target = radio_config_get_bt_target(target_bda, target_name, sizeof(target_name), NULL);

    size_t count = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < num && count < max_items; i++) {
        memset(&out_list[count], 0, sizeof(bt_device_info_t));
        memcpy(out_list[count].bda, list[i], 6);
        bt_format_bda(list[i], out_list[count].bda_str, sizeof(out_list[count].bda_str));
        out_list[count].bonded = true;
        out_list[count].is_audio_sink = true;

        if (has_target && memcmp(target_bda, list[i], 6) == 0 && target_name[0]) {
            snprintf(out_list[count].name, sizeof(out_list[count].name), "%s", target_name);
        } else {
            for (size_t j = 0; j < s_scan_count; j++) {
                if (memcmp(s_scan_devs[j].bda, list[i], 6) == 0 && s_scan_devs[j].name[0]) {
                    snprintf(out_list[count].name, sizeof(out_list[count].name), "%s", s_scan_devs[j].name);
                    break;
                }
            }
        }
        if (out_list[count].name[0] == '\0') {
            snprintf(out_list[count].name, sizeof(out_list[count].name), "Paired Speaker (%s)", out_list[count].bda_str);
        }
        count++;
    }
    xSemaphoreGive(s_lock);
    return count;
}

esp_err_t bt_a2dp_set_wifi_afh_channel(uint8_t wifi_ch)
{
    if (!s_st.initialized || wifi_ch < 1 || wifi_ch > 13) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 2.4 GHz Wi-Fi ch center = 2407 + 5*ch MHz (±11 MHz = 22 MHz bandwidth).
     * Classic BT ch k (0..78) = 2402 + k MHz. Mark overlapping BT channels bad (0). */
    int center_mhz = 2407 + 5 * (int)wifi_ch;
    int low_bt = (center_mhz - 11) - 2402;
    int high_bt = (center_mhz + 11) - 2402;
    if (low_bt < 0) low_bt = 0;
    if (high_bt > 78) high_bt = 78;

    esp_bt_gap_afh_channels map;
    memset(map, 0xFF, sizeof(map));
    map[9] &= 0x7F; /* Bit 79 is reserved and must be 0 */

    int usable = 79;
    for (int k = low_bt; k <= high_bt; k++) {
        map[k / 8] &= (uint8_t)~(1U << (k % 8));
        usable--;
    }

    esp_err_t err = esp_bt_gap_set_afh_channels(map);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Applied BT AFH mask for Wi-Fi Ch %u (blacklisted BT ch %d..%d, %d/79 usable)",
                 (unsigned)wifi_ch, low_bt, high_bt, usable);
    } else {
        ESP_LOGW(TAG, "esp_bt_gap_set_afh_channels(Ch %u) failed: %s",
                 (unsigned)wifi_ch, esp_err_to_name(err));
    }
    return err;
}
