// Grok-bot style avatar: a soft white blob with two tilted pill eyes, orbiting colour sparks,
// drifting dust, a speech bubble for captions and small bot avatars with status chips.
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

constexpr int W = 320, H = 240;
constexpr float DEG = 3.14159265f / 180.f;

uint16_t C_BG, C_PANEL, C_BLOB, C_BLOB_EDGE, C_EYE, C_TXT, C_DIM, C_BUBBLE, C_BUBBLE_EDGE, C_CHIP;

// spark palette (Grok-ish gradients): teal, green, purple, pink, orange, blue
struct RGB { uint8_t r, g, b; };
const RGB kPal[] = {{60, 220, 200}, {90, 230, 110}, {150, 90, 255}, {255, 90, 150}, {255, 170, 60}, {70, 140, 255}};

inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return M5.Display.color565(r, g, b); }
inline uint16_t rgb(RGB c) { return rgb(c.r, c.g, c.b); }
inline RGB mix(RGB a, RGB b, float t) { return {(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t), (uint8_t)(a.b + (b.b - a.b) * t)}; }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

void initPalette() {
    C_BG          = rgb(9, 9, 11);
    C_PANEL       = rgb(20, 20, 23);
    C_BLOB        = rgb(252, 252, 250);
    C_BLOB_EDGE   = rgb(60, 60, 66);
    C_EYE         = rgb(18, 18, 22);
    C_TXT         = rgb(236, 236, 240);
    C_DIM         = rgb(110, 112, 125);
    C_BUBBLE      = rgb(34, 34, 40);
    C_BUBBLE_EDGE = rgb(72, 72, 82);
    C_CHIP        = rgb(30, 30, 36);
}

// ---------------------------------------------------------------- particles
struct Spark {
    float angle, radius, speed, len, life, hue;   // hue = palette index (float for blending)
    bool alive;
};
Spark g_sparks[7];
struct Dust { float x, y, vx, vy, r; };
Dust g_dust[4];

void spawnSpark(Spark& s, float intensity) {
    s.angle = (esp_random() % 360) * DEG;
    s.radius = 70 + (esp_random() % 40);
    s.speed = (0.4f + (esp_random() % 100) / 100.f) * (0.6f + intensity) * ((esp_random() & 1) ? 1 : -1);
    s.len = 12 + (esp_random() % 26);
    s.life = 1.f;
    s.hue = esp_random() % 6;
    s.alive = true;
}

void drawSpark(const Spark& s, int cx, int cy, float scale) {
    // arc drawn as beads with a colour gradient and a fading tail
    RGB a = kPal[(int)s.hue], b = kPal[((int)s.hue + 1) % 6];
    int beads = (int)(s.len / 3);
    float alpha = clamp01(s.life * 1.4f);
    for (int i = 0; i < beads; i++) {
        float t = (float)i / beads;
        float ang = s.angle - t * (s.len / s.radius) * (s.speed > 0 ? 1 : -1);
        float rr = s.radius * scale;
        int x = cx + (int)(cosf(ang) * rr), y = cy + (int)(sinf(ang) * rr * 0.85f);
        RGB c = mix(a, b, t);
        float k = alpha * (1.f - t * 0.8f);
        c = mix({C_BG >> 8 & 0xF8, (C_BG >> 3) & 0xFC, (C_BG << 3) & 0xF8}, c, k);
        int r = (int)(3.f * (1.f - t * 0.6f));
        if (r < 1) r = 1;
        g_canvas.fillCircle(x, y, r, rgb(c));
    }
}

// ---------------------------------------------------------------- eyes
// A pill eye: centre (x,y), half-length L, radius r, tilt in degrees (positive = top leans right)
void pill(int x, int y, float L, int r, float tilt, uint16_t col) {
    float dx = sinf(tilt * DEG) * L, dy = cosf(tilt * DEG) * L;
    if (L < 1.f) { g_canvas.fillCircle(x, y, r, col); return; }
    g_canvas.drawWideLine(x - dx, y - dy, x + dx, y + dy, r, col);
}

// blob squash/eye positions
struct Anim {
    float blink = 0;
    uint32_t nextBlink = 0, blinkStart = 0;
    float gx = 0, gy = 0;
    float mouth = 0;
    float bob = 0;
    float excite = 0;   // 0..1 drives spark intensity
} A;

