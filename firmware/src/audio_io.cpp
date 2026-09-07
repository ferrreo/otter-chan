#include "audio_io.h"
#include "config.h"
#include "state.h"
#include <M5Unified.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace {

audio::MicFrameHandler g_handler;
std::atomic<AudioDir> g_requested{AudioDir::Off};
AudioDir g_current = AudioDir::Off;
uint8_t g_volume = DEFAULT_VOLUME;

// Mic buffers: 3 rotating buffers, M5Unified keeps 2 requests in flight.
int16_t* g_micBuf[3];

// Speaker ring of fixed-size chunks (PSRAM). Each slot holds up to SPK_CHUNK_SAMPLES.
struct Chunk { int16_t* pcm; size_t samples; };
Chunk g_ring[SPK_RING_CHUNKS];
volatile size_t g_ringHead = 0;   // next slot to write (producer)
volatile size_t g_ringTail = 0;   // next slot to play (consumer)
SemaphoreHandle_t g_ringMutex;
// partial chunk accumulation from the network
int16_t* g_partial;
size_t g_partialSamples = 0;
std::atomic<size_t> g_rxBytes{0}, g_playedBytes{0};
std::atomic<uint32_t> g_underruns{0};
bool g_wasPlaying = false;
uint32_t g_micStartedMs = 0;
// last two chunks handed to M5.Speaker must stay valid; we hand out ring slots directly
// and only recycle a slot once the speaker reports it finished (isPlaying < 2 keeps 1 in flight).

std::atomic<int> g_toneReq{-1};
std::atomic<uint32_t> g_toneMs{0};
std::atomic<float> g_toneFreq{0};

// ---- VAD ----
float g_noise = 400.f;
uint32_t g_speechRunMs = 0;
uint32_t g_silenceRunMs = 0;

VadResult runVad(const int16_t* s, size_t n) {
    double acc = 0;
    for (size_t i = 0; i < n; i++) acc += (double)s[i] * s[i];
    float rms = sqrtf((float)(acc / n));
    // adaptive floor: fast to fall, slow to rise
    if (rms < g_noise) g_noise += (rms - g_noise) * 0.15f;
    else g_noise += (rms - g_noise) * 0.004f;
    if (g_noise < 60.f) g_noise = 60.f;
    bool loud = rms > fmaxf(VAD_MIN_RMS, g_noise * VAD_ONSET_RATIO);
    const uint32_t frameMs = MIC_FRAME_SAMPLES * 1000 / AUDIO_RATE;
    if (loud) { g_speechRunMs += frameMs; g_silenceRunMs = 0; }
    else { g_silenceRunMs += frameMs; if (g_silenceRunMs > 200) g_speechRunMs = 0; }
    bool speech = g_speechRunMs >= VAD_ONSET_MS || (loud && g_silenceRunMs == 0 && g_speechRunMs > 0);
    g_state.micLevel = fminf(1.f, rms / 4000.f);
    return {speech, rms, g_noise};
}

void startMic() {
    M5.Speaker.end();
    auto cfg = M5.Mic.config();
    cfg.sample_rate = AUDIO_RATE;
    cfg.stereo = false;
    cfg.magnification = 24;
    cfg.dma_buf_count = 6;
    cfg.dma_buf_len = 512;
    M5.Mic.config(cfg);
    M5.Mic.begin();
    g_noise = 400.f; g_speechRunMs = g_silenceRunMs = 0;
    g_micStartedMs = millis();
}

void startSpeaker() {
    M5.Mic.end();
    auto cfg = M5.Speaker.config();
    cfg.sample_rate = 48000;   // internal DAC rate; playRaw resamples from 16 k
    cfg.stereo = false;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 512;
    cfg.task_priority = 3;
    M5.Speaker.config(cfg);
    M5.Speaker.begin();
    M5.Speaker.setVolume(g_volume);
}

void stopAll() {
    M5.Mic.end();
    M5.Speaker.stop();
    M5.Speaker.end();
}

