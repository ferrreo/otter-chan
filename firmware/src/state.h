// Shared robot state — written by the main state machine, read by render/audio/net tasks.
#pragma once
#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

enum class Mode : uint8_t {
    Boot,
    Provision,
    Connecting,
    Standby,     // mic on for wake word (if enabled), idle animations
    Listening,   // streaming an utterance to the server
    Thinking,    // waiting on STT/LLM
    Speaking,    // playing TTS
    Muted,       // hard mute: mic fully off, no wake word
    Sleep,
};

enum class Expression : uint8_t {
    Neutral, Happy, Sad, Surprised, Thinking, Sleepy, Angry, Love, Confused, Wink, Error
};

struct BotStatus {
    char name[24]     = {0};
    char activity[16] = {0};   // idle|coding|browsing|thinking|talking|error|offline|...
    char detail[48]   = {0};
    char emoji[8]     = {0};
    uint32_t updatedMs = 0;
};

constexpr int MAX_BOTS = 6;

struct RobotState {
    std::atomic<Mode> mode{Mode::Boot};
    std::atomic<Expression> expression{Expression::Neutral};
    std::atomic<uint32_t> expressionUntilMs{0};   // 0 = sticky
    std::atomic<float> mouthOpen{0.f};            // 0..1, driven by TTS RMS
    std::atomic<float> micLevel{0.f};             // 0..1, driven by mic RMS
    std::atomic<float> gazeX{0.f};                // -1..1, where the eyes look
    std::atomic<float> gazeY{0.f};
    std::atomic<bool> targetVisible{false};
    std::atomic<bool> wifiOk{false};
    std::atomic<bool> serverOk{false};
    std::atomic<int>  rssi{0};
    std::atomic<float> batteryV{0.f};
    std::atomic<float> batteryMa{0.f};
    std::atomic<int>  batteryPct{-1};
    std::atomic<bool> charging{false};
    std::atomic<bool> voiceDetected{false};

    // Text shown under the face (transcript / reply / notification). Guarded by mutex.
    SemaphoreHandle_t mutex = nullptr;
    String caption;
    uint32_t captionUntilMs = 0;
    String statusLine;   // small top-right text, e.g. "listening"
    BotStatus bots[MAX_BOTS];
    int botCount = 0;

    void init() { mutex = xSemaphoreCreateMutex(); }
    void lock()   { xSemaphoreTake(mutex, portMAX_DELAY); }
    void unlock() { xSemaphoreGive(mutex); }

    void setCaption(const String& s, uint32_t ms = 6000) {
        lock(); caption = s; captionUntilMs = ms ? millis() + ms : 0; unlock();
    }
    void setExpression(Expression e, uint32_t ms = 0) {
        expression = e; expressionUntilMs = ms ? millis() + ms : 0;
    }
};

extern RobotState g_state;
const char* modeName(Mode m);
const char* expressionName(Expression e);
bool expressionFromName(const char* n, Expression& out);
