// Otter-chan — custom firmware for the M5Stack StackChan (CoreS3)
//
// Controls
//   side (power) button  click  : open mic (conversation stays open until dismissed) / close mic
//                        double : hard mute toggle (wake word off)
//                        hold   : sleep / wake
//   head touch panel     tap    : happy nod   swipe fwd/back : volume   hold : love
//   screen               tap    : wink        hold : show IP / battery
//   wake word            say the wake phrase (server-side check) to open the mic hands-free;
//                        it stays open across turns until you dismiss him ("that will be all"),
//                        click the button, or 3 minutes pass in silence
//   bottom RST button    is wired to the chip reset line — it always reboots (hardware).
//
#include <Arduino.h>
#include <M5Unified.h>
#include <M5StackChan.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "config.h"
#include "settings.h"
#include "state.h"
#include "audio_io.h"
#include "net_link.h"
#include "face.h"
#include "tracker.h"
#include "behaviors.h"
#include "provisioning.h"
#include "wakeword.h"
#include "adpcm.h"

// ---------------------------------------------------------------- listening state
static SemaphoreHandle_t g_utterMutex;
static uint32_t g_listenStart = 0, g_lastSpeech = 0, g_listenWindowMs = LISTEN_NO_SPEECH_MS;
static bool g_heardSpeech = false;
static int16_t* g_wakeBuf;            // ring buffer for wake-word candidate
static size_t g_wakeLen = 0;          // samples currently captured into wake clip
static bool g_wakeCapturing = false;
static uint32_t g_wakeSpeechMs = 0, g_lastWakeSent = 0;
static int16_t* g_preBuf;             // pre-roll ring (WAKE_PRE_SAMPLES)
static size_t g_prePos = 0;
static uint32_t g_lastTelemetry = 0;
static bool g_followupPending = false;
static bool g_conversation = false;     // wake word / button opened a session: mic stays open until dismissed
static bool g_endRequested = false;     // server asked to close the session after this reply
static uint32_t g_sayStartedAt = 0;
static bool g_sayEnded = false;
static bool g_useAdpcm = false;      // negotiated with the server in hello_ack
static size_t g_sayTotalBytes = 0;   // from say_end; 0 = unknown (older server)
static uint32_t g_loopMaxMs = 0, g_loopLast = 0;
// captions arrive ahead of their audio; reveal each one when playback reaches its byte offset
struct PendingCaption { String text; size_t atBytes; };
static PendingCaption g_capQueue[8];
static int g_capHead = 0, g_capTail = 0;
static SemaphoreHandle_t g_capMutex;
static void queueCaption(const String& t) {
    xSemaphoreTake(g_capMutex, portMAX_DELAY);
    if ((g_capTail + 1) % 8 != g_capHead) { g_capQueue[g_capTail] = {t, audio::bytesReceived()}; g_capTail = (g_capTail + 1) % 8; }
    xSemaphoreGive(g_capMutex);
}
static void clearCaptions() { xSemaphoreTake(g_capMutex, portMAX_DELAY); g_capHead = g_capTail = 0; xSemaphoreGive(g_capMutex); }
static void revealCaptions() {
    xSemaphoreTake(g_capMutex, portMAX_DELAY);
    size_t played = audio::bytesPlayed();
    while (g_capHead != g_capTail && g_capQueue[g_capHead].atBytes <= played + 2048) {
        g_state.setCaption(g_capQueue[g_capHead].text, 0);
        g_capHead = (g_capHead + 1) % 8;
    }
    xSemaphoreGive(g_capMutex);
}

static void setMode(Mode m);

