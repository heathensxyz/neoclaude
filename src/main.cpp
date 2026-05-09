#include <Arduino.h>
#include <LittleFS.h>
#include <Wire.h>
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include <Adafruit_XCA9554.h>
#include "pin_config.h"

#define FRAME_W 192
#define FRAME_H 208
#define FRAME_BYTES (FRAME_W * FRAME_H * 2)
#define MAX_FRAMES 8
#define ANIM_COUNT 9

#define DISPLAY_X ((LCD_WIDTH - FRAME_W) / 2)
#define DISPLAY_Y ((LCD_HEIGHT - FRAME_H) / 2)

#define FRAME_MS 120
#define TAP_THRESHOLD 400
#define LONG_PRESS_MS 800

struct AnimDef {
    const char* prefix;
    uint8_t gemini_frames;
    uint8_t duo_frames;
    bool looping;
};

const AnimDef ANIMS[ANIM_COUNT] = {
    {"i",  6, 6, true},   // idle
    {"rr", 8, 8, true},   // run right
    {"rl", 8, 8, true},   // run left
    {"w",  4, 4, false},  // wave
    {"j",  8, 5, false},  // jump
    {"s",  8, 8, false},  // sad
    {"wt", 6, 6, true},   // wait
    {"wk", 6, 6, true},   // work
    {"th", 6, 6, true},   // think
};

const char* SPRITE_DIRS[] = {"/g", "/d"};
const char* SPRITE_NAMES[] = {"Gemini", "Duo"};

Adafruit_XCA9554 expander;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_SH8601 *gfx = new Arduino_SH8601(
    bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT);

std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus =
    std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);

void touch_isr(void);

std::unique_ptr<Arduino_IIC> touch(new Arduino_FT3x68(
    IIC_Bus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TP_INT, touch_isr));

void touch_isr(void) {
    touch->IIC_Interrupt_Flag = true;
}

uint16_t* frame_buf[MAX_FRAMES];
uint8_t cur_sprite = 0;
uint8_t cur_anim = 0;
uint8_t cur_frame = 0;
uint8_t loaded_count = 0;
unsigned long last_frame_ms = 0;
bool anim_done = false;

unsigned long touch_down_ms = 0;
bool touch_active = false;

uint8_t frame_count_for(uint8_t sprite, uint8_t anim) {
    return sprite == 0 ? ANIMS[anim].gemini_frames : ANIMS[anim].duo_frames;
}

bool load_animation(uint8_t sprite, uint8_t anim) {
    uint8_t count = frame_count_for(sprite, anim);
    char path[24];

    for (uint8_t i = 0; i < count && i < MAX_FRAMES; i++) {
        snprintf(path, sizeof(path), "%s/%s%d.bin", SPRITE_DIRS[sprite], ANIMS[anim].prefix, i);

        File f = LittleFS.open(path, "r");
        if (!f) {
            Serial.printf("Failed to open %s\n", path);
            return false;
        }
        f.read((uint8_t*)frame_buf[i], FRAME_BYTES);
        f.close();
    }

    loaded_count = count;
    cur_frame = 0;
    anim_done = false;
    last_frame_ms = millis();

    Serial.printf("Loaded %s %s (%d frames)\n",
                  SPRITE_NAMES[sprite], ANIMS[anim].prefix, count);
    return true;
}

void draw_current_frame() {
    if (loaded_count == 0) return;
    gfx->draw16bitRGBBitmap(DISPLAY_X, DISPLAY_Y,
                            frame_buf[cur_frame], FRAME_W, FRAME_H);
}

void switch_anim(uint8_t anim) {
    if (anim >= ANIM_COUNT) anim = 0;
    cur_anim = anim;
    load_animation(cur_sprite, cur_anim);
    draw_current_frame();
}

void switch_sprite() {
    cur_sprite = 1 - cur_sprite;
    load_animation(cur_sprite, cur_anim);
    draw_current_frame();
    Serial.printf("Switched to %s\n", SPRITE_NAMES[cur_sprite]);
}

void play_reaction(uint8_t anim) {
    switch_anim(anim);
}

void return_to_idle() {
    switch_anim(0);
}

void handle_tap() {
    static uint8_t tap_anim = 0;
    const uint8_t tap_cycle[] = {3, 4, 8, 7, 6};  // wave, jump, think, work, wait
    play_reaction(tap_cycle[tap_anim]);
    tap_anim = (tap_anim + 1) % 5;
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("Hendo ESP32 starting...");

    Wire.begin(IIC_SDA, IIC_SCL);

    if (!expander.begin(0x20)) {
        Serial.println("XCA9554 not found");
        while (1) delay(1000);
    }

    expander.pinMode(0, OUTPUT);
    expander.pinMode(1, OUTPUT);
    expander.pinMode(2, OUTPUT);
    expander.digitalWrite(0, LOW);
    expander.digitalWrite(1, LOW);
    expander.digitalWrite(2, LOW);
    delay(20);
    expander.digitalWrite(0, HIGH);
    expander.digitalWrite(1, HIGH);
    expander.digitalWrite(2, HIGH);

    while (!touch->begin()) {
        Serial.println("Touch init failed, retrying...");
        delay(1000);
    }
    Serial.println("Touch OK");

    touch->IIC_Write_Device_State(
        touch->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
        touch->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);

    gfx->begin();
    gfx->fillScreen(0x0000);

    for (int i = 0; i <= 200; i += 5) {
        gfx->setBrightness(i);
        delay(3);
    }
    Serial.println("Display OK");

    if (!LittleFS.begin(false)) {
        Serial.println("LittleFS mount failed");
        gfx->setCursor(20, LCD_HEIGHT / 2);
        gfx->setTextColor(0xFFFF);
        gfx->setTextSize(2);
        gfx->println("No sprite data!");
        gfx->println("Run: pio run -t uploadfs");
        while (1) delay(1000);
    }
    Serial.println("LittleFS OK");

    for (uint8_t i = 0; i < MAX_FRAMES; i++) {
        frame_buf[i] = (uint16_t*)ps_malloc(FRAME_BYTES);
        if (!frame_buf[i]) {
            Serial.printf("PSRAM alloc failed for frame %d\n", i);
            while (1) delay(1000);
        }
    }
    Serial.printf("Allocated %d frame buffers in PSRAM\n", MAX_FRAMES);

    load_animation(0, 0);
    draw_current_frame();

    Serial.println("Ready! Tap to interact, long press to switch sprites.");
}

void loop() {
    unsigned long now = millis();

    if (touch->IIC_Interrupt_Flag) {
        touch->IIC_Interrupt_Flag = false;

        int32_t tx = touch->IIC_Read_Device_Value(
            touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
        int32_t ty = touch->IIC_Read_Device_Value(
            touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);

        if (!touch_active) {
            touch_active = true;
            touch_down_ms = now;
        }
    } else if (touch_active) {
        unsigned long held = now - touch_down_ms;
        touch_active = false;

        if (held >= LONG_PRESS_MS) {
            switch_sprite();
        } else if (held < TAP_THRESHOLD) {
            handle_tap();
        }
    }

    if (loaded_count > 0 && !anim_done && (now - last_frame_ms >= FRAME_MS)) {
        cur_frame++;
        if (cur_frame >= loaded_count) {
            if (ANIMS[cur_anim].looping) {
                cur_frame = 0;
            } else {
                cur_frame = loaded_count - 1;
                anim_done = true;
                return_to_idle();
                return;
            }
        }
        draw_current_frame();
        last_frame_ms = now;
    }

    delay(5);
}
