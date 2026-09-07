#include "behaviors.h"
#include "config.h"
#include "state.h"
#include "settings.h"
#include "tracker.h"
#include "face.h"
#include "audio_io.h"
#include "net_link.h"
#include <M5Unified.h>
#include <M5StackChan.h>
#include <math.h>
#include <WiFi.h>
#include <ArduinoJson.h>

namespace leds {

static Pattern g_pat = Pattern::Off, g_prevPat = Pattern::Off;
static uint8_t g_r, g_g, g_b, g_pr, g_pg, g_pb;
static uint32_t g_until = 0, g_lastTick = 0;

void set(Pattern p, uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
    if (ms) { g_prevPat = g_pat; g_pr = g_r; g_pg = g_g; g_pb = g_b; g_until = millis() + ms; }
    else g_until = 0;
    g_pat = p; g_r = r; g_g = g; g_b = b;
}

static void hsv(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.f, 2) - 1)), m = v - c;
    float rr, gg, bb;
    if (h < 60) { rr = c; gg = x; bb = 0; } else if (h < 120) { rr = x; gg = c; bb = 0; } else if (h < 180) { rr = 0; gg = c; bb = x; }
    else if (h < 240) { rr = 0; gg = x; bb = c; } else if (h < 300) { rr = x; gg = 0; bb = c; } else { rr = c; gg = 0; bb = x; }
    r = (rr + m) * 255; g = (gg + m) * 255; b = (bb + m) * 255;
}

void tick() {
    uint32_t now = millis();
    if (now - g_lastTick < 40) return;
    g_lastTick = now;
    if (g_until && now > g_until) { g_pat = g_prevPat; g_r = g_pr; g_g = g_pg; g_b = g_pb; g_until = 0; }
    switch (g_pat) {
        case Pattern::Off: M5StackChan.showRgbColor(0, 0, 0); break;
        case Pattern::Solid: M5StackChan.showRgbColor(g_r, g_g, g_b); break;
        case Pattern::Pulse: { float k = 0.5f + 0.5f * sinf(now / 400.f); M5StackChan.showRgbColor(g_r * k, g_g * k, g_b * k); break; }
        case Pattern::Rainbow:
            for (int i = 0; i < 12; i++) { uint8_t r, g, b; hsv(fmodf(now / 12.f + i * 30, 360), 1, 0.35f, r, g, b); M5StackChan.setRgbColor(i, r, g, b); }
            M5StackChan.refreshRgb(); break;
        case Pattern::Listening: {   // red VU meter, both sides
            float l = g_state.micLevel; int n = (int)(l * 6 + 0.5f);
            for (int i = 0; i < 6; i++) { bool on = i < n; M5StackChan.setRgbColor(i, on ? 120 : 8, on ? 20 : 0, on ? 30 : 0); M5StackChan.setRgbColor(11 - i, on ? 120 : 8, on ? 20 : 0, on ? 30 : 0); }
            M5StackChan.refreshRgb(); break;
        }
        case Pattern::Thinking: {    // amber chaser
            int pos = (now / 90) % 6;
            for (int i = 0; i < 6; i++) { uint8_t v = i == pos ? 90 : (abs(i - pos) == 1 ? 25 : 2); M5StackChan.setRgbColor(i, v, v * 2 / 3, 0); M5StackChan.setRgbColor(11 - i, v, v * 2 / 3, 0); }
            M5StackChan.refreshRgb(); break;
        }
        case Pattern::Speaking: { float k = 0.15f + 0.85f * g_state.mouthOpen; M5StackChan.showRgbColor(20 * k, 90 * k, 140 * k); break; }
        case Pattern::Error: M5StackChan.showRgbColor(((now / 150) & 1) ? 120 : 0, 0, 0); break;
        case Pattern::Notify: { float k = 0.5f + 0.5f * sinf(now / 120.f); M5StackChan.showRgbColor(0, 90 * k, 40 * k); break; }
    }
}

}  // namespace leds