void applyDirection() {
    AudioDir want = g_requested.load();
    if (want == g_current) return;
    switch (want) {
        case AudioDir::Mic: startMic(); break;
        case AudioDir::Speaker: startSpeaker(); break;
        case AudioDir::Off: stopAll(); break;
    }
    g_current = want;
}

void micStep() {
    static int idx = 0;
    // Submit current buffer; when this returns, the buffer submitted two calls ago is complete.
    M5.Mic.record(g_micBuf[idx], MIC_FRAME_SAMPLES, AUDIO_RATE);
    int done = (idx + 1) % 3;
    idx = (idx + 1) % 3;
    static int primed = 0;
    if (primed < 2) { primed++; return; }
    VadResult v = runVad(g_micBuf[done], MIC_FRAME_SAMPLES);
    if (millis() - g_micStartedMs < MIC_SETTLE_MS) { v.speech = false; g_speechRunMs = 0; }   // codec switch-over / echo tail
    if (g_handler) g_handler(g_micBuf[done], MIC_FRAME_SAMPLES, v);
}

void speakerStep() {
    // Feed the speaker while it has room (M5Unified keeps up to 2 playRaw requests per channel).
    while (M5.Speaker.isPlaying(0) < 2) {
        xSemaphoreTake(g_ringMutex, portMAX_DELAY);
        bool have = g_ringTail != g_ringHead;
        Chunk c = have ? g_ring[g_ringTail % SPK_RING_CHUNKS] : Chunk{nullptr, 0};
        if (have) g_ringTail++;
        xSemaphoreGive(g_ringMutex);
        if (!have) break;
        // mouth animation from chunk energy
        double acc = 0;
        for (size_t i = 0; i < c.samples; i += 4) acc += (double)c.pcm[i] * c.pcm[i];
        float rms = sqrtf((float)(acc / (c.samples / 4)));
        g_state.mouthOpen = fminf(1.f, rms / 6000.f);
        g_playedBytes += c.samples * sizeof(int16_t);
        M5.Speaker.playRaw(c.pcm, c.samples, AUDIO_RATE, false, 1, 0, false);
    }
    bool playing = M5.Speaker.isPlaying(0) != 0;
    if (!playing) g_state.mouthOpen = 0.f;
    // a dry spell = speaker idle while we are mid-utterance and the ring has nothing queued
    xSemaphoreTake(g_ringMutex, portMAX_DELAY);
    bool ringEmpty = g_ringTail == g_ringHead;
    xSemaphoreGive(g_ringMutex);
    if (g_wasPlaying && !playing && ringEmpty && g_state.mode == Mode::Speaking) g_underruns++;
    g_wasPlaying = playing;
    vTaskDelay(pdMS_TO_TICKS(8));
}

void audioTask(void*) {
    for (;;) {
        applyDirection();
        int tone = g_toneReq.exchange(-1);
        if (tone >= 0) {
            AudioDir prev = g_current;
            if (g_current != AudioDir::Speaker) { startSpeaker(); g_current = AudioDir::Speaker; }
            M5.Speaker.tone(g_toneFreq.load(), g_toneMs.load(), 1, true);
            while (M5.Speaker.isPlaying(1)) vTaskDelay(pdMS_TO_TICKS(5));
            if (prev != AudioDir::Speaker) { g_requested = prev; }
            continue;
        }
        switch (g_current) {
            case AudioDir::Mic: micStep(); break;
            case AudioDir::Speaker: speakerStep(); break;
            default: vTaskDelay(pdMS_TO_TICKS(20)); break;
        }
    }
}

}  // namespace

