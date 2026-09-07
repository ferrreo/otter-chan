// Grok-bot style avatar: a soft white blob with two tilted pill eyes, orbiting colour sparks,
// drifting dust, a speech bubble for captions and small bot avatars with status chips.
#include "face.h"
#include "config.h"
#include "state.h"
#include <M5Unified.h>
#include <freertos/idf_additions.h>
#include <math.h>

namespace {

M5Canvas g_canvas(&M5.Display);
std::atomic<bool> g_enabled{false};
SemaphoreHandle_t g_canvasMutex;
std::atomic<uint32_t> g_pokeUntil{0};
std::atomic<bool> g_uiDirty{true};      // status/caption/bots changed: push the whole frame
// Only this rectangle animates every frame; the rest is pushed when it changes.
constexpr int DYN_X = 20, DYN_Y = 28, DYN_W = 280, DYN_H = 178;

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

// ---------------------------------------------------------------- blob shape
// The blob is a closed radial curve r(t) = R * (1 + sum_k a_k * cos(k*t + p_k)), scaled per axis,
// filled by scanline. Harmonics animate smoothly between presets so it wobbles, squashes and morphs.
constexpr int HARM = 5;               // harmonics 1..5
constexpr int POLY_N = 128;
struct Shape { float a[HARM]; float p[HARM]; float sx, sy; float rot; };
Shape g_shape = {{0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, 1.f, 1.f, 0.f};
Shape g_target = g_shape;
float g_shapeVel[HARM] = {0};
uint32_t g_nextMorph = 0;
float g_wob = 0;                     // wobble phase
float g_jelly = 0, g_jellyVel = 0;   // spring for pokes / speech
float g_px = 0, g_py = 0, g_pvx = 0, g_pvy = 0;   // drift offset + velocity

// presets: (a1..a5), sx, sy
void pickPreset(int which, Shape& t) {
    for (int i = 0; i < HARM; i++) { t.a[i] = 0; t.p[i] = (esp_random() % 628) / 100.f; }
    t.sx = t.sy = 1.f; t.rot = 0;
    switch (which) {
        case 0: break;                                              // circle
        case 1: t.a[3] = 0.035f; break;                             // squircle-ish
        case 2: t.a[0] = 0.045f; t.sy = 1.06f; break;               // egg
        case 3: t.a[1] = 0.06f; t.rot = (esp_random() % 314) / 100.f; break;   // oval, random angle
        case 4: t.a[2] = 0.04f; break;                              // soft triangle
        case 5: t.sx = 1.08f; t.sy = 0.93f; break;                  // squashed
        case 6: t.sx = 0.94f; t.sy = 1.07f; break;                  // stretched tall
        case 7: t.a[1] = 0.03f; t.a[2] = 0.025f; break;             // gently lumpy
    }
}

void stepShape(float dt) {
    for (int i = 0; i < HARM; i++) {
        // spring toward target amplitude (under-damped: it overshoots = jelly)
        float acc = (g_target.a[i] - g_shape.a[i]) * 40.f - g_shapeVel[i] * 6.f;
        g_shapeVel[i] += acc * dt;
        g_shape.a[i] += g_shapeVel[i] * dt;
        g_shape.p[i] = lerp(g_shape.p[i], g_target.p[i], 2.f * dt);
    }
    g_shape.sx = lerp(g_shape.sx, g_target.sx, 3.f * dt);
    g_shape.sy = lerp(g_shape.sy, g_target.sy, 3.f * dt);
    g_shape.rot = lerp(g_shape.rot, g_target.rot, 2.f * dt);
    float jacc = -g_jelly * 180.f - g_jellyVel * 9.f;
    g_jellyVel += jacc * dt; g_jelly += g_jellyVel * dt;
    g_wob += dt;
}

// Scanline-fill the blob silhouette. cx/cy centre, R base radius.
void blobPoints(int cx, int cy, float R, float mouthBulge, float* xs, float* ys) {
    float squash = 1.f + g_jelly;
    float c = cosf(g_shape.rot), sn = sinf(g_shape.rot);
    for (int i = 0; i < POLY_N; i++) {
        float t = i * (2.f * 3.14159265f / POLY_N);
        float r = 1.f;
        for (int k = 0; k < HARM; k++) r += g_shape.a[k] * cosf((k + 1) * t + g_shape.p[k]);
        r += 0.007f * sinf(3.f * t + g_wob * 2.1f) + 0.005f * sinf(5.f * t - g_wob * 1.7f);   // idle wobble
        if (mouthBulge > 0.f && sinf(t) > 0.f) r += mouthBulge * 0.10f * sinf(t);              // speaking: lower half bulges
        float x = cosf(t) * r * g_shape.sx * (1.f - 0.5f * (squash - 1.f)), y = sinf(t) * r * g_shape.sy * squash;
        xs[i] = cx + (x * c - y * sn) * R;
        ys[i] = cy + (x * sn + y * c) * R;
    }
}

void fillPoly(const float* xs, const float* ys, uint16_t fill) {
    int ymin = H, ymax = 0;
    for (int i = 0; i < POLY_N; i++) { ymin = min(ymin, (int)floorf(ys[i])); ymax = max(ymax, (int)ceilf(ys[i])); }
    for (int y = max(0, ymin); y <= min(H - 1, ymax); y++) {
        float xl = 1e9f, xr = -1e9f;
        for (int i = 0; i < POLY_N; i++) {
            int j = (i + 1) % POLY_N;
            float y0 = ys[i], y1 = ys[j];
            if ((y >= y0 && y < y1) || (y >= y1 && y < y0)) {
                float x = xs[i] + (y - y0) * (xs[j] - xs[i]) / (y1 - y0);
                xl = fminf(xl, x); xr = fmaxf(xr, x);
            }
        }
        if (xr >= xl) g_canvas.drawFastHLine((int)(xl + 0.5f), y, (int)(xr - xl + 0.5f) + 1, fill);
    }
}

void drawBlob(int cx, int cy, float R, uint16_t fill, uint16_t edge, float mouthBulge) {
    static float xs[POLY_N], ys[POLY_N];
    blobPoints(cx, cy + 1, R + 1.5f, mouthBulge, xs, ys);   // soft shadow/edge: same silhouette, slightly larger
    fillPoly(xs, ys, edge);
    blobPoints(cx, cy, R, mouthBulge, xs, ys);
    fillPoly(xs, ys, fill);
}

// ---------------------------------------------------------------- eyes
// Parametric eye: capsule of half-length L and thickness T, tilted; `arc` bends it into a smile/frown
// crescent; `dot` rounds it into a circle. Everything is interpolated so shapes morph.
struct Eye { float L, T, tilt, arc, dot; uint16_t col; };
Eye g_eyeL = {16, 7, 12, 0, 0, 0}, g_eyeR = {16, 7, -12, 0, 0, 0};

void lerpEye(Eye& e, const Eye& t, float k) {
    e.L = lerp(e.L, t.L, k); e.T = lerp(e.T, t.T, k); e.tilt = lerp(e.tilt, t.tilt, k);
    e.arc = lerp(e.arc, t.arc, k); e.dot = lerp(e.dot, t.dot, k); e.col = t.col;
}

void capsule(int x, int y, float L, float r, float tilt, uint16_t col) {
    float dx = sinf(tilt * DEG) * L, dy = cosf(tilt * DEG) * L;
    if (L < 1.f) { g_canvas.fillCircle(x, y, (int)r, col); return; }
    g_canvas.drawWideLine(x - dx, y - dy, x + dx, y + dy, r, col);
}

void drawEye(int x, int y, const Eye& e, float open, uint16_t bg) {
    float T = fmaxf(1.5f, e.T * open);
    float L = e.L * (0.4f + 0.6f * open) * (1.f - e.dot);
    float r = lerp(T, fmaxf(T, e.L * 0.9f), e.dot);   // dot: grow round
    if (fabsf(e.arc) > 0.05f) {
        // crescent: capsule with a bg-coloured capsule cut out on the inner side
        float k = e.arc;
        capsule(x, y, e.L, T + 2, 90.f, e.col);
        capsule(x, y + (k > 0 ? -1 : 1) * (T + 2) * 0.9f, e.L + 2, T + 2, 90.f, bg);
        if (open < 0.2f) capsule(x, y, e.L, 2, 90.f, e.col);
        return;
    }
    capsule(x, y, L, r, e.tilt, e.col);
}

// Target eye shapes per expression / mode; idle "looks" add variety.
void targetEyes(Expression e, Mode m, uint32_t now, Eye& tl, Eye& tr) {
    uint16_t col = C_EYE;
    tl = {16, 7, 12, 0, 0, col}; tr = {16, 7, -12, 0, 0, col};
    switch (e) {
        case Expression::Happy:  tl.arc = 1; tr.arc = 1; break;
        case Expression::Love:   tl.arc = 1; tr.arc = 1; tl.col = tr.col = rgb(255, 90, 140); break;
        case Expression::Sad:    tl.tilt = -22; tr.tilt = 22; tl.L = tr.L = 13; break;
        case Expression::Angry:  tl.tilt = 40; tr.tilt = -40; tl.L = tr.L = 13; tl.T = tr.T = 6; break;
        case Expression::Surprised: tl.dot = tr.dot = 1; tl.L = tr.L = 12; tl.T = tr.T = 6; break;
        case Expression::Sleepy: tl.T = tr.T = 2.5f; tl.tilt = tr.tilt = 90; tl.L = tr.L = 12; break;
        case Expression::Confused: tl.tilt = 4; tr.tilt = -32; tr.L = 12; break;
        case Expression::Thinking: tl.L = 14; tr.L = 10; tr.T = 6; break;
        case Expression::Wink:   tr.T = 2.5f; tr.tilt = 90; tr.L = 10; break;
        case Expression::Error:  tl.col = tr.col = rgb(255, 80, 80); tl.tilt = 45; tr.tilt = -45; tl.L = tr.L = 9; break;
        default: {
            // neutral: cycle through small idle looks every few seconds
            int look = (now / 3500) % 6;
            if (look == 1) { tl.tilt = 4; tr.tilt = -4; }                       // straighter
            else if (look == 2) { tl.L = tr.L = 12; tl.T = tr.T = 8; }          // chubbier
            else if (look == 3) { tl.tilt = 20; tr.tilt = 8; }                  // glance
            else if (look == 4) { tl.dot = tr.dot = 0.35f; }                    // rounder
        }
    }
    if (m == Mode::Listening) { tl.T = tr.T = 8; tl.L = tr.L = 18; tl.dot = tr.dot = 0.15f; }
    if (m == Mode::Thinking && e == Expression::Thinking) { tl.tilt = 6; tr.tilt = -6; }
    if (m == Mode::Sleep) { tl.T = tr.T = 2; tl.tilt = tr.tilt = 90; }
}

struct Anim {
    float blink = 0;
    uint32_t nextBlink = 0, blinkStart = 0;
    float gx = 0, gy = 0;
    float mouth = 0;
    uint32_t lastMs = 0;
} A;

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
    if (!(tgx == tgx)) tgx = 0; if (!(tgy == tgy)) tgy = 0;   // NaN guards
    A.gx = lerp(A.gx, tgx, 0.15f); A.gy = lerp(A.gy, tgy, 0.15f);
    float mo = g_state.mouthOpen.load(); if (!(mo == mo)) mo = 0;
    A.mouth = lerp(A.mouth, mo, 0.5f);
    A.gx = fmaxf(-1.f, fminf(1.f, A.gx)); A.gy = fmaxf(-1.f, fminf(1.f, A.gy)); A.mouth = fmaxf(0.f, fminf(1.f, A.mouth));
    static uint32_t lastFull = 0;
    if (now - lastFull > 1000) { g_uiDirty = true; lastFull = now; }   // status bar / clock-ish things refresh at 1 Hz
    static uint32_t frames = 0, lastHb = 0; frames++;
    if (now - lastHb > 30000) { log_i("face: %.1f fps", frames * 1000.f / (now - lastHb)); lastHb = now; frames = 0; }

    static uint32_t tClear = 0, tBlob = 0, tSparks = 0, tUi = 0, tPush = 0, tN = 0;
    static bool firstFrame = true;
    uint32_t tt = micros();
    bool full = firstFrame || g_uiDirty.exchange(false);
    firstFrame = false;
    if (full) {
        g_canvas.fillScreen(C_BG);
        g_canvas.fillRoundRect(24, 44, W - 48, 150, 18, C_PANEL);   // soft panel behind the blob (Grok UI card)
    } else {
        // the panel covers almost all of the animated region: paint it once, then only the margins in BG
        g_canvas.fillRect(DYN_X, DYN_Y, DYN_W, DYN_H, C_PANEL);
        g_canvas.fillRect(DYN_X, DYN_Y, DYN_W, 44 - DYN_Y, C_BG);                       // above the panel
        g_canvas.fillRect(DYN_X, 194, DYN_W, DYN_Y + DYN_H - 194, C_BG);               // below the panel
        g_canvas.fillRect(DYN_X, 44, 24 - DYN_X, 150, C_BG);                           // left margin
        g_canvas.fillRect(W - 24, 44, DYN_X + DYN_W - (W - 24), 150, C_BG);            // right margin
        for (int i = 0; i < 18; i++) {   // rounded corners of the panel
            int inset = 18 - (int)sqrtf((float)(18 * 18 - (18 - i) * (18 - i)));
            g_canvas.drawFastHLine(24, 44 + i, inset, C_BG); g_canvas.drawFastHLine(W - 24 - inset, 44 + i, inset, C_BG);
            g_canvas.drawFastHLine(24, 193 - i, inset, C_BG); g_canvas.drawFastHLine(W - 24 - inset, 193 - i, inset, C_BG);
        }
    }
    tClear += micros() - tt; tt = micros();

    float dt = A.lastMs ? fminf(0.1f, (now - A.lastMs) / 1000.f) : 0.033f;
    A.lastMs = now;

    // ---- shape behaviour per mode ----
    if (now > g_nextMorph) {
        int preset = (m == Mode::Standby || m == Mode::Muted) ? (int)(esp_random() % 8) : 0;
        pickPreset(preset, g_target);
        g_nextMorph = now + 3000 + esp_random() % 5000;
    }
    if (m == Mode::Listening) { g_target.sx = 1.04f; g_target.sy = 1.04f + 0.03f * sinf(now / 300.f); g_target.rot = 0; }
    else if (m == Mode::Thinking) { g_target.a[1] = 0.09f; g_target.rot = 0.25f * sinf(now / 900.f); g_target.sx = g_target.sy = 1.f; }
    else if (m == Mode::Speaking) { g_target.sy = 1.f + 0.06f * A.mouth; g_target.sx = 1.f - 0.03f * A.mouth; }
    else if (m == Mode::Sleep) { g_target.sx = 1.10f; g_target.sy = 0.86f; g_target.rot = 0; }
    stepShape(dt);
    if (now < g_pokeUntil && g_jellyVel == 0.f && g_jelly == 0.f) g_jellyVel = -2.2f;   // poke: squash then bounce
    static float lastMouth = 0;
    if (A.mouth - lastMouth > 0.25f) g_jellyVel += 0.6f * (A.mouth - lastMouth);          // syllable kicks
    lastMouth = A.mouth;

    // ---- drift: slow wander around the panel, spring back, lean toward the gaze target ----
    float wantX = 24.f * A.gx + 10.f * sinf(now / 2300.f) + 6.f * sinf(now / 4100.f + 1.f);
    float wantY = -8.f * A.gy + 5.f * sinf(now / 1900.f + 2.f) + 2.f * sinf(now / 1100.f);
    if (m == Mode::Listening) { wantX *= 0.5f; wantY = -6.f; }
    if (m == Mode::Sleep) { wantX = 0; wantY = 14.f; }
    g_pvx += ((wantX - g_px) * 18.f - g_pvx * 5.f) * dt;
    g_pvy += ((wantY - g_py) * 18.f - g_pvy * 5.f) * dt;
    g_px += g_pvx * dt; g_py += g_pvy * dt;

    int cx = W / 2 + (int)g_px, cy = 119 + (int)g_py;
    float R = 74.f;
    tSparks += micros() - tt; tt = micros();
    drawBlob(cx, cy, R, C_BLOB, rgb(150, 152, 160), m == Mode::Speaking ? A.mouth : 0.f);

    // ---- eyes: morph toward the target shape, follow the gaze, blink ----
    Eye tl, tr; targetEyes(e, m, now, tl, tr);
    lerpEye(g_eyeL, tl, 6.f * dt); lerpEye(g_eyeR, tr, 6.f * dt);
    int ex = 30, ey = cy - 6 + (int)(A.gy * 8) - (int)(A.mouth * 4);
    int gxo = (int)(A.gx * 14);
    float sq = 1.f + g_jelly;   // eyes squash with the body
    ey = cy + (int)((ey - cy) * sq);
    drawEye(cx - ex + gxo, ey, g_eyeL, open, C_BLOB);
    drawEye(cx + ex + gxo, ey, g_eyeR, open, C_BLOB);
    if (open < 0.08f && g_eyeL.arc < 0.05f) {
        capsule(cx - ex + gxo, ey, 16, 2.5f, 90.f, C_EYE);
        capsule(cx + ex + gxo, ey, 16, 2.5f, 90.f, C_EYE);
    }
    tBlob += micros() - tt; tt = micros();

    // ---- mode ornaments ----
    if (m == Mode::Thinking) {
        for (int d = 0; d < 3; d++) {
            float ph = fmodf(now / 320.f - d * 0.33f, 1.f);
            g_canvas.fillCircle(cx + (int)R + 16 + d * 10, cy - (int)R + 8, 2 + (int)(2 * (1 - ph)), rgb(mix(kPal[2], {40, 40, 46}, ph)));
        }
    } else if (m == Mode::Listening) {
        float l = g_state.micLevel;
        for (int i = 0; i < 7; i++) {
            float h = 2 + l * 16 * fabsf(sinf(now / 90.f + i * 0.9f));
            g_canvas.fillRoundRect(cx - 24 + i * 8, cy + (int)R + 6, 5, (int)h, 2, rgb(255, 110, 140));
        }
    } else if (m == Mode::Sleep) {
        g_canvas.setFont(&fonts::DejaVu12); g_canvas.setTextColor(C_DIM, C_BG); g_canvas.setTextDatum(middle_center);
        int zy = (now / 70) % 30;
        g_canvas.drawString("z", cx + (int)R + 10, cy - 20 - zy);
        g_canvas.drawString("Z", cx + (int)R + 24, cy - 40 - zy / 2);
    } else if (m == Mode::Muted) {
        g_canvas.setFont(&fonts::Font0); g_canvas.setTextColor(rgb(255, 100, 100), C_PANEL); g_canvas.setTextDatum(middle_center);
        g_canvas.drawString("muted", cx, cy + (int)R + 12);
    }
    tSparks += micros() - tt; tt = micros();

    statusBar(m, now);
    botsRow(now);
    bubble(now, cy + (int)R);
    tUi += micros() - tt; tt = micros();
    if (full) {
        g_canvas.pushSprite(0, 0);
    } else {
        M5.Display.setClipRect(DYN_X, DYN_Y, DYN_W, DYN_H);   // pushSprite honours the clip: only this region is sent
        g_canvas.pushSprite(0, 0);
        M5.Display.clearClipRect();
    }
    tPush += micros() - tt; tN++;
    if (tN == 100) {
        log_i("face cost/frame ms: clear %.1f blob %.1f sparks %.1f ui %.1f push %.1f", tClear / 100000.f, tBlob / 100000.f, tSparks / 100000.f, tUi / 100000.f, tPush / 100000.f);
        tClear = tBlob = tSparks = tUi = tPush = tN = 0;
    }
}

void renderTask(void*) {
    for (;;) {
        if (g_enabled) {
            xSemaphoreTake(g_canvasMutex, portMAX_DELAY);
            drawFrame(millis());
            xSemaphoreGive(g_canvasMutex);
        }
        Mode mm = g_state.mode.load();
        bool talking = mm == Mode::Listening || mm == Mode::Thinking || mm == Mode::Speaking;
        vTaskDelay(pdMS_TO_TICKS(talking ? 60 : 30));   // ~9 fps while audio is live, ~13 fps idle
    }
}

}  // namespace

namespace face {

void begin() {
    initPalette();
    g_canvasMutex = xSemaphoreCreateMutex();
    g_canvas.setColorDepth(8);     // RGB332 in PSRAM: half the memory traffic of 16-bit; the palette is flat anyway
    g_canvas.setPsram(true);
    g_canvas.createSprite(W, H);
    xTaskCreatePinnedToCore(renderTask, "face", 8192, nullptr, 1, nullptr, 1);   // internal stack, shares core 1 fairly with the main loop
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
void markDirty() { g_uiDirty = true; }

}  // namespace face
