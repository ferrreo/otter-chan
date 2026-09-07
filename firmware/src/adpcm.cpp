#include "adpcm.h"

namespace {
const int8_t kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int16_t kStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

namespace adpcm {

size_t encode(const int16_t* pcm, size_t n, uint8_t* out) {
    size_t bytes = 4 + (n + 1) / 2;
    for (size_t i = 0; i < bytes; i++) out[i] = 0;
    if (n == 0) return bytes;
    int pred = pcm[0], index = 0;
    out[0] = (uint8_t)pred; out[1] = (uint8_t)(pred >> 8); out[2] = 0;
    for (size_t i = 0; i < n; i++) {
        int step = kStep[index];
        int diff = pcm[i] - pred;
        int nibble = 0;
        if (diff < 0) { nibble = 8; diff = -diff; }
        int delta = 0;
        if (diff >= step) { nibble |= 4; diff -= step; delta += step; }
        if (diff >= (step >> 1)) { nibble |= 2; diff -= step >> 1; delta += step >> 1; }
        if (diff >= (step >> 2)) { nibble |= 1; delta += step >> 2; }
        delta += step >> 3;
        pred = clampi((nibble & 8) ? pred - delta : pred + delta, -32768, 32767);
        index = clampi(index + kIndex[nibble], 0, 88);
        if (i & 1) out[4 + i / 2] |= (uint8_t)(nibble << 4); else out[4 + i / 2] = (uint8_t)nibble;
    }
    return bytes;
}

size_t decode(const uint8_t* data, size_t len, int16_t* pcm, size_t maxSamples) {
    if (len < 4) return 0;
    int pred = (int16_t)(data[0] | (data[1] << 8));
    int index = clampi(data[2], 0, 88);
    size_t total = (len - 4) * 2;
    if (total > maxSamples) total = maxSamples;
    for (size_t i = 0; i < total; i++) {
        int nibble = (i & 1) ? (data[4 + i / 2] >> 4) : (data[4 + i / 2] & 0x0f);
        int step = kStep[index];
        int delta = step >> 3;
        if (nibble & 4) delta += step;
        if (nibble & 2) delta += step >> 1;
        if (nibble & 1) delta += step >> 2;
        pred = clampi((nibble & 8) ? pred - delta : pred + delta, -32768, 32767);
        index = clampi(index + kIndex[nibble], 0, 88);
        pcm[i] = (int16_t)pred;
    }
    return total;
}

}  // namespace adpcm