namespace audio {

void begin(MicFrameHandler handler) {
    g_handler = handler;
    for (auto& b : g_micBuf) b = (int16_t*)heap_caps_malloc(MIC_FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    for (auto& c : g_ring) { c.pcm = (int16_t*)heap_caps_malloc(SPK_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM); c.samples = 0; }
    g_partial = (int16_t*)heap_caps_malloc(SPK_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    g_ringMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 6, nullptr, 1);
}

void setDirection(AudioDir d) {
    if (d != AudioDir::Speaker) { /* keep ring for later */ }
    g_requested = d;
}
AudioDir direction() { return g_current; }

static bool pushChunk(const int16_t* pcm, size_t samples) {
    xSemaphoreTake(g_ringMutex, portMAX_DELAY);
    // leave one slot of slack so the chunk currently queued in the DAC isn't overwritten
    bool full = (g_ringHead - g_ringTail) >= SPK_RING_CHUNKS - 2;
    if (!full) {
        Chunk& c = g_ring[g_ringHead % SPK_RING_CHUNKS];
        memcpy(c.pcm, pcm, samples * sizeof(int16_t));
        c.samples = samples;
        g_ringHead++;
    }
    xSemaphoreGive(g_ringMutex);
    return !full;
}

bool pushPcm(const uint8_t* data, size_t bytes) {
    // Accumulate into fixed chunks. Returns false only if the ring is full before consuming anything.
    xSemaphoreTake(g_ringMutex, portMAX_DELAY);
    size_t free = SPK_RING_CHUNKS - 2 - (g_ringHead - g_ringTail);
    xSemaphoreGive(g_ringMutex);
    size_t need = (g_partialSamples + bytes / 2 + SPK_CHUNK_SAMPLES - 1) / SPK_CHUNK_SAMPLES;
    if (need > free) return false;
    const int16_t* s = (const int16_t*)data;
    size_t n = bytes / 2;
    g_rxBytes += bytes;
    while (n) {
        size_t take = min(n, SPK_CHUNK_SAMPLES - g_partialSamples);
        memcpy(g_partial + g_partialSamples, s, take * sizeof(int16_t));
        g_partialSamples += take; s += take; n -= take;
        if (g_partialSamples == SPK_CHUNK_SAMPLES) { pushChunk(g_partial, SPK_CHUNK_SAMPLES); g_partialSamples = 0; }
    }
    return true;
}

static void flushPartial() {
    if (g_partialSamples) { pushChunk(g_partial, g_partialSamples); g_partialSamples = 0; }
}

void clearPlayback() {
    xSemaphoreTake(g_ringMutex, portMAX_DELAY);
    g_ringTail = g_ringHead; g_partialSamples = 0;
    g_rxBytes = 0; g_playedBytes = 0;
    xSemaphoreGive(g_ringMutex);
    M5.Speaker.stop(0);
    g_state.mouthOpen = 0.f;
}

bool isPlaybackIdle() {
    if (g_partialSamples) flushPartial();
    xSemaphoreTake(g_ringMutex, portMAX_DELAY);
    bool empty = g_ringTail == g_ringHead;
    xSemaphoreGive(g_ringMutex);
    return empty && (g_current != AudioDir::Speaker || M5.Speaker.isPlaying(0) == 0);
}

size_t playbackBacklogMs() {
    xSemaphoreTake(g_ringMutex, portMAX_DELAY);
    size_t n = g_ringHead - g_ringTail;
    xSemaphoreGive(g_ringMutex);
    return n * SPK_CHUNK_SAMPLES * 1000 / AUDIO_RATE;
}

size_t bytesReceived() { return g_rxBytes; }
size_t bytesPlayed() { return g_playedBytes; }
uint32_t underruns() { return g_underruns; }

void setVolume(uint8_t v) { g_volume = v; M5.Speaker.setVolume(v); }

void toneAsync(float freq, uint32_t ms) { g_toneFreq = freq; g_toneMs = ms; g_toneReq = 1; }

void playChime(int which) {
    switch (which) {
        case 0: toneAsync(1320, 90); break;   // wake: bright blip
        case 1: toneAsync(880, 70); break;    // ok
        case 2: toneAsync(220, 250); break;   // error
        case 3: toneAsync(440, 200); break;   // sleep
        case 4: toneAsync(1046, 120); break;  // notify
    }
}

}  // namespace audio
