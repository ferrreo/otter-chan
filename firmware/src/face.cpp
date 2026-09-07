#include "face.h"
#include "config.h"
#include "state.h"
#include <M5Unified.h>
#include <math.h>

namespace {

M5Canvas g_canvas(&M5.Display);
std::atomic<bool> g_enabled{false};
SemaphoreHandle_t g_canvasMutex;
std::atomic<uint32_t> g_pokeUntil{0};
TaskHandle_t g_task;

constexpr int W = 320, H = 240;

// palette
uint16_t C_BG, C_FACE, C_FACE_DK, C_EYE, C_EYE_HI, C_PUPIL, C_MOUTH, C_BLUSH, C_ANT, C_TXT, C_DIM, C_ACC;

struct Anim {
    float blink = 0;           // 0 open .. 1 closed
    uint32_t nextBlinkMs = 0;
    uint32_t blinkStart = 0;
    float gx = 0, gy = 0;      // smoothed gaze
    float breathe = 0;
    float mouth = 0;
    float wobble = 0;
    uint32_t t0 = 0;
} A;

inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return M5.Display.color565(r, g, b); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float easeOut(float t) { return 1 - (1 - t) * (1 - t); }

void initPalette() {
    C_BG      = rgb(16, 18, 30);
    C_FACE    = rgb(238, 240, 250);
    C_FACE_DK = rgb(200, 205, 225);
    C_EYE     = rgb(30, 34, 52);
    C_EYE_HI  = rgb(255, 255, 255);
    C_PUPIL   = rgb(80, 200, 255);
    C_MOUTH   = rgb(40, 44, 64);
    C_BLUSH   = rgb(255, 150, 170);
    C_ANT     = rgb(255, 200, 60);
    C_TXT     = rgb(230, 232, 240);
    C_DIM     = rgb(120, 125, 150);
    C_ACC     = rgb(120, 230, 255);
}

// ---- drawing helpers ----
void eye(int cx, int cy, int rx, int ry, float open, float gx, float gy, Expression e, bool left) {
    // eye white
    int ryo = max(2, (int)(ry * open));
    if (e == Expression::Happy || e == Expression::Love) {
        // happy arcs (closed smiling eyes)
        g_canvas.fillEllipse(cx, cy, rx, ry, C_EYE);
        g_canvas.fillEllipse(cx, cy + ry / 2 + 2, rx + 2, ry, C_FACE);
        if (e == Expression::Love) {
            // heart pupils
            uint16_t pink = rgb(255, 90, 130);
            g_canvas.fillCircle(cx - rx / 3, cy - 2, rx / 3, pink);
            g_canvas.fillCircle(cx + rx / 3, cy - 2, rx / 3, pink);
            g_canvas.fillTriangle(cx - rx * 2 / 3, cy, cx + rx * 2 / 3, cy, cx, cy + ry * 2 / 3, pink);
        }
        return;
    }
    if (e == Expression::Wink && !left) {
        g_canvas.fillRoundRect(cx - rx, cy - 3, rx * 2, 6, 3, C_EYE);
        return;
    }
    if (e == Expression::Sleepy) { ryo = max(3, ry / 4); }
    g_canvas.fillEllipse(cx, cy, rx, ryo, C_EYE);
    if (ryo > 6) {
        int px = cx + (int)(gx * rx * 0.45f), py = cy + (int)(gy * ryo * 0.45f);
        int pr = min(rx, ryo) * 2 / 3;
        if (e == Expression::Surprised) pr = min(rx, ryo) / 2;
        g_canvas.fillCircle(px, py, pr, C_PUPIL);
        g_canvas.fillCircle(px, py, pr * 2 / 3, C_EYE);
        g_canvas.fillCircle(px - pr / 3, py - pr / 3, max(2, pr / 3), C_EYE_HI);
    }
    // lids for moods
    if (e == Expression::Angry) {
        int dir = left ? 1 : -1;
        g_canvas.fillTriangle(cx - rx - 2, cy - ryo - 2, cx + rx + 2, cy - ryo - 2, cx + dir * rx, cy - ryo / 3, C_FACE);
    } else if (e == Expression::Sad) {
        int dir = left ? -1 : 1;
        g_canvas.fillTriangle(cx - rx - 2, cy - ryo - 2, cx + rx + 2, cy - ryo - 2, cx + dir * rx, cy - ryo / 3, C_FACE);
    } else if (e == Expression::Confused && left) {
        g_canvas.fillRect(cx - rx - 2, cy - ryo - 2, rx * 2 + 4, ryo / 2, C_FACE);
    }
}

void mouth(int cx, int cy, float open, Expression e, Mode m) {
    int w = 44;
    if (m == Mode::Speaking || open > 0.05f) {
        int h = 4 + (int)(open * 26);
        g_canvas.fillEllipse(cx, cy + h / 2, w / 2 + (int)(open * 6), h, C_MOUTH);
        if (h > 12) g_canvas.fillEllipse(cx, cy + h / 2 + h / 3, w / 3, h / 3, rgb(240, 120, 140));
        return;
    }
    switch (e) {
        case Expression::Happy: case Expression::Love: case Expression::Wink:
            g_canvas.fillEllipse(cx, cy, w / 2, 14, C_MOUTH);
            g_canvas.fillEllipse(cx, cy - 6, w / 2 + 2, 12, C_FACE);
            break;
        case Expression::Sad:
            g_canvas.fillEllipse(cx, cy + 12, w / 2, 12, C_MOUTH);
            g_canvas.fillEllipse(cx, cy + 18, w / 2 + 2, 12, C_FACE);
            break;
        case Expression::Surprised:
            g_canvas.fillEllipse(cx, cy + 4, 12, 14, C_MOUTH);
            break;
        case Expression::Angry:
            g_canvas.fillRoundRect(cx - w / 2, cy, w, 6, 3, C_MOUTH);
            break;
        case Expression::Confused:
            for (int i = 0; i < w; i += 4) g_canvas.fillRect(cx - w / 2 + i, cy + (int)(3 * sinf(i * 0.5f)), 3, 4, C_MOUTH);
            break;
        case Expression::Thinking:
            g_canvas.fillRoundRect(cx - 10, cy + 2, 20, 5, 2, C_MOUTH);
            break;
        case Expression::Sleepy:
            g_canvas.fillEllipse(cx, cy + 4, 8, 6, C_MOUTH);
            break;
        case Expression::Error:
            g_canvas.drawLine(cx - 14, cy - 6, cx + 14, cy + 8, C_MOUTH);
            g_canvas.drawLine(cx - 14, cy + 8, cx + 14, cy - 6, C_MOUTH);
            break;
        default: {
            int lvl = (int)(open * 10);
            g_canvas.fillRoundRect(cx - w / 2, cy, w, 5 + lvl, 3, C_MOUTH);
            g_canvas.fillRoundRect(cx - w / 2 + 3, cy - 3, w - 6, 5, 3, C_FACE);
        }
    }
}

void statusBar(Mode m) {
    // top strip: wifi / server / mic / battery
    int x = 6, y = 4;
    uint16_t wifiC = g_state.wifiOk ? C_ACC : rgb(255, 80, 80);
    for (int i = 0; i < 4; i++) g_canvas.fillRect(x + i * 5, y + 10 - i * 3, 3, 2 + i * 3, (g_state.wifiOk && (i < (g_state.rssi + 100) / 15 + 1)) ? wifiC : C_DIM);
    x += 26;
    g_canvas.fillCircle(x + 4, y + 6, 4, g_state.serverOk ? rgb(90, 230, 120) : rgb(255, 120, 60));
    x += 16;
    // mic glyph
    uint16_t micC = (m == Mode::Listening) ? rgb(255, 90, 90) : (m == Mode::Muted ? rgb(120, 120, 120) : (m == Mode::Standby ? rgb(120, 200, 255) : C_DIM));
    g_canvas.fillRoundRect(x + 2, y, 6, 10, 3, micC);
    g_canvas.drawLine(x + 5, y + 10, x + 5, y + 13, micC);
    if (m == Mode::Muted) g_canvas.drawLine(x, y + 13, x + 10, y, rgb(255, 80, 80));
    if (m == Mode::Listening) { float l = g_state.micLevel; g_canvas.fillRect(x + 12, y + 12 - (int)(l * 12), 3, (int)(l * 12) + 1, micC); }
    // battery
    int pct = g_state.batteryPct;
    int bx = W - 34, by = 4;
    g_canvas.drawRoundRect(bx, by, 26, 12, 2, C_DIM);
    g_canvas.fillRect(bx + 26, by + 3, 2, 6, C_DIM);
    if (pct >= 0) {
        uint16_t bc = pct > 30 ? rgb(90, 230, 120) : rgb(255, 120, 60);
        if (g_state.charging) bc = C_ACC;
        g_canvas.fillRect(bx + 2, by + 2, max(1, 22 * pct / 100), 8, bc);
    }
    // status word
    g_canvas.setTextDatum(top_right);
    g_canvas.setTextColor(C_DIM, C_BG);
    g_canvas.setFont(&fonts::Font0);
    g_canvas.setTextSize(1);
    g_state.lock();
    String s = g_state.statusLine.length() ? g_state.statusLine : String(modeName(m));
    g_state.unlock();
    g_canvas.drawString(s, bx - 6, by + 2);
}

const char* activityGlyph(const char* a) {
    if (!strcmp(a, "coding")) return ">_";
    if (!strcmp(a, "browsing")) return "www";
    if (!strcmp(a, "thinking")) return "...";
    if (!strcmp(a, "talking")) return "\"\"";
    if (!strcmp(a, "writing")) return "~";
    if (!strcmp(a, "error")) return "!!";
    if (!strcmp(a, "offline")) return "zz";
    if (!strcmp(a, "done")) return "ok";
    return "*";
}

uint16_t activityColor(const char* a) {
    if (!strcmp(a, "coding")) return rgb(120, 230, 255);
    if (!strcmp(a, "browsing")) return rgb(150, 255, 150);
    if (!strcmp(a, "thinking")) return rgb(255, 220, 100);
    if (!strcmp(a, "talking")) return rgb(255, 160, 220);
    if (!strcmp(a, "error")) return rgb(255, 90, 90);
    if (!strcmp(a, "offline")) return rgb(110, 110, 130);
    if (!strcmp(a, "done")) return rgb(120, 255, 180);
    return rgb(200, 200, 220);
}

void botsRow(uint32_t now) {
    g_state.lock();
    int n = g_state.botCount;
    BotStatus bots[MAX_BOTS];
    for (int i = 0; i < n; i++) bots[i] = g_state.bots[i];
    g_state.unlock();
    if (n == 0) return;
    int cardW = min(100, (W - 8) / n - 4);
    int x0 = (W - (cardW + 4) * n) / 2 + 2;
    int y = H - 34;
    g_canvas.setFont(&fonts::Font0);
    g_canvas.setTextSize(1);
    for (int i = 0; i < n; i++) {
        int x = x0 + i * (cardW + 4);
        uint16_t c = activityColor(bots[i].activity);
        bool live = now - bots[i].updatedMs < 120000;
        g_canvas.fillRoundRect(x, y, cardW, 30, 6, rgb(28, 31, 48));
        g_canvas.drawRoundRect(x, y, cardW, 30, 6, live ? c : C_DIM);
        // little animated indicator
        float ph = fmodf(now / 400.f + i, 2.f);
        if (!strcmp(bots[i].activity, "coding")) {
            g_canvas.fillRect(x + 6, y + 8, 3, 12, (ph < 1.f) ? c : rgb(28, 31, 48));   // blinking cursor
        } else if (!strcmp(bots[i].activity, "thinking")) {
            for (int d = 0; d < 3; d++) g_canvas.fillCircle(x + 6 + d * 5, y + 14 + (int)(2 * sinf(now / 150.f + d)), 1, c);
        } else if (!strcmp(bots[i].activity, "browsing")) {
            g_canvas.drawCircle(x + 8, y + 14, 5, c); g_canvas.drawLine(x + 3, y + 14, x + 13, y + 14, c);
            g_canvas.drawEllipse(x + 8, y + 14, 2, 5, c);
        } else if (!strcmp(bots[i].activity, "talking")) {
            int h = 3 + (int)(5 * fabsf(sinf(now / 120.f + i)));
            g_canvas.fillRoundRect(x + 5, y + 14 - h / 2, 7, h, 2, c);
        } else if (!strcmp(bots[i].activity, "offline")) {
            g_canvas.drawString("z", x + 5, y + 8);
        } else {
            g_canvas.fillCircle(x + 8, y + 14, 3 + (int)(1.5f * sinf(now / 300.f + i)), c);
        }
        g_canvas.setTextDatum(top_left);
        g_canvas.setTextColor(C_TXT, rgb(28, 31, 48));
        char nm[12]; strncpy(nm, bots[i].name, 11); nm[11] = 0;
        g_canvas.drawString(nm, x + 18, y + 4);
        g_canvas.setTextColor(c, rgb(28, 31, 48));
        char det[18]; strncpy(det, bots[i].detail[0] ? bots[i].detail : bots[i].activity, 17); det[17] = 0;
        g_canvas.drawString(det, x + 18, y + 17);
    }
}

void caption(uint32_t now) {
    g_state.lock();
    String s = g_state.caption;
    bool show = s.length() && (g_state.captionUntilMs == 0 || now < g_state.captionUntilMs);
    g_state.unlock();
    if (!show) return;
    g_canvas.setFont(&fonts::Font0);
    g_canvas.setTextSize(1);
    g_canvas.setTextDatum(top_center);
    g_canvas.setTextColor(C_TXT, C_BG);
    // wrap into up to 2 lines of ~50 chars
    int maxc = 52;
    String l1 = s.substring(0, min((int)s.length(), maxc));
    String l2 = s.length() > maxc ? s.substring(maxc, min((int)s.length(), maxc * 2)) : "";
    int y = g_state.botCount ? H - 58 : H - 40;
    g_canvas.drawString(l1, W / 2, y);
    if (l2.length()) g_canvas.drawString(l2, W / 2, y + 11);
}

void drawFrame(uint32_t now) {
    Mode m = g_state.mode.load();
    Expression e = g_state.expression.load();
    uint32_t until = g_state.expressionUntilMs.load();
    if (until && now > until) { g_state.expression = Expression::Neutral; g_state.expressionUntilMs = 0; e = Expression::Neutral; }
    if (m == Mode::Thinking && e == Expression::Neutral) e = Expression::Thinking;
    if (m == Mode::Sleep) e = Expression::Sleepy;

    // blink
    if (now > A.nextBlinkMs) { A.blinkStart = now; A.nextBlinkMs = now + 2500 + (esp_random() % 4000); }
    float bt = (now - A.blinkStart) / 140.f;
    A.blink = bt < 1.f ? sinf(bt * PI) : 0.f;
    if (m == Mode::Sleep) A.blink = 1.f;
    // gaze smoothing
    float tgx = g_state.gazeX.load(), tgy = g_state.gazeY.load();
    if (m == Mode::Listening) { tgx *= 0.5f; tgy = -0.2f; }
    A.gx = lerp(A.gx, tgx, 0.18f); A.gy = lerp(A.gy, tgy, 0.18f);
    A.breathe = sinf(now / 900.f) * 2.5f;
    A.mouth = lerp(A.mouth, g_state.mouthOpen.load(), 0.5f);
    bool poked = now < g_pokeUntil;
    float pokeT = poked ? 1.f - (g_pokeUntil - now) / 500.f : 1.f;
    float squash = poked ? 1.f - 0.12f * sinf(pokeT * PI) : 1.f;

    g_canvas.fillScreen(C_BG);

    // head
    int cx = W / 2, cy = H / 2 - 6 + (int)A.breathe;
    int rw = 128, rh = (int)(96 * squash);
    // thinking: head tilts a bit / listening: leans forward
    if (m == Mode::Thinking) cx += (int)(6 * sinf(now / 500.f));
    // antenna
    int ax = cx, ay = cy - rh - 6;
    float antSway = sinf(now / 350.f) * (m == Mode::Listening ? 6 : 2);
    g_canvas.drawLine(ax, ay + 4, ax + (int)antSway, ay - 16, C_FACE_DK);
    g_canvas.drawLine(ax + 1, ay + 4, ax + 1 + (int)antSway, ay - 16, C_FACE_DK);
    uint16_t antC = C_ANT;
    if (m == Mode::Listening) antC = ((now / 250) & 1) ? rgb(255, 80, 80) : rgb(255, 160, 160);
    else if (m == Mode::Thinking) antC = rgb(255, 220, 100 + (int)(100 * fabsf(sinf(now / 200.f))));
    else if (m == Mode::Speaking) antC = C_ACC;
    else if (m == Mode::Muted) antC = C_DIM;
    g_canvas.fillCircle(ax + (int)antSway, ay - 18, 6, antC);
    // ears (side bumps)
    g_canvas.fillRoundRect(cx - rw - 10, cy - 22, 14, 44, 6, C_FACE_DK);
    g_canvas.fillRoundRect(cx + rw - 4, cy - 22, 14, 44, 6, C_FACE_DK);
    // face plate
    g_canvas.fillRoundRect(cx - rw, cy - rh, rw * 2, rh * 2, 34, C_FACE);
    g_canvas.fillRoundRect(cx - rw + 6, cy - rh + 6, rw * 2 - 12, rh * 2 - 12, 30, C_FACE);
    // screen-ish inner panel shading
    g_canvas.fillRoundRect(cx - rw + 8, cy + rh - 26, rw * 2 - 16, 18, 9, C_FACE_DK);

    // eyes
    int ex = 52, ey = cy - 14;
    int erx = 26, ery = 30;
    if (e == Expression::Surprised) { erx = 30; ery = 36; }
    float open = 1.f - A.blink;
    if (m == Mode::Sleep) open = 0.f;
    eye(cx - ex, ey, erx, ery, open, A.gx, A.gy, e, true);
    eye(cx + ex, ey, erx, ery, open, A.gx, A.gy, e, false);
    if (open < 0.08f && e != Expression::Happy && e != Expression::Love) {
        g_canvas.fillRoundRect(cx - ex - erx, ey - 2, erx * 2, 5, 2, C_EYE);
        g_canvas.fillRoundRect(cx + ex - erx, ey - 2, erx * 2, 5, 2, C_EYE);
    }
    // blush
    if (e == Expression::Happy || e == Expression::Love || poked) {
        g_canvas.fillEllipse(cx - ex - 6, ey + 34, 14, 7, C_BLUSH);
        g_canvas.fillEllipse(cx + ex + 6, ey + 34, 14, 7, C_BLUSH);
    }
    // mouth
    mouth(cx, ey + 46, A.mouth, e, m);

    // mode ornaments
    if (m == Mode::Thinking) {
        for (int d = 0; d < 3; d++) {
            int r = 3 + (((now / 160) % 3) == d ? 2 : 0);
            g_canvas.fillCircle(cx + rw + 22 + d * 12, cy - rh + 10, r, C_ACC);
        }
    } else if (m == Mode::Sleep) {
        g_canvas.setFont(&fonts::Font2); g_canvas.setTextColor(C_DIM, C_BG); g_canvas.setTextDatum(middle_center);
        int zy = (now / 60) % 40;
        g_canvas.drawString("z", cx + rw + 14, cy - 20 - zy);
        g_canvas.drawString("Z", cx + rw + 28, cy - 40 - zy / 2);
    } else if (m == Mode::Listening) {
        // little equalizer bars under the face
        float l = g_state.micLevel;
        for (int i = 0; i < 9; i++) {
            float h = 3 + l * 18 * fabsf(sinf(now / 90.f + i * 0.9f));
            g_canvas.fillRoundRect(cx - 40 + i * 10, cy + rh + 8, 6, (int)h, 2, rgb(255, 120, 120));
        }
    } else if (m == Mode::Standby && g_state.targetVisible) {
        g_canvas.fillCircle(cx + rw + 16, cy - rh + 10, 3, rgb(120, 255, 180));   // "I see you"
    }

    statusBar(m);
    caption(now);
    botsRow(now);
    g_canvas.pushSprite(0, 0);
}

void renderTask(void*) {
    A.t0 = millis();
    for (;;) {
        if (g_enabled) {
            xSemaphoreTake(g_canvasMutex, portMAX_DELAY);
            drawFrame(millis());
            xSemaphoreGive(g_canvasMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

}  // namespace

namespace face {

void begin() {
    initPalette();
    g_canvasMutex = xSemaphoreCreateMutex();
    g_canvas.setColorDepth(16);
    g_canvas.setPsram(true);
    if (!g_canvas.createSprite(W, H)) {
        // no PSRAM: fall back to an 8-bit canvas that fits in internal RAM
        g_canvas.setColorDepth(8);
        g_canvas.createSprite(W, H);
    }
    xTaskCreatePinnedToCore(renderTask, "face", 6144, nullptr, 2, &g_task, 1);
}

void setEnabled(bool on) { g_enabled = on; }

void showText(const char* title, const char* body) {
    xSemaphoreTake(g_canvasMutex, portMAX_DELAY);
    g_canvas.fillScreen(C_BG);
    g_canvas.setTextDatum(top_left);
    g_canvas.setFont(&fonts::Font2);
    g_canvas.setTextColor(C_ACC, C_BG);
    g_canvas.drawString(title, 10, 10);
    g_canvas.setFont(&fonts::Font0);
    g_canvas.setTextColor(C_TXT, C_BG);
    g_canvas.setCursor(10, 40);
    g_canvas.print(body);
    g_canvas.pushSprite(0, 0);
    xSemaphoreGive(g_canvasMutex);
}

void pokeReaction() { g_pokeUntil = millis() + 500; }

}  // namespace face
