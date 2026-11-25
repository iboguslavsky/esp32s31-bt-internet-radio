"""
PlatformIO post-script: Ensure ESP32_APP_OFFSET honors the 'factory' partition
in partitions.csv.

pioarduino/platform-espressif32 (61.04.00-RC1) only checks subtype == 'ota_0'
when computing ESP32_APP_OFFSET, defaulting to 0x10000 for factory-only tables.
Our partitions.csv places the factory app at 0x20000 (after a 64 KB NVS region).
"""

import os

Import("env")


def _parse_int(val):
    val = val.strip()
    if val.lower().endswith("k"):
        return int(val[:-1], 0) * 1024
    if val.lower().endswith("m"):
        return int(val[:-1], 0) * 1024 * 1024
    return int(val, 0)


def _find_factory_offset(csv_path):
    if not csv_path or not os.path.isfile(csv_path):
        return None
    with open(csv_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) >= 5 and cols[1] == "app" and cols[2] == "factory" and cols[3]:
                return _parse_int(cols[3])
    return None


part_csv = env.get("PARTITIONS_TABLE_CSV") or os.path.join(
    env.subst("$PROJECT_DIR"), "partitions.csv"
)
factory_offset = _find_factory_offset(part_csv)

if factory_offset is not None:
    offset_hex = hex(factory_offset)
    old_offset = env.get("ESP32_APP_OFFSET", "0x10000")
    if old_offset != offset_hex:
        env.Replace(ESP32_APP_OFFSET=offset_hex)
        if "INTEGRATION_EXTRA_DATA" in env:
            env["INTEGRATION_EXTRA_DATA"]["application_offset"] = offset_hex
        # Also patch UPLOADERFLAGS if the old offset was already expanded
        flags = list(env.get("UPLOADERFLAGS", []))
        updated = False
        for i in range(len(flags) - 1):
            if str(flags[i]) == str(old_offset) and "firmware.bin" in str(flags[i + 1]):
                flags[i] = offset_hex
                updated = True
        if updated:
            env.Replace(UPLOADERFLAGS=flags)
        print(f"[pio_factory_offset] Updated ESP32_APP_OFFSET: {old_offset} -> {offset_hex}")
