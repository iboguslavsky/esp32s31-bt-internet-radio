# Architecture & Engineering Notes

This document records the low-level systems architecture, memory budgeting, FreeRTOS task layout, and empirical 2.4 GHz coexistence tuning developed for the **Waveshare ESP32-S31-Zero Classic Bluetooth Internet Radio**.

---

## 1. Flash Partition Map & Boot Offsets

Defined in [`partitions.csv`](../partitions.csv) for the 16 MB QSPI Flash (`DIO @ 80 MHz`):

| Offset | Size | Name | Type / Subtype | Purpose |
|---|---|---|---|---|
| `0x002000` | — | `bootloader` | ROM stage 2 | ESP32-S31 bootloader (note: ESP32-S31 bootloader is at `0x2000`, not `0x0` or `0x1000`) |
| `0x008000` | `4 KB` | `partitions` | Partition table | Custom CSV partition table |
| `0x009000` | `64 KB` | `nvs` | `data / nvs` | Wi-Fi credentials, BT link keys, `cfg_v5` (32 presets, ~11.2 KB) |
| `0x019000` | `4 KB` | `phy_init` | `data / phy` | RF calibration data |
| `0x020000` | `4 MB` | `factory` | `app / factory` | Main firmware image (~2.1 MB used) |
| `0x420000` | `11.875 MB` | `storage` | `data / spiffs` | Reserved storage partition |

### PlatformIO `factory` Offset Workaround

`pioarduino/platform-espressif32` (`61.04.00-RC1`) computes `ESP32_APP_OFFSET` in `builder/main.py` by only checking `partition["subtype"] == "ota_0"`. For a `factory`-only partition table whose app starts at `0x20000` (because of the 64 KB NVS region), PlatformIO defaults `ESP32_APP_OFFSET` to `0x10000`, causing a bootloop (`invalid magic byte` at `0x20000`).

[`tools/pio_factory_offset.py`](../tools/pio_factory_offset.py) runs as a PlatformIO `post:` script to parse `partitions.csv` and automatically rewrite `ESP32_APP_OFFSET` (and `UPLOADERFLAGS`) to `0x20000`. [`Makefile`](../Makefile) also passes explicit flash offsets (`0x2000`, `0x8000`, `0x20000`) to `esptool.py`.

---

## 2. Memory Budget (320 KB Internal SRAM vs. 16 MB Octal PSRAM)

Internal SRAM on ESP32-S31 (`320 KB` usable) is shared by the Wi-Fi MAC/DMA buffers, LwIP, Orca BR/EDR Bluetooth Controller, Bluedroid host stacks, and RTOS task stacks. Running out of Internal SRAM triggers `assert failed: fixed_queue_new` inside Bluedroid.

Key allocations in [`sdkconfig.defaults`](../sdkconfig.defaults) and [`main/audio_pipeline.c`](../main/audio_pipeline.c):

- `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_200M=y`: Enables full-speed 200 MHz Octal PSRAM.
- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`: Keeps small/DMA-hot structures in Internal SRAM while routing allocations `> 4 KB` (such as mbedTLS contexts, cJSON serialization buffers, and large audio structures) into PSRAM automatically.
- `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536`: Guarantees a 64 KB Internal SRAM floor for DMA/controller allocations.
- `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`: Places TLS session buffers in Octal PSRAM.
- `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=12`, `DYNAMIC_RX=32`, `DYNAMIC_TX=32`: Balanced for continuous streaming without starving Bluedroid.
- **Audio ring buffers in PSRAM**: Both `s_stream_ring` (`2 MB`) and `s_pcm_ring` (`2 MB`) live in 200 MHz Octal PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`), leaving **~70–80 KB Internal SRAM free** and **~12–13 MB PSRAM free** during live playback.

---

## 3. Dual-Core FreeRTOS Task Layout

