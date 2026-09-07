// Camera-based user tracking: on-device motion centroid + optional server face-detect assist.
#pragma once
#include <Arduino.h>

namespace tracker {
bool begin();                       // init camera; returns false if the camera is unavailable
void setEnabled(bool on);           // start/stop the tracking loop (servo follow)
bool enabled();
bool cameraOk();
// Server face-detect assist result (normalized 0..1 image coords); found=false clears it.
void onFaceResult(bool found, float x, float y, float w);
// Capture a JPEG of the current view (allocated with malloc; caller frees). Returns size or 0.
size_t captureJpeg(uint8_t** out, int quality);
void lookAt(float nx, float ny, int speed);   // manual look (normalized -1..1), pauses tracking briefly
void goHome();
void nod(); void shake(); void dance();       // scripted moves (non-blocking, run on the tracker task)
}