// ---------------------------------------------------------------- mic frames (audio task)
static void onMicFrame(const int16_t* s, size_t n, const VadResult& v) {
    Mode m = g_state.mode.load();
    const uint32_t frameMs = n * 1000 / AUDIO_RATE;
    g_state.voiceDetected = v.speech;

    if (m == Mode::Listening) {
        uint32_t now = millis();
        if (v.speech) { g_lastSpeech = now; g_heardSpeech = true; }
        if (g_useAdpcm) {
            static uint8_t enc[4 + MIC_FRAME_SAMPLES / 2 + 1];
            size_t len = adpcm::encode(s, n, enc);
            net::sendBin(BIN_MIC_ADPCM, enc, len, true);
        } else {
            net::sendBin(BIN_AUDIO, (const uint8_t*)s, n * sizeof(int16_t), true);
        }
        return;
    }

    if (m == Mode::Standby && g_settings.wakeWord && wakeword::available()) {
        wakeword::feed(s, n);   // on-device microWakeWord; nothing to ship
        return;
    }
    if (m == Mode::Standby && g_settings.wakeWord) {
        // server-side fallback: keep a small pre-roll so the start of the wake phrase isn't clipped
        for (size_t i = 0; i < n; i++) { g_preBuf[g_prePos] = s[i]; g_prePos = (g_prePos + 1) % WAKE_PRE_SAMPLES; }
        uint32_t now = millis();
        if (!g_wakeCapturing) {
            if (v.speech && now - g_lastWakeSent > WAKE_COOLDOWN_MS) {
                g_wakeCapturing = true; g_wakeLen = 0; g_wakeSpeechMs = 0;
                // copy pre-roll in order
                for (size_t i = 0; i < WAKE_PRE_SAMPLES; i++) g_wakeBuf[g_wakeLen++] = g_preBuf[(g_prePos + i) % WAKE_PRE_SAMPLES];
            } else return;
        }
        size_t room = WAKE_CLIP_SAMPLES - g_wakeLen;
        size_t take = min(n, room);
        memcpy(g_wakeBuf + g_wakeLen, s, take * sizeof(int16_t));
        g_wakeLen += take;
        if (v.speech) g_wakeSpeechMs += frameMs;
        bool full = g_wakeLen >= WAKE_CLIP_SAMPLES;
        bool ended = !v.speech && g_wakeSpeechMs > 0 && (now - g_lastSpeech) > 350;
        if (v.speech) g_lastSpeech = now;
        if (full || ended) {
            g_wakeCapturing = false;
            if (g_wakeSpeechMs >= WAKE_MIN_SPEECH_MS && net::connected()) {
                net::sendBin(BIN_WAKE_CLIP, (const uint8_t*)g_wakeBuf, g_wakeLen * sizeof(int16_t), true);
                g_lastWakeSent = now;
            }
        }
    }
}

// ---------------------------------------------------------------- mode transitions
static void setMode(Mode m) {
    Mode prev = g_state.mode.exchange(m);
    if (prev == m) return;
    log_i("mode %s -> %s", modeName(prev), modeName(m));
    // Servo moves inject noise into the audio path: hold still while the mic or speaker is live.
    tracker::setFrozen(m == Mode::Listening || m == Mode::Thinking);   // mic live: keep the servos quiet
    tracker::setTalking(m == Mode::Speaking);                          // gentle nods while he talks
    switch (m) {
        case Mode::Standby:
            audio::setDirection(g_settings.wakeWord ? AudioDir::Mic : AudioDir::Off);
            leds::set(leds::Pattern::Off);
            g_state.lock(); g_state.statusLine = g_settings.wakeWord ? "" : "standby"; g_state.unlock();
            tracker::setEnabled(g_settings.tracking);
            break;
        case Mode::Listening:
            audio::setDirection(AudioDir::Mic);
            g_listenStart = millis(); g_lastSpeech = g_listenStart; g_heardSpeech = false;
            g_listenWindowMs = g_conversation ? CONVERSATION_IDLE_MS : (g_followupPending ? LISTEN_FOLLOWUP_MS : LISTEN_NO_SPEECH_MS);
            g_followupPending = false;
            leds::set(leds::Pattern::Listening);
            g_state.setExpression(Expression::Neutral);
            g_state.lock(); g_state.statusLine = g_conversation ? "listening (say 'that will be all' to dismiss)" : "listening"; g_state.unlock();
            net::sendJson("listen_start");
            break;
        case Mode::Thinking:
            audio::setDirection(AudioDir::Off);
            leds::set(leds::Pattern::Thinking);
            g_state.lock(); g_state.statusLine = "thinking"; g_state.unlock();
            break;
        case Mode::Speaking:
            audio::setDirection(AudioDir::Speaker);
            leds::set(leds::Pattern::Speaking);
            g_state.lock(); g_state.statusLine = "speaking"; g_state.unlock();
            g_sayStartedAt = millis(); g_sayEnded = false;
            break;
        case Mode::Muted:
            audio::setDirection(AudioDir::Off);
            leds::set(leds::Pattern::Solid, 40, 0, 0);
            g_state.lock(); g_state.statusLine = "muted"; g_state.unlock();
            break;
        case Mode::Sleep:
            break;
        default: break;
    }
    JsonDocument d; d["type"] = "state"; d["state"] = modeName(m); net::sendJson(d);
}