void drawEyes(int cx, int cy, Expression e, Mode m, float open, uint32_t now) {
    int ex = 30;                          // eye spacing from centre
    int ey = cy - 4;
    int r = 7;                             // pill radius
    float L = 16 * open;                   // half length
    float tiltL = 12, tiltR = -12;         // default: slight inward lean (top toward centre)
    uint16_t col = C_EYE;
    int gx = (int)(A.gx * 14), gy = (int)(A.gy * 8);
    float bounce = A.mouth * 4;

    switch (e) {
        case Expression::Happy:  L = 10 * open; tiltL = 35; tiltR = -35; ey -= 2; break;
        case Expression::Love:   col = rgb(255, 90, 140); L = 12 * open; tiltL = 30; tiltR = -30; break;
        case Expression::Sad:    tiltL = -22; tiltR = 22; ey += 4; break;
        case Expression::Angry:  tiltL = 40; tiltR = -40; L = 13 * open; break;
        case Expression::Surprised: L = 0; r = 11; break;
        case Expression::Sleepy: L = 12; r = 3; tiltL = 90; tiltR = 90; break;   // flat dashes
        case Expression::Confused: tiltL = 5; tiltR = -30; break;
        case Expression::Thinking: tiltL = 8; tiltR = -8; gy -= 6; gx += 6; break;
        case Expression::Wink:   break;
        case Expression::Error:  col = rgb(255, 80, 80); break;
        default: break;
    }
    if (m == Mode::Listening) { r = 8; L = 18 * open; }
    if (m == Mode::Speaking) { L = lerp(L, 10, A.mouth * 0.5f); }

    int lx = cx - ex + gx, rx = cx + ex + gx, y = ey + gy - (int)bounce;
    if (e == Expression::Error) {
        for (int s = -1; s <= 1; s += 2) {
            int x = s < 0 ? lx : rx;
            pill(x, y, 10, 3, 45, col); pill(x, y, 10, 3, -45, col);
        }
        return;
    }
    if (e == Expression::Wink) {
        pill(lx, y, L, r, tiltL, col);
        pill(rx, y, 10, 3, 90, col);
        return;
    }
    pill(lx, y, L, r, tiltL, col);
    pill(rx, y, L, r, tiltR, col);
    if (e == Expression::Surprised) { g_canvas.fillCircle(lx - 3, y - 3, 3, C_BLOB); g_canvas.fillCircle(rx - 3, y - 3, 3, C_BLOB); }
}

// ---------------------------------------------------------------- top status
void statusBar(Mode m, uint32_t now) {
    int y = 6;
    // wifi bars
    int bars = g_state.wifiOk ? ((g_state.rssi + 100) / 15 + 1) : 0;
    for (int i = 0; i < 4; i++) g_canvas.fillRoundRect(8 + i * 5, y + 9 - i * 3, 3, 3 + i * 3, 1, i < bars ? rgb(120, 220, 200) : rgb(40, 40, 46));
    // server dot
    g_canvas.fillCircle(36, y + 6, 3, g_state.serverOk ? rgb(90, 230, 120) : rgb(255, 110, 70));
    // mic pill
    uint16_t micC = (m == Mode::Listening) ? rgb(255, 90, 120) : (m == Mode::Muted ? rgb(90, 90, 100) : rgb(70, 72, 84));
    g_canvas.fillRoundRect(46, y, 6, 10, 3, micC);
    if (m == Mode::Muted) g_canvas.drawLine(44, y + 12, 54, y - 1, rgb(255, 90, 90));
    // battery
    int pct = g_state.batteryPct;
    int bx = W - 32;
    g_canvas.drawRoundRect(bx, y, 22, 11, 3, rgb(70, 72, 84));
    g_canvas.fillRect(bx + 22, y + 3, 2, 5, rgb(70, 72, 84));
    if (pct >= 0) g_canvas.fillRoundRect(bx + 2, y + 2, max(1, 18 * pct / 100), 7, 2, g_state.charging ? rgb(120, 200, 255) : (pct > 30 ? rgb(90, 230, 120) : rgb(255, 120, 70)));
}

// ---------------------------------------------------------------- bots row (top)
void botAvatar(int x, int y, int r, RGB c, uint32_t now, int idx) {
    g_canvas.fillCircle(x, y, r + 1, rgb(mix(c, {0, 0, 0}, 0.55f)));
    g_canvas.fillCircle(x, y, r, rgb(c));
    // two tiny pill eyes, occasional blink
    bool blink = ((now / 100 + idx * 7) % 40) == 0;
    uint16_t eye = rgb(20, 20, 26);
    if (blink) { g_canvas.fillRect(x - 5, y - 1, 3, 2, eye); g_canvas.fillRect(x + 2, y - 1, 3, 2, eye); }
    else { g_canvas.fillRoundRect(x - 5, y - 4, 3, 7, 1, eye); g_canvas.fillRoundRect(x + 2, y - 4, 3, 7, 1, eye); }
}

