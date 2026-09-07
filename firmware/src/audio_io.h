// Microphone capture + VAD + speaker playback ring buffer.
// Half duplex: the CoreS3 codec path in M5Unified can't run Mic and Speaker at once,
// so the audio task switches between them based on the requested direction.
#pragma once
#include <Arduino.h>
#include <functional>

enum class AudioDir : uint8_t { Off, Mic, Speaker };

struct VadResult {
    bool speech;        // current frame classified as speech
    float rms;
    float noiseFloor;
};

namespace audio {

// Called from the audio task for every 32 ms mic frame while the mic is on.
using MicFrameHandler = std::function<void(const int16_t* samples, size_t n, const VadResult& vad)>;

void begin(MicFrameHandler handler);
void setDirection(AudioDir d);
AudioDir direction();

// Speaker side: enqueue 16 kHz s16le PCM; returns false if the ring is full (caller retries).
bool pushPcm(const uint8_t* data, size_t bytes);
void clearPlayback();
bool isPlaybackIdle();         // ring empty and speaker finished
size_t playbackBacklogMs();
size_t bytesReceived();        // TTS bytes accepted since the last clearPlayback()
size_t bytesPlayed();          // TTS bytes handed to the DAC since the last clearPlayback()
uint32_t underruns();          // times the speaker ran dry mid-utterance
void setVolume(uint8_t v);
void toneAsync(float freq, uint32_t ms);   // short UI beep; switches to speaker briefly
void playChime(int which);                 // 0 = wake, 1 = ok, 2 = error, 3 = sleep, 4 = notify

}  // namespace audio
