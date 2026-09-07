// WebSocket link to the Otter server. JSON text frames for control, tagged binary frames for media.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>

// Binary frame tags (first byte of every binary frame, both directions)
enum : uint8_t {
    BIN_AUDIO      = 0x01,   // device->server: 16 kHz s16le mic frames; server->device: TTS PCM
    BIN_WAKE_CLIP  = 0x02,   // device->server: one whole wake-word candidate clip
    BIN_PHOTO      = 0x03,   // device->server: JPEG (photo request)
    BIN_TRACK_JPEG = 0x04,   // device->server: small JPEG for face-detect assist
};

namespace net {

using JsonHandler = std::function<void(JsonDocument& doc)>;
using BinHandler  = std::function<void(uint8_t tag, const uint8_t* data, size_t len)>;

void begin(JsonHandler onJson, BinHandler onBin);
bool connected();
void sendJson(const JsonDocument& doc);
void sendJson(const char* type);                       // {"type": type}
// Queue a binary frame (copied). Audio frames are dropped when the queue is saturated.
bool sendBin(uint8_t tag, const uint8_t* data, size_t len, bool dropIfFull = false);
void reconnect();                                      // forces a reconnect (e.g. after settings change)

}  // namespace net
