PIO ?= $(shell command -v pio 2>/dev/null || echo $(HOME)/.local/bin/pio)
PYTHON ?= $(HOME)/.platformio/penv/bin/python
ESPTOOL ?= $(HOME)/.platformio/packages/tool-esptoolpy/esptool.py
BUILD_DIR := .pio/build/waveshare_esp32_s31_zero

# Auto-detect highest /dev/ttyACM* (fallback /dev/ttyUSB*), or pass PORT=/dev/ttyACM2
PORT ?= $(shell ls -1 /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | sort -V | tail -n 1)
BAUD ?= 115200
BEFORE ?= default-reset

.PHONY: all setup-pio build flash flash-dl monitor clean distclean help

help:
	@echo "ESP32-S31-Zero Classic Bluetooth Internet Radio (PlatformIO + ESP-IDF v6.1)"
	@echo ""
	@echo "Targets:"
	@echo "  make setup-pio  Install pioarduino ESP-IDF v6.1 platform into ~/.platformio/platforms/espressif32-idf61"
	@echo "  make build      Build firmware via PlatformIO (auto-runs setup-pio if needed)"
	@echo "  make flash      Flash bootloader, partitions, and firmware (--no-stub)"
	@echo "  make flash-dl   Flash when already in BOOT+RESET download mode (--before no-reset)"
	@echo "  make monitor    Open USB-C serial console (rts=0, dtr=0)"
	@echo "  make all        Build, flash, and open serial console"
	@echo "  make clean      Clean PlatformIO build outputs"
	@echo "  make distclean  Remove .pio and generated sdkconfig.waveshare_esp32_s31_zero"
	@echo ""
	@echo "Variables:"
	@echo "  PORT=$(if $(PORT),$(PORT),<none detected>)   Override serial port (e.g. PORT=/dev/ttyACM2)"
	@echo "  BEFORE=$(BEFORE)          Reset mode (default-reset | usb-reset | no-reset)"

all: build flash monitor

setup-pio:
	@test -d "$(HOME)/.platformio/platforms/espressif32-idf61" || bash tools/setup_pio_idf61.sh

build: setup-pio
	$(PIO) run

flash: build
	@test -n "$(PORT)" || (echo "ERROR: No /dev/ttyACM* or /dev/ttyUSB* found. Pass PORT=/dev/ttyACM..." && exit 1)
	$(PYTHON) $(ESPTOOL) \
		--chip esp32s31 --port $(PORT) --baud $(BAUD) \
		--before $(BEFORE) --after hard-reset --no-stub \
		write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
		0x2000 $(BUILD_DIR)/bootloader.bin \
		0x8000 $(BUILD_DIR)/partitions.bin \
		0x20000 $(BUILD_DIR)/firmware.bin

flash-dl:
	@$(MAKE) flash BEFORE=no-reset

monitor:
	@test -n "$(PORT)" || (echo "ERROR: No /dev/ttyACM* or /dev/ttyUSB* found. Pass PORT=/dev/ttyACM..." && exit 1)
	$(PIO) device monitor --port $(PORT)

clean:
	$(PIO) run -t clean

distclean:
	rm -rf .pio sdkconfig.waveshare_esp32_s31_zero
