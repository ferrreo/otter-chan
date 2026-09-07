// Otter-chan firmware — compile-time configuration
#pragma once
#include <stdint.h>

#ifndef OTTER_FW_VERSION
#define OTTER_FW_VERSION "dev"
#endif

// ---- Compile-time defaults (override in secrets.ini; NVS settings win over these) ----
#ifndef OTTER_WIFI_SSID
#define OTTER_WIFI_SSID ""
#endif
#ifndef OTTER_WIFI_PASS
#define OTTER_WIFI_PASS ""
#endif
#ifndef OTTER_SERVER_URL
#define OTTER_SERVER_URL "wss://bot.ferreo.dev/ws/device"
#endif
#ifndef OTTER_DEVICE_TOKEN
#define OTTER_DEVICE_TOKEN ""
#endif
#ifndef OTTER_DEVICE_NAME
#define OTTER_DEVICE_NAME "tarquin"
#endif

// ---- Audio ----
constexpr uint32_t AUDIO_RATE          = 16000;   // Hz, both directions, s16le mono
constexpr size_t   MIC_FRAME_SAMPLES   = 512;     // 32 ms per mic frame
constexpr size_t   MIC_QUEUE_FRAMES    = 24;      // ~0.77 s of backlog before drop
constexpr size_t   WAKE_CLIP_SAMPLES   = AUDIO_RATE * 5 / 2;  // 2.5 s wake-word clip
constexpr size_t   WAKE_PRE_SAMPLES    = AUDIO_RATE / 4;      // 250 ms kept before VAD onset
constexpr size_t   SPK_CHUNK_SAMPLES   = 2048;    // 128 ms playback chunks
constexpr size_t   SPK_RING_CHUNKS     = 24;      // ~3 s of buffered TTS
constexpr uint8_t  DEFAULT_VOLUME      = 160;     // 0..255

// VAD (energy based, adaptive noise floor)
constexpr float    VAD_ONSET_RATIO     = 3.0f;    // rms must exceed floor * ratio
constexpr float    VAD_MIN_RMS         = 220.0f;  // absolute minimum to count as speech
constexpr uint32_t VAD_ONSET_MS        = 96;      // consecutive speech to trigger
constexpr uint32_t VAD_END_SILENCE_MS  = 900;     // trailing silence ends an utterance
constexpr uint32_t LISTEN_NO_SPEECH_MS = 6000;    // cancel if nobody talks
constexpr uint32_t LISTEN_FOLLOWUP_MS  = 4000;    // shorter window after a reply
constexpr uint32_t LISTEN_MAX_MS       = 20000;   // hard cap per utterance
constexpr uint32_t WAKE_MIN_SPEECH_MS  = 250;     // ignore very short bursts for wake clips
constexpr uint32_t WAKE_COOLDOWN_MS    = 1500;

// ---- Camera / tracking ----
constexpr int      CAM_W               = 640;
constexpr int      CAM_H               = 480;
constexpr int      TRK_DS              = 8;       // downsample factor for motion map (80x60)
constexpr int      TRK_W               = CAM_W / TRK_DS;
constexpr int      TRK_H               = CAM_H / TRK_DS;
constexpr int      TRK_DIFF_THRESHOLD  = 28;      // luma delta that counts as motion
constexpr int      TRK_MIN_PIXELS      = 25;      // motion blobs smaller than this are noise
constexpr float    TRK_SMOOTH          = 0.35f;   // EMA on target position
constexpr float    TRK_DEADBAND        = 0.08f;   // normalized; don't chase tiny errors
constexpr float    CAM_HFOV_DEG        = 62.0f;   // GC0308 approx horizontal FOV
constexpr float    CAM_VFOV_DEG        = 48.0f;
constexpr float    TRK_GAIN            = 0.55f;   // fraction of the angular error corrected per step
constexpr int      TRK_YAW_SIGN        = 1;       // flip if the head turns away from you
constexpr int      TRK_PITCH_SIGN      = 1;
constexpr uint32_t TRK_LOST_MS         = 6000;    // return to home after this long without target
constexpr uint32_t TRK_ASSIST_MS       = 1000;     // server face-detect assist interval (0 = off)
constexpr int      TRK_ASSIST_JPEG_Q   = 40;

// ---- Servo ranges (BSP units: 10 = 1 degree) ----
constexpr int      YAW_MIN             = -900;
constexpr int      YAW_MAX             = 900;
constexpr int      PITCH_MIN           = 50;      // M5 recommends 5..85 deg for the pitch servo
constexpr int      PITCH_MAX           = 800;
constexpr int      PITCH_HOME          = 120;     // slightly above level: a seated face from a desk

// ---- Misc ----
constexpr uint32_t TELEMETRY_MS        = 10000;
constexpr uint32_t IDLE_ANIM_MIN_MS    = 6000;
constexpr uint32_t IDLE_ANIM_MAX_MS    = 16000;
constexpr uint32_t BUTTON_HOLD_MS      = 1200;    // hold side button -> sleep / wake
constexpr uint32_t WS_RECONNECT_MS     = 3000;
constexpr uint32_t PROVISION_PORTAL_MS = 15 * 60 * 1000;
