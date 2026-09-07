// LEDs, idle fidgets, head-touch / shake reactions.
#pragma once
#include <Arduino.h>

namespace leds {
enum class Pattern : uint8_t { Off, Solid, Pulse, Rainbow, Listening, Thinking, Speaking, Error, Notify };
void set(Pattern p, uint8_t r = 0, uint8_t g = 0, uint8_t b = 0, uint32_t ms = 0);  // ms=0 sticky
void tick();   // call every loop (~50 ms)
}

namespace behaviors {
void tick();   // idle fidgets, IMU shake, head touch. Call every loop.
}