| Core | Task | Priority | Stack | Role |
|---|---|---:|---:|---|
| **Core 0** | Wi-Fi / LwIP (`wifi`, `tiT`) | IDF default | IDF default | 2.4 GHz Wi-Fi 6 MAC + TCP/IP |
| **Core 0** | BT Controller + Bluedroid (`BT_CTRL`, `BTU`, `BTC`) | IDF default | 6 KB BTC/BTU | Orca BR/EDR HCI + A2DP Source + AVRCP |
| **Core 1** | `radio_http` | **7** | 16 KB | HTTP/HTTPS ICY stream reader + `.pls`/`.m3u` resolver |
| **Core 1** | `radio_dec` | **5** | **24 KB** | `minimp3` / `libhelix-aac` decoder + Q16.16 resampler + micro-PLL |
| **Core 1** | `httpd` | **5** | 8 KB | WebUI & REST JSON API server |
| **Core 1** | `supervisor` / `board_hw` / `usb_cli` | low | 4–6 KB | LED animations, button gestures, temp sensor, 20 s stall watchdog, CLI |

### Critical Pinning & Stack Lessons

1. **Keep Bluedroid on Core 0 (`CONFIG_BT_BLUEDROID_PINNED_TO_CORE_0=y`)**:
   Pinning Bluedroid to Core 1 while the Orca `BT_CTRL` task stays on Core 0 causes cross-core ACL TX pacing jitter that exhausts the controller's ACL TX buffer pool (`CONFIG_BT_CTRL_BR_EDR_ACLU_TX_BUF_NB=10`, Kconfig max `10`), aborting with `assert failed: hci_driver_vhci_host_tx`. Keeping Bluedroid + `BT_CTRL` together on Core 0 and offloading `radio_http` + `radio_dec` + `httpd` to Core 1 eliminates this completely.
2. **Avoid Core 1 priority inversion**:
   `radio_http` (I/O-bound) runs at priority **7**, above CPU-bound `radio_dec` (priority **5**). `radio_dec` also yields with `vTaskDelay(1)` every 2 decoded frames so `httpd` (priority **5**) never starves when serving WebUI requests.
3. **`radio_dec` requires a 24 KB stack**:
   `minimp3`'s `mp3dec_decode_frame()` allocates `mp3dec_scratch_t` (~15 KB) on the stack. A 16 KB stack overflows immediately on MP3 streams; 24 KB provides clean headroom for both MP3 and HE-AAC SBR.

---

## 4. Lock-Free Audio Pipeline & Micro-PLL

Implemented in [`main/audio_pipeline.c`](../main/audio_pipeline.c):

- **Lock-free SPSC rings**: Both the `2 MB` compressed stream ring and `2 MB` PCM ring use `__atomic_load_n` / `__atomic_store_n` (`ACQUIRE` / `RELEASE`) so PSRAM `memcpy` never disables interrupts or stalls Core 0 wireless ISRs.
- **Fast tune-in + deep underrun resilience**:
  - `STREAM_PREBUFFER_BYTES = 32 KB` (~4 s at 64 kbps)
  - `PCM_PREBUFFER_BYTES = 64 KB` (~360 ms) for snappy station switching
  - `PCM_REBUFFER_BYTES = 352 KB` (~2.0 s) for clean recovery if a transient network glitch causes an underrun
  - `PCM_HIGH_WATERMARK = 1536 KB` (75% = ~8.9 s decoded reserve) caps decoder read-ahead
- **±0.15% Micro-PLL Clock-Drift Compensation**:
  Icecast server clocks and Bluetooth speaker A2DP media clocks inevitably drift by tens of ppm. Inside `resample_and_push_pcm()`, the Q16.16 resampler maintains a deadband between `600 KB` (~3.5 s) and `1050 KB` (~6.1 s):
  - Below `600 KB`: slows output rate by `-0.15%` (~±66 Hz) to gently rebuild buffer reserve
  - Above `1050 KB`: speeds output rate by `+0.15%` to prevent slow creep into high watermark
  - Inaudible pitch shift (`±2.6 cents`), keeping the PCM ring centered across multi-day runs.
- **Zero-click volume control**: Q15 smoothed gain ramping prevents zipper noise during WebUI or AVRCP volume changes.

---

## 5. Single-Antenna 2.4 GHz Wi-Fi + Classic BT Coexistence Tuning

The Waveshare ESP32-S31-Zero shares a **single 2.4 GHz antenna and RF front-end** between Wi-Fi 6 and Classic Bluetooth BR/EDR A2DP. Empirical testing on live hardware revealed several decisive findings:

