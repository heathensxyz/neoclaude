# NeoClaude

## What This Is
Desk companion running an animated pixel pet on a Waveshare ESP32-S3-Touch-AMOLED-1.8INCH with tap-to-approve for Claude Code permissions.

## Tech Stack
- **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.8INCH (368x448 AMOLED, QSPI SH8601, FT3168 touch)
- **Framework:** Arduino via PlatformIO
- **Display:** Arduino GFX library (direct rendering, no LVGL)
- **Storage:** LittleFS for sprite data, PSRAM for frame buffers at runtime
- **Sprites:** RGB565 binary files, 192x208 per frame, up to two pet sets

## Key Decisions
- **PlatformIO over Arduino IDE:** cleaner for projects with binary data files (LittleFS upload via `pio run -t uploadfs`)
- **Direct GFX over LVGL:** sprite animation and permission text rendering work fine with Arduino GFX
- **LittleFS over embedded C arrays:** sprites are too large for app partition. LittleFS data partition holds them, and sprites can be updated without recompiling.
- **2x nearest-neighbor scaling:** 192x208 sprites scaled to 368x416, centered on 368x448 display
- **Partition scheme:** 3MB app, ~12.9MB data (custom partitions.csv)
- **Permission bridge via Claude Code hooks:** PermissionRequest hook POSTs to ESP32 HTTP server via bridge script. No Mac-side service needed.
- **Command hook over HTTP hook type:** Claude Code's command hook with curl is more proven and allows fallback logic in the script
- **Side button = approve, power key = deny:** more accessible button maps to more common action

## Project Layout
```
src/main.cpp              - firmware (pet animation + WiFi + HTTP server + permission bridge)
include/pin_config.h      - board pin definitions
include/wifi_config.h     - WiFi credentials (.gitignored)
lib/                      - Waveshare vendor libraries + XPowersLib + ArduinoJson
lib/XPowersLib/           - AXP2101 PMU driver (for power key button)
data/                     - LittleFS partition content (generated, .gitignored)
tools/convert_sprites.py  - sprite sheet to RGB565 converter
```

## Dev Notes
- FT3168 touch address: 0x38, uses interrupt on GPIO21
- XCA9554 I2C expander at 0x20 controls display/touch power and reset
- ARDUINO_USB_CDC_ON_BOOT=1 makes `Serial` use USB CDC (no HWCDC workaround needed)
- Frame data: each .bin file is exactly 79,872 bytes (192 * 208 * 2)
- PSRAM allocation: 8 frame buffers pre-allocated (~624KB total)
- Speech bubble overlay attempted but abandoned: 2x sprite covers y=16-432, no room for overlays