const char* activityWord(const char* a) {
    if (!strcmp(a, "coding")) return "coding";
    if (!strcmp(a, "browsing")) return "browsing";
    if (!strcmp(a, "thinking")) return "thinking";
    if (!strcmp(a, "talking")) return "talking";
    if (!strcmp(a, "writing")) return "writing";
    if (!strcmp(a, "error")) return "error";
    if (!strcmp(a, "offline")) return "offline";
    if (!strcmp(a, "done")) return "done";
    if (!strcmp(a, "waiting")) return "waiting";
    return "idle";
}

void botsRow(uint32_t now) {
    g_state.lock();
    int n = g_state.botCount;
    BotStatus bots[MAX_BOTS];
    for (int i = 0; i < n; i++) bots[i] = g_state.bots[i];
    g_state.unlock();
    if (n == 0) return;
    g_canvas.setFont(&fonts::Font0);
    g_canvas.setTextSize(1);
    g_canvas.setTextDatum(middle_left);
    int x = 8, y = 30;
    for (int i = 0; i < n && i < 4; i++) {
        RGB c = kPal[i % 6];
        bool active = strcmp(bots[i].activity, "idle") && strcmp(bots[i].activity, "offline") && strcmp(bots[i].activity, "done");
        bool live = now - bots[i].updatedMs < 120000;
        if (!live) c = mix(c, {60, 60, 66}, 0.7f);
        char nm[10]; strncpy(nm, bots[i].name, 9); nm[9] = 0;
        char st[12]; strncpy(st, activityWord(bots[i].activity), 11); st[11] = 0;
        int wName = g_canvas.textWidth(nm), wSt = g_canvas.textWidth(st);
        int chipW = 10 + wName + 6 + (active ? 14 : 0) + wSt + 8;
        if (x + 24 + chipW > W - 40) break;
        botAvatar(x + 11, y, 10, c, now, i);
        // chip
        g_canvas.fillRoundRect(x + 24, y - 9, chipW, 18, 9, C_CHIP);
        g_canvas.drawRoundRect(x + 24, y - 9, chipW, 18, 9, rgb(mix(c, {0, 0, 0}, 0.4f)));
        int tx = x + 24 + 8;
        g_canvas.setTextColor(C_TXT, C_CHIP);
        g_canvas.drawString(nm, tx, y);
        tx += wName + 6;
        if (active) {   // animated ••• in the bot's colour
            for (int d = 0; d < 3; d++) {
                float ph = fmodf(now / 300.f - d * 0.33f, 1.f);
                uint16_t dc = rgb(mix(c, {40, 40, 46}, ph));
                g_canvas.fillCircle(tx + d * 4, y, 1, dc);
            }
            tx += 14;
        }
        g_canvas.setTextColor(rgb(active ? c : RGB{140, 142, 155}), C_CHIP);
        g_canvas.drawString(st, tx, y);
        x += 24 + chipW + 8;
    }
}

// ---------------------------------------------------------------- speech bubble
void wrapLines(const String& s, int maxW, String out[], int maxLines, int& nLines) {
    nLines = 0;
    String line, word;
    for (size_t i = 0; i <= s.length(); i++) {
        char c = i < s.length() ? s[i] : ' ';
        if (c == ' ' || c == '\n') {
            String test = line.length() ? line + " " + word : word;
            if (g_canvas.textWidth(test) <= maxW) line = test;
            else {
                if (nLines < maxLines) out[nLines++] = line;
                line = word;
            }
            word = "";
            if (nLines >= maxLines) break;
        } else word += c;
    }
    if (line.length() && nLines < maxLines) out[nLines++] = line;
    if (nLines == maxLines && s.length() > 0) {
        String& last = out[nLines - 1];
        while (g_canvas.textWidth(last + "...") > maxW && last.length()) last.remove(last.length() - 1);
        if (word.length()) last += "...";
    }
}

