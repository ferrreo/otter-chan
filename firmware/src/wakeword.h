// On-device wake word: microWakeWord streaming model (see mww.cpp) fed straight from the mic task.
// No network round-trip; the server-side clip check remains as a fallback if no model is compiled in.
#pragma once
#include <Arduino.h>

namespace wakeword {
bool begin();                                   // true if the model loaded and detection runs
bool available();
void feed(const int16_t* samples, size_t n);    // 16 kHz mono, continuous, call from the mic task in standby
bool wasDetected();                             // one-shot: true once per detection
void setEnabled(bool on);
}