### 1. Permanent `ESP_COEX_PREFER_WIFI` Is Mandatory
Across controlled builds comparing `ESP_COEX_PREFER_WIFI`, `ESP_COEX_PREFER_BALANCE`, and `ESP_COEX_PREFER_BT`:
- `ESP_COEX_PREFER_BT` and `ESP_COEX_PREFER_BALANCE` both starved Wi-Fi (`50–100%` ICMP ping loss) whenever Classic BT A2DP was actively streaming.
- Permanent **`ESP_COEX_PREFER_WIFI`** keeps Wi-Fi alive (`0%` loss) while Bluetooth A2DP still gets sufficient scheduled slots via the software coexistence arbiter (`CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`).

### 2. Modem Sleep + OFDM-Only + BT AFH Masking + SBC Bitpool Cap
- **`WIFI_PS_MIN_MODEM`** (NOT `WIFI_PS_NONE`): Disabling Wi-Fi modem sleep (`WIFI_PS_NONE`) caused the AP to transmit continuously during BT RF slots, resulting in 3–4.6 s AP retry queueing (bufferbloat). `WIFI_PS_MIN_MODEM` lets the ESP32-S31 coordinate clean PS-Poll/null-frame handoffs with the AP around BT slots.
- **OFDM-only 20 MHz (`WIFI_BW20`)**: Disables 802.11b (`1–11 Mbps` DSSS) so Wi-Fi airtime bursts finish 5–10× faster, leaving more RF time for BT.
- **`CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=n`**: Prevents long aggregated TX bursts from blocking Bluetooth ACL/SBC frames.
- **SBC Max Bitpool = `38`** (~245 kbps instead of default 53 ≈ 328 kbps): Preserves transparent audio quality while trimming ~25% of Bluetooth RF airtime.
- **Bluetooth AFH Wi-Fi Channel Blacklisting (`bt_a2dp_set_wifi_afh_channel`)**: Reads the connected Wi-Fi STA channel (`1–13`) and calls `esp_bt_gap_set_afh_channels()` to mark the overlapping 22 MHz Wi-Fi band as bad across the 79 Bluetooth hops, steering Classic BT hopping onto clean 2.4 GHz spectrum.
- **256-Byte Paced HTTP Responses (`send_small_chunks`)**: Sending full JSON/HTML payloads in one TCP burst caused LwIP socket stalls during BT streaming. Setting `TCP_NODELAY` and writing `256`-byte chunks with `5 ms` pacing in [`main/web_server.c`](../main/web_server.c) dropped `/api/status` response time to ~500–750 ms reliably.

### 3. The Stream Bitrate Knee (`48–96 kbps` AAC vs. `160 kbps` MP3)
With Classic BT A2DP active (`BP 38` ≈ 245 kbps), sustained single-antenna Wi-Fi RX hits its coexistence knee around `~100–130 kbps`:
- **`160 kbps` MP3** (`WKCR`) saturated Wi-Fi RX, causing `2–4 s` ping spikes and periodic underruns.
- **`64 kbps` AAC** (`KCSM`) ran effortlessly with **`8.6–83.8 ms` ping (`0%` packet loss)**, **`74%` (~8.8 s) PCM buffer**, and **`0` underruns**.

This motivated curating all 27 built-in presets around `35–96 kbps` AAC / HE-AAC (SBR) streams.

---

## 6. Long-Run Soak Verification

Observed telemetry from continuous unattended hardware testing on Waveshare ESP32-S31-Zero (Wi-Fi STA on Ch 6 + Anker SoundCore Classic BT A2DP sink):

- **Continuous uptime:** `162,238 s` (**45.1 hours**), `0` reboots
- **Internal SRAM free:** `~76 KB` steady (zero leak)
- **Octal PSRAM free:** `~12.1–13.2 MB` steady (zero leak)
- **Internal chip temperature (`tsens`):** `52–64 °C` steady
- **Live 64 kbps AAC streaming (`KCSM`):** `8.6–83.8 ms` Wi-Fi ping, `0%` packet loss, `74%` PCM buffer fill, `0` underruns