static void goToSleep() {
    log_i("sleep");
    audio::playChime(3);
    delay(250);
    audio::clearPlayback();
    setMode(Mode::Sleep);
    audio::setDirection(AudioDir::Off);
    tracker::setEnabled(false);
    tracker::goHome();
    delay(900);
    M5StackChan.Motion.setTorqueEnabled(false);
    M5StackChan.setServoPowerEnabled(false);
    leds::set(leds::Pattern::Off); leds::tick();
    g_state.setCaption("", 1);
    delay(100);
    face::setEnabled(false);
    M5.Display.setBrightness(0);
    M5.Display.sleep();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    setCpuFrequencyMhz(80);
}

static void wakeFromSleep() {
    log_i("wake");
    setCpuFrequencyMhz(240);
    M5.Display.wakeup();
    M5.Display.setBrightness(g_settings.brightness);
    face::setEnabled(true);
    M5StackChan.setServoPowerEnabled(true);
    delay(200);
    M5StackChan.Motion.setTorqueEnabled(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_settings.wifiSsid.c_str(), g_settings.wifiPass.c_str());
    g_state.setExpression(Expression::Happy, 2000);
    audio::playChime(0);
    setMode(Mode::Standby);
}

// ---------------------------------------------------------------- server messages (net task)
static void onServerJson(JsonDocument& d) {
    const char* type = d["type"] | "";
    if (!strcmp(type, "hello_ack")) {
        g_useAdpcm = !strcmp(d["codec"] | "", "adpcm");
        log_i("server codec: %s", g_useAdpcm ? "adpcm (8 KB/s)" : "pcm (32 KB/s)");
        if (d["volume"].is<int>()) { g_settings.volume = d["volume"]; audio::setVolume(g_settings.volume); }
        if (d["name"].is<const char*>()) g_state.setCaption(String("hi, I'm ") + d["name"].as<const char*>(), 3000);
        g_state.setExpression(Expression::Happy, 1500);
    } else if (!strcmp(type, "wake")) {
        if (g_state.mode == Mode::Standby) { g_conversation = true; g_endRequested = false; audio::playChime(0); g_state.setExpression(Expression::Surprised, 800); setMode(Mode::Listening); }
    } else if (!strcmp(type, "transcript")) {
        g_state.setCaption(String("\"") + (d["text"] | "") + "\"", 8000);
        if (d["final"] | false) { if (g_state.mode == Mode::Listening) setMode(Mode::Thinking); }
    } else if (!strcmp(type, "thinking")) {
        if (g_state.mode == Mode::Listening) setMode(Mode::Thinking);
    } else if (!strcmp(type, "say_start")) {
        audio::clearPlayback();
        clearCaptions();
        Expression e; if (expressionFromName(d["expression"] | "", e)) g_state.setExpression(e);
        if (d["text"].is<const char*>()) g_state.setCaption(d["text"].as<const char*>(), 0);
        g_followupPending = d["followup"] | false;
        setMode(Mode::Speaking);
    } else if (!strcmp(type, "say_end")) {
        g_sayEnded = true;
        g_sayTotalBytes = d["bytes"] | 0;
        g_followupPending = d["followup"] | g_followupPending;
        if (d["end"] | false) g_endRequested = true;
    } else if (!strcmp(type, "expression")) {
        Expression e; if (expressionFromName(d["name"] | "", e)) g_state.setExpression(e, d["ms"] | 0);
    } else if (!strcmp(type, "caption")) {
        if (g_state.mode == Mode::Speaking && (d["ms"] | 0) == 0) queueCaption(d["text"] | "");
        else g_state.setCaption(d["text"] | "", d["ms"] | 6000);
    } else if (!strcmp(type, "nothing_heard")) {
        // empty transcript: keep the session open quietly, or drop out if this was a one-shot
        if (g_conversation) { g_state.setExpression(Expression::Confused, 900); setMode(Mode::Listening); }
        else { g_state.setExpression(Expression::Confused, 1500); setMode(Mode::Standby); }
    } else if (!strcmp(type, "bots")) {
        g_state.lock();
        int n = 0;
        for (JsonObject b : d["bots"].as<JsonArray>()) {
            if (n >= MAX_BOTS) break;
            BotStatus& s = g_state.bots[n++];
            strlcpy(s.name, b["name"] | "bot", sizeof(s.name));
            strlcpy(s.activity, b["activity"] | "idle", sizeof(s.activity));
            strlcpy(s.detail, b["detail"] | "", sizeof(s.detail));
            strlcpy(s.emoji, b["emoji"] | "", sizeof(s.emoji));
            s.updatedMs = millis() - (uint32_t)((b["age_s"] | 0) * 1000);
        }
        g_state.botCount = n;
        g_state.unlock();
        face::markDirty();
    } else if (!strcmp(type, "notify")) {
        audio::playChime(4);
        leds::set(leds::Pattern::Notify, 0, 0, 0, 4000);
        g_state.setExpression(Expression::Surprised, 1500);
        g_state.setCaption(String(d["from"] | "") + ": " + (d["text"] | ""), 10000);
    } else if (!strcmp(type, "look")) {
        tracker::lookAt(d["x"] | 0.f, d["y"] | 0.f, d["speed"] | 500);
    } else if (!strcmp(type, "gesture")) {
        tracker::gesture(d["name"] | "");
    } else if (!strcmp(type, "led")) {
        const char* mode = d["mode"] | "solid";
        leds::Pattern p = leds::Pattern::Solid;
        if (!strcmp(mode, "off")) p = leds::Pattern::Off; else if (!strcmp(mode, "pulse")) p = leds::Pattern::Pulse;
        else if (!strcmp(mode, "rainbow")) p = leds::Pattern::Rainbow;
        leds::set(p, d["r"] | 0, d["g"] | 0, d["b"] | 0, d["ms"] | 0);
    } else if (!strcmp(type, "face")) {
        tracker::onFaceResult(d["found"] | false, d["x"] | 0.5f, d["y"] | 0.5f, d["w"] | 0.f);
    } else if (!strcmp(type, "photo_request")) {
        uint32_t t0 = millis();
        uint8_t* jpg; size_t n = tracker::captureJpeg(&jpg, d["quality"] | 20);
        log_i("photo: %u bytes in %u ms", (unsigned)n, (unsigned)(millis() - t0));
        if (n) { bool ok = net::sendBin(BIN_PHOTO, jpg, n); free(jpg); if (!ok) log_w("photo: send failed"); }
        else { JsonDocument r; r["type"] = "photo_failed"; net::sendJson(r); }
    } else if (!strcmp(type, "listen")) {
        if (g_state.mode == Mode::Standby) { g_conversation = true; g_endRequested = false; setMode(Mode::Listening); }
    } else if (!strcmp(type, "cancel")) {
        audio::clearPlayback();
        g_conversation = false;
        if (g_state.mode != Mode::Sleep && g_state.mode != Mode::Muted) setMode(Mode::Standby);
    } else if (!strcmp(type, "sleep")) {
        if (g_state.mode != Mode::Sleep) goToSleep();
    } else if (!strcmp(type, "config")) {
        bool changed = false;
        if (d["volume"].is<int>()) { g_settings.volume = d["volume"]; audio::setVolume(g_settings.volume); changed = true; }
        if (d["tracking"].is<bool>()) { g_settings.tracking = d["tracking"]; tracker::setEnabled(g_settings.tracking && g_state.mode != Mode::Sleep); changed = true; }
        if (d["wake_word"].is<bool>()) { g_settings.wakeWord = d["wake_word"]; if (g_state.mode == Mode::Standby) audio::setDirection(g_settings.wakeWord ? AudioDir::Mic : AudioDir::Off); changed = true; }
        if (d["brightness"].is<int>()) { g_settings.brightness = d["brightness"]; M5.Display.setBrightness(g_settings.brightness); changed = true; }
        if (changed) g_settings.save();
    } else if (!strcmp(type, "reboot")) {
        ESP.restart();
    }
}

