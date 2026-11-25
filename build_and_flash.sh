#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v idf.py >/dev/null 2>&1; then
  export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$HOME/.espressif-v6.1}"
  IDF_PATH_DIR="${IDF_PATH:-$HOME/esp/esp-idf-v6.1}"
  IDF_EXPORT="$IDF_PATH_DIR/export.sh"

  if [[ ! -f "$IDF_EXPORT" ]]; then
    echo "Error: idf.py not in PATH and ESP-IDF v6.1 not found at $IDF_PATH_DIR" >&2
    echo "Set IDF_PATH=/path/to/esp-idf-v6.1 or source export.sh first." >&2
    exit 1
  fi

  # shellcheck disable=SC1090
  source "$IDF_EXPORT" >/dev/null
fi

cd "$PROJECT_DIR"

ACTION="${1:-build}"
PORT="${2:-}"

if [[ ! -f sdkconfig ]]; then
  idf.py --preview set-target esp32s31
fi

PORT_ARGS=()
if [[ -n "$PORT" ]]; then
  PORT_ARGS=(-p "$PORT")
fi

case "$ACTION" in
  build)
    idf.py build
    ;;
  flash)
    idf.py "${PORT_ARGS[@]}" flash
    echo "If the board remains in download mode, press the RESET button on the ESP32-S31-Zero."
    ;;
  monitor)
    idf.py "${PORT_ARGS[@]}" monitor --no-reset
    ;;
  all|flash-monitor)
    idf.py "${PORT_ARGS[@]}" build flash
    echo "If the board does not auto-boot after flashing, press the RESET button on the ESP32-S31-Zero."
    idf.py "${PORT_ARGS[@]}" monitor --no-reset
    ;;
  clean)
    idf.py fullclean
    rm -f sdkconfig
    ;;
  *)
    echo "Usage: $0 [build | flash | monitor | all | clean] [serial_port]"
    echo "Example: $0 all /dev/ttyACM0"
    exit 1
    ;;
esac
