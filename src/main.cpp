#include <Arduino.h>
#include <LittleFS.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include <Adafruit_XCA9554.h>
#include "pin_config.h"
#include "XPowersLib.h"
#include "wifi_config.h"

#define FRAME_W 192
#define FRAME_H 208
#define FRAME_BYTES (FRAME_W * FRAME_H * 2)
#define MAX_FRAMES 8
#define ANIM_COUNT 9

#define CROP_X 4
#define SCALED_W (((FRAME_W) - (CROP_X) * 2) * 2)
#define SCALED_H ((FRAME_H) * 2)
#define SCALED_BYTES (SCALED_W * SCALED_H * 2)
#define DISPLAY_X 0
#define DISPLAY_Y ((LCD_HEIGHT - SCALED_H) / 2)

#define FRAME_MS 120
#define TOUCH_DEBOUNCE_MS 250
#define SWITCH_ZONE_Y 100

#define SIDE_BUTTON_PIN 4
#define PMU_IRQ_PIN 5
#define PERM_TIMEOUT_MS 30000
#define WIFI_RECONNECT_MS 30000

#define PERM_GREEN  0x2E8B
#define PERM_RED    0xD8A6
#define PERM_WHITE  0xFFFF
#define PERM_GRAY   0x8410
#define PERM_CREAM  0xFFF5
#define PERM_DARK   0x2104
#define PERM_BUTTON_Y 348
#define PERM_BUTTON_H 70
#define PERM_BUTTON_R 12
#define PERM_DIVIDER_X (LCD_WIDTH / 2)

#define SPRITE_COUNT 2

struct AnimDef {
    const char* prefix;
    uint8_t frames[SPRITE_COUNT];
    bool looping;
};

const AnimDef ANIMS[ANIM_COUNT] = {
    {"i",  {6, 6}, true},
    {"rr", {8, 8}, true},
    {"rl", {8, 8}, true},
    {"w",  {4, 4}, false},
    {"j",  {8, 5}, false},
    {"s",  {8, 8}, false},
    {"wt", {6, 6}, true},
    {"wk", {6, 6}, true},
    {"th", {6, 6}, true},
};

const char* SPRITE_DIRS[] = {"/h", "/g"};
const char* SPRITE_NAMES[] = {"Pet 1", "Pet 2"};

Adafruit_XCA9554 expander;
XPowersPMU power;
WebServer server(80);
bool pmu_available = false;
bool wifi_connected = false;

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
uint16_t* scaled_buf;
uint8_t cur_sprite = 0;
uint8_t cur_anim = 0;
uint8_t cur_frame = 0;
uint8_t loaded_count = 0;
unsigned long last_frame_ms = 0;
bool anim_done = false;
unsigned long last_tap_ms = 0;
unsigned long last_wifi_check = 0;

enum PermState { PERM_IDLE, PERM_PENDING, PERM_DECIDED };
volatile PermState perm_state = PERM_IDLE;
volatile bool perm_decision = false;
String perm_tool_name;
String perm_tool_summary;

