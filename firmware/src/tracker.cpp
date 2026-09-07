#include "tracker.h"
#include "config.h"
#include "state.h"
#include "settings.h"
#include "net_link.h"
#include <M5Unified.h>
#include <freertos/idf_additions.h>
#include <M5StackChan.h>
#include <esp_camera.h>
#include <img_converters.h>
#include <math.h>

namespace {

bool g_camOk = false;
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_frozen{false};
uint8_t* g_prev;   // TRK_W*TRK_H luma of previous frame
uint8_t* g_cur;
SemaphoreHandle_t g_camMutex;

// face assist
std::atomic<bool> g_faceFound{false};
float g_faceX = 0.5f, g_faceY = 0.5f, g_faceW = 0.f;
uint32_t g_faceAt = 0;

// tracking state
float g_tx = 0.f, g_ty = 0.f;   // smoothed normalized target offset from centre (-1..1)
uint32_t g_lastSeen = 0;
uint32_t g_pauseUntil = 0;
int g_yaw = 0, g_pitch = PITCH_HOME;

enum class Script : uint8_t { None, Nod, Shake, Dance };
std::atomic<Script> g_script{Script::None};

camera_config_t makeConfig(pixformat_t fmt) {
    camera_config_t c = {};
    c.pin_pwdn = -1; c.pin_reset = -1; c.pin_xclk = -1;       // XCLK comes from the on-board 20 MHz crystal
    c.pin_sccb_sda = 12; c.pin_sccb_scl = 11;                  // shared internal I2C (M5Unified In_I2C)
    c.pin_d7 = 47; c.pin_d6 = 48; c.pin_d5 = 16; c.pin_d4 = 15;
    c.pin_d3 = 42; c.pin_d2 = 41; c.pin_d1 = 40; c.pin_d0 = 39;
    c.pin_vsync = 46; c.pin_href = 38; c.pin_pclk = 45;
    c.xclk_freq_hz = 20000000;
    c.ledc_timer = LEDC_TIMER_0; c.ledc_channel = LEDC_CHANNEL_0;
    c.pixel_format = fmt;
    c.frame_size = (CAM_W == 640) ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
    c.jpeg_quality = 12;
    c.fb_count = 1;                       // one buffer + grab-when-empty: the DVP DMA idles between polls
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    c.sccb_i2c_port = 1;
    return c;
}

// Downsample a frame to TRK_W x TRK_H luma
void toLuma(const camera_fb_t* fb, uint8_t* out) {
    if (fb->format == PIXFORMAT_GRAYSCALE) {
        for (int y = 0; y < TRK_H; y++) {
            const uint8_t* row = fb->buf + (y * TRK_DS) * fb->width;
            for (int x = 0; x < TRK_W; x++) out[y * TRK_W + x] = row[x * TRK_DS];
        }
    } else if (fb->format == PIXFORMAT_RGB565) {
        for (int y = 0; y < TRK_H; y++) {
            const uint16_t* row = (const uint16_t*)(fb->buf + (y * TRK_DS) * fb->width * 2);
            for (int x = 0; x < TRK_W; x++) {
                uint16_t p = __builtin_bswap16(row[x * TRK_DS]);
                int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                out[y * TRK_W + x] = (uint8_t)((r * 8 * 77 + g * 4 * 150 + b * 8 * 29) >> 8);
            }
        }
    } else if (fb->format == PIXFORMAT_YUV422) {
        for (int y = 0; y < TRK_H; y++) {
            const uint8_t* row = fb->buf + (y * TRK_DS) * fb->width * 2;
            for (int x = 0; x < TRK_W; x++) out[y * TRK_W + x] = row[x * TRK_DS * 2];
        }
    }
}

// Motion centroid. Returns true if enough pixels changed; cx/cy normalized -1..1
bool motionCentroid(float& cx, float& cy) {
    long sx = 0, sy = 0; int n = 0;
    for (int y = 1; y < TRK_H - 1; y++) {
        for (int x = 1; x < TRK_W - 1; x++) {
            int i = y * TRK_W + x;
            int d = abs((int)g_cur[i] - (int)g_prev[i]);
            if (d > TRK_DIFF_THRESHOLD) { sx += x; sy += y; n++; }
        }
    }
    if (n < TRK_MIN_PIXELS || n > TRK_W * TRK_H / 2) return false;   // too little, or global change (head moving / light)
    cx = ((float)sx / n) / (TRK_W - 1) * 2.f - 1.f;
    cy = ((float)sy / n) / (TRK_H - 1) * 2.f - 1.f;
    return true;
}

void applyServo(float errX, float errY, int speed) {
    // errX/errY are normalized offsets (-1..1) of the target from image centre.
    float dYaw = -errX * (CAM_HFOV_DEG / 2) * TRK_GAIN * 10 * TRK_YAW_SIGN;   // BSP units: 10 per degree
    float dPitch = -errY * (CAM_VFOV_DEG / 2) * TRK_GAIN * 10 * TRK_PITCH_SIGN;
    g_yaw = constrain(g_yaw + (int)dYaw, YAW_MIN, YAW_MAX);
    g_pitch = constrain(g_pitch + (int)dPitch, PITCH_MIN, PITCH_MAX);
    M5StackChan.Motion.move(g_yaw, g_pitch, speed);
}

// frame2jpg() mallocs its output from internal RAM, which is scarce here; collect the JPEG into
// a PSRAM buffer via the callback variant instead.
struct JpegSink { uint8_t* buf; size_t len, cap; };
static size_t jpegSinkCb(void* arg, size_t index, const void* data, size_t len) {
    JpegSink* s = (JpegSink*)arg;
    if (index + len > s->cap) return 0;
    memcpy(s->buf + index, data, len);
    s->len = index + len;
    return len;
}

static bool frameToJpegPsram(camera_fb_t* fb, int quality, uint8_t** out, size_t* len) {
    JpegSink sink{(uint8_t*)heap_caps_malloc(256 * 1024, MALLOC_CAP_SPIRAM), 0, 256 * 1024};
    if (!sink.buf) return false;
    bool ok = frame2jpg_cb(fb, quality, jpegSinkCb, &sink) && sink.len > 0;
    if (!ok) { free(sink.buf); return false; }
    *out = sink.buf; *len = sink.len;
    return true;
}

void runScript(Script s) {
    auto& mo = M5StackChan.Motion;
    switch (s) {
        case Script::Nod:
            for (int i = 0; i < 2; i++) { mo.movePitch(constrain(g_pitch - 200, PITCH_MIN, PITCH_MAX), 900); vTaskDelay(pdMS_TO_TICKS(260)); mo.movePitch(g_pitch, 900); vTaskDelay(pdMS_TO_TICKS(260)); }
            break;
        case Script::Shake:
            for (int i = 0; i < 2; i++) { mo.moveYaw(g_yaw - 250, 900); vTaskDelay(pdMS_TO_TICKS(240)); mo.moveYaw(g_yaw + 250, 900); vTaskDelay(pdMS_TO_TICKS(240)); }
            mo.moveYaw(g_yaw, 700);
            break;
        case Script::Dance: {
            for (int i = 0; i < 6; i++) {
                int y = (i % 2) ? 400 : -400;
                int p = constrain(PITCH_HOME + ((i % 3) - 1) * 200, PITCH_MIN, PITCH_MAX);
                mo.move(y, p, 800);
                M5StackChan.showRgbColor(esp_random() & 255, esp_random() & 255, esp_random() & 255);
                vTaskDelay(pdMS_TO_TICKS(380));
            }
            mo.move(0, PITCH_HOME, 500);
            g_yaw = 0; g_pitch = PITCH_HOME;
            M5StackChan.showRgbColor(0, 0, 0);
            break;
        }
        default: break;
    }
}

void trackTask(void*) {
    uint32_t lastAssist = 0;
    for (;;) {
        Script s = g_script.exchange(Script::None);
        if (s != Script::None && !g_frozen) { runScript(s); g_pauseUntil = millis() + 800; continue; }
        if (!g_enabled || !g_camOk || g_frozen) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }   // frozen: no grabs, no uploads

        camera_fb_t* fb = nullptr;
        if (xSemaphoreTake(g_camMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            fb = esp_camera_fb_get();
            if (fb) toLuma(fb, g_cur);
            // face-detect assist: ship a small JPEG to the server now and then
            uint32_t now = millis();
            if (fb && TRK_ASSIST_MS && net::connected() && now - lastAssist >= TRK_ASSIST_MS) {
                uint8_t* jpg = nullptr; size_t jl = 0;
                if (frameToJpegPsram(fb, TRK_ASSIST_JPEG_Q, &jpg, &jl)) {
                    bool ok = net::sendBin(BIN_TRACK_JPEG, jpg, jl, true);
                    static int nAssist = 0;
                    if ((nAssist++ % 20) == 0) log_i("assist jpeg %u bytes sent=%d", (unsigned)jl, (int)ok);
                    free(jpg);
                }
                lastAssist = now;
            }
            if (fb) esp_camera_fb_return(fb);
            xSemaphoreGive(g_camMutex);
        }
        if (!fb) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        uint32_t now = millis();
        float cx, cy; bool seen = false;
        // prefer a fresh face fix from the server
        if (g_faceFound && now - g_faceAt < 1500) {
            cx = g_faceX * 2.f - 1.f; cy = g_faceY * 2.f - 1.f; seen = true;
        } else if (motionCentroid(cx, cy)) {
            seen = true;
        }
        memcpy(g_prev, g_cur, TRK_W * TRK_H);

        if (seen) {
            g_tx = g_tx + (cx - g_tx) * TRK_SMOOTH;
            g_ty = g_ty + (cy - g_ty) * TRK_SMOOTH;
            g_lastSeen = now;
            g_state.targetVisible = true;
            g_state.gazeX = g_tx; g_state.gazeY = g_ty;
            if (now > g_pauseUntil && !g_frozen && g_state.mode != Mode::Sleep) {
                float ex = fabsf(g_tx) > TRK_DEADBAND ? g_tx : 0.f;
                float ey = fabsf(g_ty) > TRK_DEADBAND ? g_ty : 0.f;
                if (ex != 0.f || ey != 0.f) {
                    applyServo(ex, ey, 450);
                    // after moving the head the next diff frame is garbage; skip it
                    vTaskDelay(pdMS_TO_TICKS(180));
                    if (xSemaphoreTake(g_camMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                        camera_fb_t* f2 = esp_camera_fb_get();
                        if (f2) { toLuma(f2, g_prev); esp_camera_fb_return(f2); }
                        xSemaphoreGive(g_camMutex);
                    }
                    g_tx *= 0.3f; g_ty *= 0.3f;
                }
            }
        } else {
            if (now - g_lastSeen > 1200) { g_state.targetVisible = false; g_state.gazeX = g_state.gazeX * 0.9f; g_state.gazeY = g_state.gazeY * 0.9f; }
            if (now - g_lastSeen > TRK_LOST_MS && (g_yaw != 0 || g_pitch != PITCH_HOME) && now > g_pauseUntil && !g_frozen) {
                g_yaw = 0; g_pitch = PITCH_HOME;
                M5StackChan.Motion.move(0, PITCH_HOME, 250);
                g_pauseUntil = now + 1500;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(TRK_POLL_MS));
    }
}

}  // namespace

namespace tracker {

bool begin() {
    g_camMutex = xSemaphoreCreateMutex();
    g_prev = (uint8_t*)heap_caps_calloc(TRK_W * TRK_H, 1, MALLOC_CAP_SPIRAM);
    g_cur = (uint8_t*)heap_caps_calloc(TRK_W * TRK_H, 1, MALLOC_CAP_SPIRAM);

    // The camera's SCCB shares the internal I2C bus with the PMIC/touch/IO-expander.
    // Hand the bus to the camera driver for init, then give it back to M5Unified.
    M5.In_I2C.release();
    // GC0308 advertises GRAYSCALE but delivers short frames (61440 != 76800); RGB565 is reliable
    // and toLuma() extracts luminance from it.
    camera_config_t cfg = makeConfig(PIXFORMAT_RGB565);
#ifdef OTTER_NO_CAMERA
    esp_err_t err = ESP_FAIL;   // experiment: measure audio without the DVP DMA running
#else
    esp_err_t err = esp_camera_init(&cfg);
#endif
    if (err == ESP_OK) {
        sensor_t* s = esp_camera_sensor_get();
        if (s) { s->set_vflip(s, 0); s->set_hmirror(s, 0); }
        g_camOk = true;
        log_i("camera ok");
    } else {
        log_e("camera init failed: 0x%x — tracking disabled", err);
    }
    M5.In_I2C.begin(I2C_NUM_1, 12, 11);

    g_pitch = PITCH_HOME;
    M5StackChan.Motion.move(0, PITCH_HOME, 300);
    xTaskCreatePinnedToCoreWithCaps(trackTask, "track", 6144, nullptr, 3, nullptr, 0, MALLOC_CAP_SPIRAM);
    return g_camOk;
}

void setEnabled(bool on) { g_enabled = on && g_camOk; if (!on) g_state.targetVisible = false; }
void setFrozen(bool on) { g_frozen = on; }
bool enabled() { return g_enabled; }
bool cameraOk() { return g_camOk; }

void onFaceResult(bool found, float x, float y, float w) {
    g_faceFound = found; g_faceX = x; g_faceY = y; g_faceW = w; g_faceAt = millis();
}

size_t captureJpeg(uint8_t** out, int quality) {
    *out = nullptr;
    if (!g_camOk) return 0;
    size_t len = 0;
    if (xSemaphoreTake(g_camMutex, pdMS_TO_TICKS(3000)) != pdTRUE) { log_w("photo: camera busy"); return 0; }
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
        if (!frameToJpegPsram(fb, quality, out, &len)) { log_w("photo: jpeg encode failed"); len = 0; }
        esp_camera_fb_return(fb);
    } else log_w("photo: no frame");
    xSemaphoreGive(g_camMutex);
    return len;
}

void lookAt(float nx, float ny, int speed) {
    g_yaw = constrain((int)(nx * YAW_MAX), YAW_MIN, YAW_MAX);
    g_pitch = constrain((int)(PITCH_HOME + ny * (PITCH_MAX - PITCH_HOME)), PITCH_MIN, PITCH_MAX);
    M5StackChan.Motion.move(g_yaw, g_pitch, speed);
    g_pauseUntil = millis() + 2500;
}

void goHome() { g_yaw = 0; g_pitch = PITCH_HOME; M5StackChan.Motion.move(0, PITCH_HOME, 400); g_pauseUntil = millis() + 1500; }
int currentYaw() { return g_yaw; }
int currentPitch() { return g_pitch; }
void nod() { g_script = Script::Nod; }
void shake() { g_script = Script::Shake; }
void dance() { g_script = Script::Dance; }

}  // namespace tracker
