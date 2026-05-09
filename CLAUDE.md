# Hendo ESP32

## What This Is
Desktop companion unit running the Hendo OpenPets sprite on a Waveshare ESP32-S3-Touch-AMOLED-1.8INCH, with WiFi sync to the desktop pet and tap-to-approve for Claude Code permissions.

## Tech Stack
- **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.8INCH (368x448 AMOLED, QSPI SH8601, FT3168 touch)
- **Framework:** Arduino via PlatformIO
- **Display:** Arduino GFX library (direct rendering, no LVGL for Phase 1)
- **Storage:** LittleFS for sprite data, PSRAM for frame buffers at runtime
- **Sprites:** RGB565 binary files, 192x208 per frame, two sets (Gemini realistic + Duo chibi)

## Key Decisions
- **PlatformIO over Arduino IDE:** cleaner for projects with binary data files (LittleFS upload via `pio run -t uploadfs`)
- **Direct GFX over LVGL:** sprite animation doesn't need a UI framework. LVGL will be added in Phase 3 for the permission approval UI.
- **LittleFS over embedded C arrays:** 117 frames across both sprite sets = ~9MB, too large for app partition. LittleFS data partition holds it all, and sprites can be updated without recompiling.
- **No scaling for Phase 1:** 192x208 sprites centered on 368x448 display. Black AMOLED border is invisible. Will add configurable scaling later if needed.
- **Partition scheme:** 3MB app, ~12.9MB data (custom partitions.csv)

## Project Layout
```
src/main.cpp          - firmware
include/pin_config.h  - board pin definitions
lib/                  - Waveshare vendor libraries (copied from examples repo)
data/                 - LittleFS partition content (generated, .gitignored)
  g/                  - Gemini sprite frames (i0.bin, rr0.bin, etc.)
  d/                  - Duo sprite frames
tools/convert_sprites.py - sprite sheet to RGB565 converter
waveshare-examples/   - cloned reference repo (.gitignored)
```

## Dev Notes
- Sprite source files: `~/Desktop/hendo_gemini_v3.png` and `~/Desktop/hendo_duo_v1.png`
- Duo sprite has magenta background, converter color-keys it out
- FT3168 touch address: 0x38, uses interrupt on GPIO21
- XCA9554 I2C expander at 0x20 controls display/touch power and reset
- ARDUINO_USB_CDC_ON_BOOT=1 makes `Serial` use USB CDC (no HWCDC workaround needed)
- Frame data: each .bin file is exactly 79,872 bytes (192 * 208 * 2)
- PSRAM allocation: 8 frame buffers pre-allocated (~624KB total)
