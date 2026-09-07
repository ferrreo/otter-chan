// IMA ADPCM, 4 bits per sample, self-contained blocks (4-byte header: int16 predictor, uint8 index, pad).
// Cuts the speech streams to 8 KB/s each way; the TLS WebSocket on this chip only manages ~25 KB/s.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace adpcm {
// out must hold 4 + (n+1)/2 bytes. Returns bytes written.
size_t encode(const int16_t* pcm, size_t n, uint8_t* out);
// Decodes up to maxSamples into pcm; returns samples written.
size_t decode(const uint8_t* data, size_t len, int16_t* pcm, size_t maxSamples);
}
