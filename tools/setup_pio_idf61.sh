#!/usr/bin/env bash
set -euo pipefail

PLATFORM_DIR="${HOME}/.platformio/platforms/espressif32-idf61"
REPO_URL="https://github.com/pioarduino/platform-espressif32.git"
TAG="61.04.00-RC1"

if [[ -d "${PLATFORM_DIR}" && -f "${PLATFORM_DIR}/platform.json" ]]; then
  echo "ESP-IDF v6.1 PlatformIO platform already installed at ${PLATFORM_DIR}"
  exit 0
fi

echo "Installing pioarduino/platform-espressif32 (${TAG}) into ${PLATFORM_DIR}..."
mkdir -p "$(dirname "${PLATFORM_DIR}")"
git clone --depth 1 --branch "${TAG}" "${REPO_URL}" "${PLATFORM_DIR}"

# Allow standard PlatformIO Core 6.1.x (upstream platform.json sets >=6.2.0 for pioarduino-core)
python3 - "${PLATFORM_DIR}/platform.json" <<'PY'
import json, sys
path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)
if data.get("engines", {}).get("platformio") == ">=6.2.0":
    data["engines"]["platformio"] = ">=6.1.0"
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    print("Patched platform.json engines.platformio -> >=6.1.0")
PY

echo "Done. You can now run: make build"
