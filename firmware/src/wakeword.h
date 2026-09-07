// On-device wake word: Espressif MultiNet7 (English) running continuously in command mode with
// "tarquin" as the command, so waking needs no network round-trip. Models live in the `model`
// partition (flash with `pio run -t upload_models`). Falls back to server-side clips if absent.
#pragma once
#include <Arduino.h>

namespace wakeword {
bool begin();                                   // true if the models loaded and detection runs
bool available();
void feed(const int16_t* samples, size_t n);    // 16 kHz mono; call from the mic task while in standby
bool wasDetected();                             // one-shot: true once per detection
void setEnabled(bool on);
}