void bubble(uint32_t now, int blobBottom) {
    g_state.lock();
    String s = g_state.caption;
    bool show = s.length() && (g_state.captionUntilMs == 0 || now < g_state.captionUntilMs);
    g_state.unlock();
    if (!show) return;
    g_canvas.setFont(&fonts::DejaVu12);
    g_canvas.setTextSize(1);
    String lines[3]; int n;
    wrapLines(s, W - 44, lines, 3, n);
    int lh = 15, pad = 8;
    int bh = n * lh + pad * 2;
    int bw = 0;
    for (int i = 0; i < n; i++) bw = max(bw, (int)g_canvas.textWidth(lines[i]));
    bw += pad * 2 + 4;
    int bx = (W - bw) / 2, by = H - bh - 6;
    // shadow + body + tail
    g_canvas.fillRoundRect(bx + 2, by + 3, bw, bh, 10, rgb(4, 4, 6));
    g_canvas.fillRoundRect(bx, by, bw, bh, 10, C_BUBBLE);
    g_canvas.drawRoundRect(bx, by, bw, bh, 10, C_BUBBLE_EDGE);
    int tx = W / 2;
    g_canvas.fillTriangle(tx - 7, by + 1, tx + 7, by + 1, tx, by - 8, C_BUBBLE);
    g_canvas.drawLine(tx - 7, by, tx, by - 8, C_BUBBLE_EDGE);
    g_canvas.drawLine(tx + 7, by, tx, by - 8, C_BUBBLE_EDGE);
    g_canvas.setTextDatum(top_left);
    g_canvas.setTextColor(C_TXT, C_BUBBLE);
    for (int i = 0; i < n; i++) g_canvas.drawString(lines[i], bx + pad + 2, by + pad + i * lh);
    (void)blobBottom;
}

