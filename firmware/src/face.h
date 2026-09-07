// Avatar renderer: a cute round robot face on a full-screen PSRAM canvas.
#pragma once
#include <Arduino.h>

namespace face {
void begin();                 // creates the canvas and starts the render task
void setEnabled(bool on);     // pause rendering (sleep)
void showText(const char* title, const char* body);  // non-face full-screen text (boot/info)
void pokeReaction();          // eyes squint / bounce (touch)
}