static void onServerBin(uint8_t tag, const uint8_t* data, size_t len) {
    if (tag == BIN_AUDIO || tag == BIN_AUDIO_ADPCM) {
        if (g_state.mode != Mode::Speaking) setMode(Mode::Speaking);
        const uint8_t* pcm = data; size_t bytes = len;
        static int16_t* dec = nullptr;
        if (tag == BIN_AUDIO_ADPCM) {
            if (!dec) dec = (int16_t*)heap_caps_malloc(SPK_CHUNK_SAMPLES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            size_t n = adpcm::decode(data, len, dec, SPK_CHUNK_SAMPLES * 2);
            pcm = (const uint8_t*)dec; bytes = n * sizeof(int16_t);
        }
        // back-pressure: the net task blocks here briefly if the ring is full
        int tries = 0;
        while (!audio::pushPcm(pcm, bytes) && tries++ < 200) vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---------------------------------------------------------------- telemetry
static void telemetry() {
    uint32_t now = millis();
    if (now - g_lastTelemetry < TELEMETRY_MS) return;
    g_lastTelemetry = now;
    g_state.batteryV = M5StackChan.getBatteryVoltage();
    g_state.batteryMa = M5StackChan.getBatteryCurrent() * 1000.f;
    g_state.batteryPct = M5.Power.getBatteryLevel();
    g_state.charging = M5.Power.isCharging();
    if (!net::connected()) return;
    JsonDocument d;
    d["type"] = "telemetry";
    d["battery_v"] = g_state.batteryV.load();
    d["battery_ma"] = g_state.batteryMa.load();
    d["battery_pct"] = g_state.batteryPct.load();
    d["charging"] = g_state.charging.load();
    d["rssi"] = g_state.rssi.load();
    d["yaw"] = tracker::currentYaw();     // commanded angles: reading the servo bus blocks the loop for 100s of ms
    d["pitch"] = tracker::currentPitch();
    d["target"] = g_state.targetVisible.load();
    d["camera"] = tracker::cameraOk();
    d["heap"] = ESP.getFreeHeap();
    d["underruns"] = audio::underruns();
    d["loop_max_ms"] = g_loopMaxMs; g_loopMaxMs = 0;
    d["psram"] = ESP.getFreePsram();
    d["uptime_s"] = now / 1000;
    net::sendJson(d);
}

// ---------------------------------------------------------------- setup / loop
void setup() {
    Serial.begin(115200);
    g_state.init();
    g_settings.load();

    M5StackChan.begin();          // M5.begin() + touch panel + IO expander (servo power, LEDs) + servos + INA226
    M5.Display.setBrightness(g_settings.brightness);
    face::begin();
    g_state.onUiChange = face::markDirty;
    face::showText("otter-chan", ("fw " OTTER_FW_VERSION "\nbooting..."));

    g_wakeBuf = (int16_t*)heap_caps_malloc(WAKE_CLIP_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    g_preBuf = (int16_t*)heap_caps_calloc(WAKE_PRE_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    g_utterMutex = xSemaphoreCreateMutex();
    g_capMutex = xSemaphoreCreateMutex();

    // Hold the side button during boot -> setup portal. Also if nothing configured yet.
    M5.update();
    bool wantPortal = M5.BtnPWR.isPressed() || !g_settings.hasWifi();
    if (wantPortal) provisioning::startPortal();   // never returns (reboots)

    wakeword::begin();
    audio::begin(onMicFrame);
    audio::setVolume(g_settings.volume);
    net::begin(onServerJson, onServerBin);
    tracker::begin();

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(g_settings.deviceName.c_str());
    WiFi.begin(g_settings.wifiSsid.c_str(), g_settings.wifiPass.c_str());
    setMode(Mode::Connecting);
    g_state.setCaption("connecting to " + g_settings.wifiSsid, 0);
    face::setEnabled(true);
}

void loop() {
    { uint32_t t = millis(); if (g_loopLast && t - g_loopLast > g_loopMaxMs) g_loopMaxMs = t - g_loopLast; g_loopLast = t; }
    M5StackChan.update();   // M5.update() + head touch
    provisioning::pollSerial();
    uint32_t now = millis();
    Mode m = g_state.mode.load();

    // ---- side (power) button ----
    if (M5.BtnPWR.wasHold()) {
        if (m == Mode::Sleep) wakeFromSleep(); else goToSleep();
        return;
    }
    if (m == Mode::Sleep) {
        // stay cheap while asleep; only the button matters
        if (M5.BtnPWR.wasClicked()) wakeFromSleep();
        delay(60);
        return;
    }
    if (M5.BtnPWR.wasDoubleClicked()) {
        if (m == Mode::Muted) { audio::playChime(1); setMode(Mode::Standby); }
        else { audio::clearPlayback(); audio::playChime(2); setMode(Mode::Muted); g_state.setCaption("muted", 1500); }
    } else if (M5.BtnPWR.wasClicked()) {
        switch (m) {
            case Mode::Standby:   g_conversation = true; g_endRequested = false; audio::playChime(0); setMode(Mode::Listening); break;
            case Mode::Listening: g_conversation = false; net::sendJson("listen_cancel"); audio::playChime(1); setMode(Mode::Standby); g_state.setCaption("mic closed", 1200); break;
            case Mode::Thinking:  g_conversation = false; net::sendJson("cancel"); setMode(Mode::Standby); break;
            case Mode::Speaking:  g_conversation = false; net::sendJson("cancel"); audio::clearPlayback(); g_followupPending = false; setMode(Mode::Standby); break;
            case Mode::Muted:     setMode(Mode::Standby); break;
            default: break;
        }
    }

    // ---- on-device wake word ----
    if (wakeword::wasDetected() && m == Mode::Standby && net::connected()) {
        g_conversation = true; g_endRequested = false;
        audio::playChime(0);
        g_state.setExpression(Expression::Surprised, 800);
        setMode(Mode::Listening);
        return;
    }

    // ---- connection state ----
    if (m == Mode::Connecting) {
        if (net::connected()) { g_state.setCaption("", 1); g_state.setExpression(Expression::Happy, 1500); setMode(Mode::Standby); }
        else if (g_state.wifiOk) g_state.setCaption("wifi ok, finding server...", 0);
        else if (now > 30000 && !g_state.wifiOk) g_state.setCaption("no wifi: hold side button at boot for setup", 0);
    } else if (!net::connected() && (m == Mode::Listening || m == Mode::Thinking)) {
        g_state.setExpression(Expression::Confused, 2000);
        g_state.setCaption("lost the server", 3000);
        g_conversation = false;
        setMode(Mode::Standby);
    }

    // ---- listening timeouts ----
    if (m == Mode::Listening) {
        uint32_t since = now - g_lastSpeech;
        if (g_heardSpeech && since > VAD_END_SILENCE_MS) {
            JsonDocument d; d["type"] = "listen_end"; d["reason"] = "silence"; net::sendJson(d);
            setMode(Mode::Thinking);
        } else if (!g_heardSpeech && now - g_listenStart > g_listenWindowMs) {
            net::sendJson("listen_cancel");
            if (g_conversation) { g_conversation = false; g_state.setCaption("session closed", 1500); audio::playChime(1); }
            else g_state.setExpression(Expression::Confused, 1500);
            setMode(Mode::Standby);
        } else if (now - g_listenStart > LISTEN_MAX_MS) {
            JsonDocument d; d["type"] = "listen_end"; d["reason"] = "max"; net::sendJson(d);
            setMode(Mode::Thinking);
        }
    }
    if (m == Mode::Thinking && now - g_listenStart > THINK_TIMEOUT_MS) {   // server never answered
        g_state.setExpression(Expression::Sad, 2000);
        g_state.setCaption("no answer from server", 3000);
        setMode(Mode::Standby);
    }

    // ---- end of speech ----
    if (m == Mode::Speaking) revealCaptions();
    // finished only when every byte the server sent has been played (a dry spell must not end it early)
    bool allPlayed = g_sayTotalBytes == 0 || audio::bytesReceived() >= g_sayTotalBytes;
    if (m == Mode::Speaking && g_sayEnded && allPlayed && audio::isPlaybackIdle()) {
        revealCaptions();
        g_state.mouthOpen = 0.f;
        if (g_endRequested) { g_conversation = false; g_endRequested = false; audio::playChime(1); setMode(Mode::Standby); g_state.setCaption("", 1); }
        else if (g_conversation || g_followupPending) { setMode(Mode::Listening); }
        else { setMode(Mode::Standby); g_state.setCaption("", 1); }
    }

    behaviors::tick();
    leds::tick();
    telemetry();
    delay(10);
}