// ---------------------------------------------------------------- frame
void drawFrame(uint32_t now) {
    Mode m = g_state.mode.load();
    Expression e = g_state.expression.load();
    uint32_t until = g_state.expressionUntilMs.load();
    if (until && now > until) { g_state.expression = Expression::Neutral; g_state.expressionUntilMs = 0; e = Expression::Neutral; }
    if (m == Mode::Thinking && e == Expression::Neutral) e = Expression::Thinking;
    if (m == Mode::Sleep) e = Expression::Sleepy;

    // blink
    if (now > A.nextBlink) { A.blinkStart = now; A.nextBlink = now + 2200 + (esp_random() % 4500); }
    float bt = (now - A.blinkStart) / 150.f;
    A.blink = bt < 1.f ? sinf(bt * 3.14159f) : 0.f;
    float open = 1.f - A.blink;
    if (m == Mode::Sleep) open = 0.f;

    float tgx = g_state.gazeX.load(), tgy = g_state.gazeY.load();
    if (m == Mode::Listening) { tgx *= 0.4f; tgy = -0.3f; }
    A.gx = lerp(A.gx, tgx, 0.15f); A.gy = lerp(A.gy, tgy, 0.15f);
    A.mouth = lerp(A.mouth, g_state.mouthOpen.load(), 0.5f);
    float targetEx = m == Mode::Thinking ? 1.f : m == Mode::Speaking ? 0.7f : m == Mode::Listening ? 0.5f : (g_state.targetVisible ? 0.25f : 0.1f);
    if (now < g_pokeUntil) targetEx = 1.f;
    A.excite = lerp(A.excite, targetEx, 0.05f);

    g_canvas.fillScreen(C_BG);
    // soft panel behind the blob (like the Grok UI card)
    g_canvas.fillRoundRect(24, 44, W - 48, 150, 18, C_PANEL);

    // blob geometry: breathe + speech bounce + poke squash
    float breathe = sinf(now / 1100.f) * 2.f;
    float squash = 1.f;
    if (now < g_pokeUntil) { float t = 1.f - (g_pokeUntil - now) / 500.f; squash = 1.f - 0.15f * sinf(t * 3.14159f); }
    int cx = W / 2 + (int)(A.gx * 6), cy = 120 + (int)breathe - (int)(A.mouth * 6);
    int rx = 78, ry = (int)(78 * squash);
    if (m == Mode::Listening) { float p = 0.5f + 0.5f * sinf(now / 180.f); g_canvas.fillEllipse(cx, cy, rx + 6 + (int)(p * 4), ry + 6 + (int)(p * 4), rgb(70 + (int)(p * 40), 20, 40)); }
    if (m == Mode::Sleep) { rx = 74; ry = 70; }

    // sparks behind the blob first (those with sin(angle) < 0), then blob, then the rest
    for (auto& s : g_sparks) {
        if (!s.alive) { if ((esp_random() % 1000) < (uint32_t)(A.excite * 60 + 2)) spawnSpark(s, A.excite); continue; }
        s.angle += s.speed * 0.05f * (0.5f + A.excite);
        s.life -= 0.006f + (1.f - A.excite) * 0.01f;
        if (s.life <= 0) s.alive = false;
    }
    for (auto& s : g_sparks) if (s.alive && sinf(s.angle) < 0) drawSpark(s, cx, cy, 1.f);
    // dust
    for (int i = 0; i < 4; i++) {
        Dust& d = g_dust[i];
        d.x += d.vx; d.y += d.vy;
        if (d.x < 20 || d.x > W - 20) d.vx = -d.vx;
        if (d.y < 50 || d.y > 190) d.vy = -d.vy;
        g_canvas.fillCircle((int)d.x, (int)d.y, (int)d.r, rgb(200, 200, 210));
    }
    // blob with subtle edge
    g_canvas.fillEllipse(cx, cy + 3, rx + 2, ry + 2, rgb(30, 30, 34));
    g_canvas.fillEllipse(cx, cy, rx, ry, C_BLOB);
    drawEyes(cx, cy, e, m, open, now);
    for (auto& s : g_sparks) if (s.alive && sinf(s.angle) >= 0) drawSpark(s, cx, cy, 1.f);

    // mode extras
    if (m == Mode::Thinking) {
        for (int d = 0; d < 3; d++) {
            float ph = fmodf(now / 320.f - d * 0.33f, 1.f);
            g_canvas.fillCircle(cx + rx + 16 + d * 10, cy - ry + 8, 2 + (int)(2 * (1 - ph)), rgb(mix(kPal[2], {40, 40, 46}, ph)));
        }
    } else if (m == Mode::Listening) {
        float l = g_state.micLevel;
        for (int i = 0; i < 7; i++) {
            float h = 2 + l * 16 * fabsf(sinf(now / 90.f + i * 0.9f));
            g_canvas.fillRoundRect(cx - 24 + i * 8, cy + ry + 8, 5, (int)h, 2, rgb(255, 110, 140));
        }
    } else if (m == Mode::Sleep) {
        g_canvas.setFont(&fonts::DejaVu12); g_canvas.setTextColor(C_DIM, C_BG); g_canvas.setTextDatum(middle_center);
        int zy = (now / 70) % 30;
        g_canvas.drawString("z", cx + rx + 10, cy - 20 - zy);
        g_canvas.drawString("Z", cx + rx + 24, cy - 40 - zy / 2);
    } else if (m == Mode::Muted) {
        g_canvas.setFont(&fonts::Font0); g_canvas.setTextColor(rgb(255, 100, 100), C_PANEL); g_canvas.setTextDatum(middle_center);
        g_canvas.drawString("muted", cx, cy + ry + 14);
    }

    statusBar(m, now);
    botsRow(now);
    bubble(now, cy + ry);
    g_canvas.pushSprite(0, 0);
}

void renderTask(void*) {
    for (auto& d : g_dust) { d.x = 40 + esp_random() % 240; d.y = 60 + esp_random() % 120; d.vx = ((int)(esp_random() % 100) - 50) / 400.f; d.vy = ((int)(esp_random() % 100) - 50) / 400.f; d.r = 1 + esp_random() % 2; }
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
        g_canvas.setColorDepth(8);
        g_canvas.createSprite(W, H);
    }
    xTaskCreatePinnedToCore(renderTask, "face", 8192, nullptr, 2, nullptr, 1);
}

void setEnabled(bool on) { g_enabled = on; }

void showText(const char* title, const char* body) {
    xSemaphoreTake(g_canvasMutex, portMAX_DELAY);
    g_canvas.fillScreen(C_BG);
    g_canvas.setTextDatum(top_left);
    g_canvas.setFont(&fonts::DejaVu18);
    g_canvas.setTextColor(rgb(120, 220, 200), C_BG);
    g_canvas.drawString(title, 12, 12);
    g_canvas.setFont(&fonts::DejaVu12);
    g_canvas.setTextColor(C_TXT, C_BG);
    g_canvas.setCursor(12, 44);
    g_canvas.print(body);
    g_canvas.pushSprite(0, 0);
    xSemaphoreGive(g_canvasMutex);
}

void pokeReaction() { g_pokeUntil = millis() + 500; }

}  // namespace face