uint8_t frame_count_for(uint8_t sprite, uint8_t anim) {
    return ANIMS[anim].frames[sprite];
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

void scale_and_draw(uint8_t frame_idx) {
    if (loaded_count == 0) return;

    const uint16_t* src = frame_buf[frame_idx];

    for (int sy = 0; sy < FRAME_H; sy++) {
        for (int sx = CROP_X; sx < FRAME_W - CROP_X; sx++) {
            uint16_t pixel = src[sy * FRAME_W + sx];
            int dx = (sx - CROP_X) * 2;
            int dy = sy * 2;
            scaled_buf[dy * SCALED_W + dx] = pixel;
            scaled_buf[dy * SCALED_W + dx + 1] = pixel;
            scaled_buf[(dy + 1) * SCALED_W + dx] = pixel;
            scaled_buf[(dy + 1) * SCALED_W + dx + 1] = pixel;
        }
    }

    gfx->draw16bitRGBBitmap(DISPLAY_X, DISPLAY_Y, scaled_buf, SCALED_W, SCALED_H);
}

void switch_anim(uint8_t anim) {
    if (anim >= ANIM_COUNT) anim = 0;
    cur_anim = anim;
    load_animation(cur_sprite, cur_anim);
    scale_and_draw(cur_frame);
}

void switch_sprite() {
    cur_sprite = (cur_sprite + 1) % SPRITE_COUNT;
    load_animation(cur_sprite, cur_anim);
    scale_and_draw(cur_frame);
    Serial.printf("Switched to %s\n", SPRITE_NAMES[cur_sprite]);
}

void return_to_idle() {
    switch_anim(0);
}

void handle_tap(int32_t ty) {
    if (ty < SWITCH_ZONE_Y) {
        switch_sprite();
    } else {
        static uint8_t tap_anim = 0;
        const uint8_t tap_cycle[] = {3, 4, 8, 7, 6};
        switch_anim(tap_cycle[tap_anim]);
        tap_anim = (tap_anim + 1) % 5;
    }
}

void resume_pet_animation() {
    gfx->fillScreen(0x0000);
    load_animation(cur_sprite, cur_anim);
    scale_and_draw(cur_frame);
}

void draw_text_centered_in(const char* text, int x, int w_zone, int y, uint8_t size, uint16_t color) {
    gfx->setTextSize(size);
    gfx->setTextColor(color);
    int16_t x1, y1;
    uint16_t tw, th;
    gfx->getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
    gfx->setCursor(x + (w_zone - tw) / 2, y);
    gfx->print(text);
}

void draw_rounded_rect(int x, int y, int w, int h, int r, uint16_t color) {
    gfx->fillRect(x + r, y, w - 2 * r, h, color);
    gfx->fillRect(x, y + r, r, h - 2 * r, color);
    gfx->fillRect(x + w - r, y + r, r, h - 2 * r, color);
    gfx->fillCircle(x + r, y + r, r, color);
    gfx->fillCircle(x + w - r - 1, y + r, r, color);
    gfx->fillCircle(x + r, y + h - r - 1, r, color);
    gfx->fillCircle(x + w - r - 1, y + h - r - 1, r, color);
}

void draw_small_pet(int cx, int cy) {
    if (loaded_count == 0) return;
    const uint16_t* src = frame_buf[0];
    int draw_w = (FRAME_W - CROP_X * 2);
    int sx_start = cx - draw_w / 2;
    int sy_start = cy - FRAME_H / 2;
    for (int sy = 0; sy < FRAME_H; sy++) {
        for (int sx = CROP_X; sx < FRAME_W - CROP_X; sx++) {
            uint16_t pixel = src[sy * FRAME_W + sx];
            if (pixel != 0x0000) {
                int dx = sx_start + (sx - CROP_X);
                int dy = sy_start + sy;
                if (dx >= 0 && dx < LCD_WIDTH && dy >= 0 && dy < LCD_HEIGHT)
                    gfx->drawPixel(dx, dy, pixel);
            }
        }
    }
}

void draw_permission_screen() {
    gfx->fillScreen(PERM_DARK);

    draw_text_centered_in("Allow this?", 0, LCD_WIDTH, 30, 2, PERM_CREAM);

    gfx->drawFastHLine(30, 52, LCD_WIDTH - 60, PERM_GRAY);

    draw_rounded_rect(15, 70, LCD_WIDTH - 30, 50, 8, 0x3186);
    draw_text_centered_in(perm_tool_name.c_str(), 0, LCD_WIDTH, 86, 3, PERM_WHITE);

    gfx->setTextSize(2);
    gfx->setTextColor(PERM_CREAM);
    int y = 140;
    int char_w = 12;
    int chars_per_line = (LCD_WIDTH - 40) / char_w;
    for (int i = 0; i < (int)perm_tool_summary.length() && y < 330; i += chars_per_line) {
        String line = perm_tool_summary.substring(i, min((int)perm_tool_summary.length(), i + chars_per_line));
        gfx->setCursor(20, y);
        gfx->print(line.c_str());
        y += 22;
    }

    int btn_gap = 12;
    int btn_w = (LCD_WIDTH - 30 - btn_gap) / 2;
    int btn_x1 = 15;
    int btn_x2 = 15 + btn_w + btn_gap;

    draw_rounded_rect(btn_x1, PERM_BUTTON_Y, btn_w, PERM_BUTTON_H, PERM_BUTTON_R, PERM_GREEN);
    draw_rounded_rect(btn_x2, PERM_BUTTON_Y, btn_w, PERM_BUTTON_H, PERM_BUTTON_R, PERM_RED);

    draw_text_centered_in("APPROVE", btn_x1, btn_w, PERM_BUTTON_Y + 24, 2, PERM_WHITE);
    draw_text_centered_in("DENY", btn_x2, btn_w, PERM_BUTTON_Y + 24, 2, PERM_WHITE);
}

void draw_decision_feedback(bool approved) {
    uint16_t color = approved ? PERM_GREEN : PERM_RED;
    const char* text = approved ? "APPROVED" : "DENIED";
    gfx->fillScreen(PERM_DARK);
    draw_rounded_rect(40, LCD_HEIGHT / 2 - 40, LCD_WIDTH - 80, 80, 16, color);
    draw_text_centered_in(text, 0, LCD_WIDTH, LCD_HEIGHT / 2 - 10, 2, PERM_WHITE);
}

String extract_tool_summary(const char* tool_name, JsonDocument& doc) {
    JsonVariant input = doc["tool_input"];
    if (!input.is<JsonObject>()) return "(no details)";

    if (strcmp(tool_name, "Bash") == 0) {
        const char* cmd = input["command"];
        if (cmd) return String(cmd).substring(0, 250);
    }

    if (strcmp(tool_name, "Write") == 0 || strcmp(tool_name, "Edit") == 0) {
        const char* path = input["file_path"];
        if (path) return String(path).substring(0, 250);
    }

    if (strcmp(tool_name, "Read") == 0) {
        const char* path = input["file_path"];
        if (path) return String(path).substring(0, 250);
    }

    String summary;
    serializeJson(input, summary);
    return summary.substring(0, 250);
}

bool poll_approve_deny() {
    if (expander.digitalRead(SIDE_BUTTON_PIN) == HIGH) {
        delay(50);
        if (expander.digitalRead(SIDE_BUTTON_PIN) == HIGH) {
            perm_decision = true;
            return true;
        }
    }

    if (pmu_available) {
        int pmu_pin = expander.digitalRead(PMU_IRQ_PIN);
        if (pmu_pin == LOW) {
            uint32_t status = power.getIrqStatus();
            if (power.isPekeyShortPressIrq()) {
                perm_decision = false;
                power.clearIrqStatus();
                return true;
            }
            power.clearIrqStatus();
        }
    }

    if (touch->IIC_Interrupt_Flag) {
        touch->IIC_Interrupt_Flag = false;
        int32_t tx = touch->IIC_Read_Device_Value(
            touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
        int32_t ty = touch->IIC_Read_Device_Value(
            touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);

        if (ty >= PERM_BUTTON_Y && ty <= (PERM_BUTTON_Y + PERM_BUTTON_H)) {
            if (tx < PERM_DIVIDER_X) {
                perm_decision = true;
                return true;
            } else {
                perm_decision = false;
                return true;
            }
        }
    }

    return false;
}

void handle_permission_post() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"no body\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        server.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }

    perm_tool_name = doc["tool_name"] | "unknown";
    perm_tool_summary = extract_tool_summary(perm_tool_name.c_str(), doc);
    perm_state = PERM_PENDING;
    perm_decision = false;
    unsigned long start = millis();

    Serial.printf("Permission request: %s - %s\n", perm_tool_name.c_str(), perm_tool_summary.c_str());

    draw_permission_screen();

    // Clear stale input before polling
    touch->IIC_Interrupt_Flag = false;
    if (pmu_available) {
        power.getIrqStatus();
        power.clearIrqStatus();
    }
    while (expander.digitalRead(SIDE_BUTTON_PIN) == HIGH) {
        delay(20);
    }
    delay(200);

    while (millis() - start < PERM_TIMEOUT_MS) {
        if (poll_approve_deny()) {
            perm_state = PERM_DECIDED;
            break;
        }
        delay(20);
    }

    if (perm_state != PERM_DECIDED) {
        Serial.println("Permission timeout");
        server.send(408, "application/json", "{\"error\":\"timeout\"}");
        perm_state = PERM_IDLE;
        resume_pet_animation();
        return;
    }

    Serial.printf("Permission %s\n", perm_decision ? "APPROVED" : "DENIED");

    JsonDocument resp;
    resp["hookSpecificOutput"]["hookEventName"] = "PermissionRequest";
    resp["hookSpecificOutput"]["decision"]["behavior"] = perm_decision ? "allow" : "deny";
    String response;
    serializeJson(resp, response);

    server.send(200, "application/json", response);

    draw_decision_feedback(perm_decision);
    delay(500);

    perm_state = PERM_IDLE;
    resume_pet_animation();
}

void handle_health() {
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("NeoClaude starting...");

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

    expander.pinMode(SIDE_BUTTON_PIN, INPUT);
    expander.pinMode(PMU_IRQ_PIN, INPUT);

    pmu_available = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
    if (!pmu_available) {
        Serial.println("PMU not found (buttons may be limited)");
    } else {
        power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
        power.clearIrqStatus();
        power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
        Serial.println("PMU OK, power key enabled");
    }

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

    scaled_buf = (uint16_t*)ps_malloc(SCALED_BYTES);
    if (!scaled_buf) {
        Serial.println("PSRAM alloc failed for scaled buffer");
        while (1) delay(1000);
    }
    memset(scaled_buf, 0, SCALED_BYTES);

    Serial.printf("Allocated %d frame buffers + scaled buffer in PSRAM\n", MAX_FRAMES);

    load_animation(0, 0);
    scale_and_draw(0);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting to WiFi");
    unsigned long wifi_start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifi_start < 10000) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifi_connected = true;
        Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());

        if (MDNS.begin("neoclaude")) {
            Serial.println("mDNS: neoclaude.local");
        }

        server.on("/permission", HTTP_POST, handle_permission_post);
        server.on("/health", HTTP_GET, handle_health);
        server.begin();
        Serial.println("HTTP server on port 80");
    } else {
        Serial.println("\nWiFi failed, running offline");
    }

    touch->IIC_Interrupt_Flag = false;
    last_tap_ms = millis();
    Serial.println("Ready!");
}

void loop() {
    unsigned long now = millis();

    if (wifi_connected) {
        server.handleClient();

        if (now - last_wifi_check > WIFI_RECONNECT_MS) {
            last_wifi_check = now;
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("WiFi reconnecting...");
                WiFi.reconnect();
            }
        }
    }

    if (perm_state != PERM_IDLE) {
        return;
    }

    if (touch->IIC_Interrupt_Flag) {
        touch->IIC_Interrupt_Flag = false;

        if (now - last_tap_ms > TOUCH_DEBOUNCE_MS) {
            int32_t ty = touch->IIC_Read_Device_Value(
                touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);

            last_tap_ms = now;
            handle_tap(ty);
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
        scale_and_draw(cur_frame);
        last_frame_ms = now;
    }

    delay(5);
}
