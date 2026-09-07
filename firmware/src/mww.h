// microWakeWord on-device detector (see mww.cpp). Compiled in only when src/mww_model.h exists;
// generate it with tools/wakeword/embed_model.py.
#pragma once
#include <Arduino.h>

namespace mww {
bool begin();
bool available();
const char* wakeWord();
void feed(const int16_t* samples, size_t n);   // 16 kHz mono
bool wasDetected();                            // one-shot
void setEnabled(bool on);
}