namespace behaviors {

static uint32_t g_nextIdle = 0;
static float g_pax, g_pay, g_paz;
static uint32_t g_lastShakePeak = 0; static int g_shakeCount = 0;
static uint32_t g_lastEvent = 0;

static void sendEvent(const char* name, const char* extra = nullptr) {
    JsonDocument d; d["type"] = "event"; d["name"] = name; if (extra) d["value"] = extra; net::sendJson(d);
}

void tick() {
    uint32_t now = millis();
    Mode m = g_state.mode.load();

    // --- head touch (Si12T three-zone panel on top of the head) ---
    auto& ts = M5StackChan.TouchSensor;
    if (ts.wasClicked()) {
        face::pokeReaction();
        g_state.setExpression(Expression::Happy, 1500);
        audio::playChime(1);
        if (m != Mode::Sleep) { tracker::nod(); sendEvent("head_tap"); }
    }
    if (ts.wasSwipedForward()) {
        g_settings.volume = min(255, g_settings.volume + 25); g_settings.save(); audio::setVolume(g_settings.volume);
        g_state.setCaption("volume " + String(g_settings.volume * 100 / 255) + "%", 1500); audio::playChime(1);
    }
    if (ts.wasSwipedBackward()) {
        g_settings.volume = max(20, g_settings.volume - 25); g_settings.save(); audio::setVolume(g_settings.volume);
        g_state.setCaption("volume " + String(g_settings.volume * 100 / 255) + "%", 1500); audio::playChime(1);
    }
    if (ts.wasHold()) {
        g_state.setExpression(Expression::Love, 2500);
        leds::set(leds::Pattern::Pulse, 120, 40, 80, 2500);
        sendEvent("head_hold");
    }

    // --- screen touch: tap = wink, hold = info ---
    auto t = M5.Touch.getDetail();
    if (t.wasClicked()) { g_state.setExpression(Expression::Wink, 900); face::pokeReaction(); }
    if (t.wasHold()) {
        String s = "ip " + WiFi.localIP().toString() + "  " + String(g_state.batteryV.load(), 2) + "V " + String((int)g_state.batteryMa.load()) + "mA  rssi " + String(g_state.rssi.load());
        g_state.setCaption(s, 5000);
    }

    // --- shake detection (BMI270) ---
    float ax, ay, az;
    if (M5.Imu.update() && M5.Imu.getAccel(&ax, &ay, &az)) {
        float diff = fabsf(ax - g_pax) + fabsf(ay - g_pay) + fabsf(az - g_paz);
        g_pax = ax; g_pay = ay; g_paz = az;
        if (diff > 1.6f && now - g_lastShakePeak > 100) {
            g_shakeCount = (now - g_lastShakePeak < 1000) ? g_shakeCount + 1 : 1;
            g_lastShakePeak = now;
            if (g_shakeCount >= 3 && now - g_lastEvent > 3000) {
                g_shakeCount = 0; g_lastEvent = now;
                g_state.setExpression(Expression::Surprised, 2000);
                g_state.setCaption("wheee!", 1500);
                leds::set(leds::Pattern::Rainbow, 0, 0, 0, 2000);
                sendEvent("shake");
            }
        }
    }

    // --- idle fidgets while nothing else is happening ---
    if (m == Mode::Standby && now > g_nextIdle) {
        g_nextIdle = now + IDLE_ANIM_MIN_MS + esp_random() % (IDLE_ANIM_MAX_MS - IDLE_ANIM_MIN_MS);
        if (!g_state.targetVisible) {
            int pick = esp_random() % 5;
            if (pick == 0) tracker::lookAt((esp_random() % 100 - 50) / 100.f, (esp_random() % 60 - 20) / 100.f, 200);
            else if (pick == 1) g_state.setExpression(Expression::Wink, 700);
            else if (pick == 2) { g_state.gazeX = (esp_random() % 200 - 100) / 100.f; g_state.gazeY = (esp_random() % 100 - 50) / 100.f; }
            else if (pick == 3) tracker::goHome();
        } else {
            if (esp_random() % 3 == 0) g_state.setExpression(Expression::Happy, 1200);
        }
    }
}

}  // namespace behaviors
