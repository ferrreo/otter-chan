// microWakeWord streaming detector (port of ESPHome's micro_wake_word, Apache-2.0):
// 16 kHz PCM -> TFLM audio frontend (40 log-mel features every 10 ms) -> int8 -> streaming
// MixConv model -> probability sliding window. Model + thresholds come from mww_model.h.
#include "mww.h"
#include "config.h"
#if __has_include("mww_model.h")
#include "mww_model.h"
#define MWW_HAVE_MODEL 1
#else
#define MWW_HAVE_MODEL 0
#endif

#if MWW_HAVE_MODEL
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <freertos/stream_buffer.h>
#include <atomic>
#include <algorithm>
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"
extern "C" {
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
}

namespace {

constexpr int FEATURES = 40;
constexpr int FEATURE_DURATION_MS = 30;
constexpr int MIN_SLICES_BEFORE_DETECTION = 100;   // 1 s cool-off after a detection

FrontendConfig g_fcfg;
FrontendState g_fstate;
tflite::MicroMutableOpResolver<24> g_ops;
tflite::MicroInterpreter* g_interp = nullptr;
uint8_t* g_arena = nullptr;
uint8_t* g_varArena = nullptr;
tflite::MicroAllocator* g_varAlloc = nullptr;
tflite::MicroResourceVariables* g_mrv = nullptr;
uint8_t g_probs[MWW_SLIDING_WINDOW];
int g_probIdx = 0;
int g_ignoreWindows = -MIN_SLICES_BEFORE_DETECTION;
int g_strideStep = 0;
StreamBufferHandle_t g_stream = nullptr;
StaticStreamBuffer_t g_streamCtl;
std::atomic<bool> g_enabled{false}, g_detected{false}, g_ok{false};
uint32_t g_lastLogMs = 0;
uint8_t g_lastMax = 0;

bool setupFrontend() {
    FrontendFillConfigWithDefaults(&g_fcfg);
    g_fcfg.window.size_ms = FEATURE_DURATION_MS;
    g_fcfg.window.step_size_ms = MWW_FEATURE_STEP_MS;
    g_fcfg.filterbank.num_channels = FEATURES;
    g_fcfg.filterbank.lower_band_limit = 125.0f;
    g_fcfg.filterbank.upper_band_limit = 7500.0f;
    g_fcfg.noise_reduction.smoothing_bits = 10;
    g_fcfg.noise_reduction.even_smoothing = 0.025f;
    g_fcfg.noise_reduction.odd_smoothing = 0.06f;
    g_fcfg.noise_reduction.min_signal_remaining = 0.05f;
    g_fcfg.pcan_gain_control.enable_pcan = 1;
    g_fcfg.pcan_gain_control.strength = 0.95f;
    g_fcfg.pcan_gain_control.offset = 80.0f;
    g_fcfg.pcan_gain_control.gain_bits = 21;
    g_fcfg.log_scale.enable_log = 1;
    g_fcfg.log_scale.scale_shift = 6;
    return FrontendPopulateState(&g_fcfg, &g_fstate, AUDIO_RATE) != 0;
}

bool setupModel() {
    g_ops.AddCallOnce(); g_ops.AddVarHandle(); g_ops.AddReshape(); g_ops.AddReadVariable();
    g_ops.AddStridedSlice(); g_ops.AddConcatenation(); g_ops.AddAssignVariable(); g_ops.AddConv2D();
    g_ops.AddMul(); g_ops.AddAdd(); g_ops.AddMean();
    // extras some exports use
    g_ops.AddDepthwiseConv2D(); g_ops.AddFullyConnected(); g_ops.AddLogistic(); g_ops.AddSoftmax();
    g_ops.AddQuantize(); g_ops.AddDequantize(); g_ops.AddPad(); g_ops.AddMaxPool2D(); g_ops.AddAveragePool2D();
    g_ops.AddSqueeze(); g_ops.AddExpandDims(); g_ops.AddSub(); g_ops.AddSplitV(); g_ops.AddSplit();
    const tflite::Model* model = tflite::GetModel(MWW_MODEL);
    if (model->version() != TFLITE_SCHEMA_VERSION) { log_e("mww: schema %lu != %d", (unsigned long)model->version(), TFLITE_SCHEMA_VERSION); return false; }
    // Internal RAM is precious (TLS needs ~40 KB); keep the arena modest and fall back to PSRAM.
    size_t arena = (MWW_TENSOR_ARENA * 3 / 2 + 15) & ~15;
    g_arena = (uint8_t*)heap_caps_aligned_alloc(16, arena, MALLOC_CAP_SPIRAM);   // PSRAM: internal RAM is for TLS/WiFi
    if (!g_arena) { log_e("mww: no RAM for arena"); return false; }
    // streaming models keep state in resource variables; they need their own allocator
    g_varArena = (uint8_t*)heap_caps_aligned_alloc(16, 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    g_varAlloc = tflite::MicroAllocator::Create(g_varArena, 1024);
    g_mrv = tflite::MicroResourceVariables::Create(g_varAlloc, 20);
    g_interp = new tflite::MicroInterpreter(model, g_ops, g_arena, arena, g_mrv);
    if (g_interp->AllocateTensors() != kTfLiteOk) {
        log_e("mww: AllocateTensors failed (arena %u, model %u bytes, %u subgraphs, %u ops)", (unsigned)arena, (unsigned)MWW_MODEL_SIZE,
              (unsigned)model->subgraphs()->size(), (unsigned)model->operator_codes()->size());
        for (unsigned i = 0; i < model->operator_codes()->size(); i++) {
            auto* oc = model->operator_codes()->Get(i);
            log_e("mww:   op %u builtin=%d (%s)", i, (int)oc->builtin_code(), tflite::EnumNameBuiltinOperator(oc->builtin_code()));
        }
        return false;
    }
    TfLiteTensor* in = g_interp->input(0);
    if (in->dims->size != 3 || in->dims->data[2] != FEATURES || in->type != kTfLiteInt8) { log_e("mww: unexpected input"); return false; }
    TfLiteTensor* out = g_interp->output(0);
    if (out->type != kTfLiteUInt8) { log_e("mww: unexpected output"); return false; }
    log_i("mww: '%s' loaded, stride %d, arena %u/%u used", MWW_WAKE_WORD, in->dims->data[1], (unsigned)g_interp->arena_used_bytes(), (unsigned)arena);
    return true;
}

void resetProbabilities() {
    for (auto& p : g_probs) p = 0;
    g_ignoreWindows = -MIN_SLICES_BEFORE_DETECTION;
}

// One 40-feature slice into the streaming model. Returns true when a detection fired.
bool pushFeatures(const int8_t* feat) {
    TfLiteTensor* in = g_interp->input(0);
    int stride = in->dims->data[1];
    g_strideStep %= stride;
    memcpy(tflite::GetTensorData<int8_t>(in) + FEATURES * g_strideStep, feat, FEATURES);
    if (++g_strideStep < stride) return false;
    if (g_interp->Invoke() != kTfLiteOk) { log_w("mww: invoke failed"); return false; }
    uint8_t p = g_interp->output(0)->data.uint8[0];
    g_probIdx = (g_probIdx + 1) % MWW_SLIDING_WINDOW;
    g_probs[g_probIdx] = p;
    if (p < MWW_PROBABILITY_CUTOFF) g_ignoreWindows = std::min(g_ignoreWindows + 1, 0);
    if (g_ignoreWindows < 0) return false;
    uint32_t sum = 0; uint8_t mx = 0;
    for (auto v : g_probs) { sum += v; mx = std::max(mx, v); }
    g_lastMax = std::max(g_lastMax, mx);
    if (sum > (uint32_t)MWW_PROBABILITY_CUTOFF * MWW_SLIDING_WINDOW) {
        log_i("mww: '%s' detected (avg %u/255)", MWW_WAKE_WORD, (unsigned)(sum / MWW_SLIDING_WINDOW));
        resetProbabilities();
        return true;
    }
    return false;
}

void detectTask(void*) {
    int16_t* buf = (int16_t*)heap_caps_malloc(MIC_FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    for (;;) {
        size_t got = xStreamBufferReceive(g_stream, buf, MIC_FRAME_SAMPLES * sizeof(int16_t), portMAX_DELAY);
        size_t n = got / sizeof(int16_t);
        if (!g_enabled || n == 0) continue;
        uint32_t t0 = micros();
        size_t off = 0;
        while (off < n) {
            size_t read = 0;
            FrontendOutput o = FrontendProcessSamples(&g_fstate, buf + off, n - off, &read);
            off += read;
            if (o.size == 0) { if (read == 0) break; continue; }
            int8_t feat[FEATURES];
            for (size_t i = 0; i < o.size && i < FEATURES; i++) {
                // matches the training-side scaling: (feature * 256) / (25.6 * 26) - 128
                int32_t v = ((int32_t)o.values[i] * 256 + 333) / 666 + INT8_MIN;
                feat[i] = (int8_t)std::max<int32_t>(INT8_MIN, std::min<int32_t>(INT8_MAX, v));
            }
            if (pushFeatures(feat)) g_detected = true;
        }
        static uint32_t acc = 0, cnt = 0;
        acc += micros() - t0; cnt++;
        if (millis() - g_lastLogMs > 30000) {
            g_lastLogMs = millis();
            log_i("mww: %.1f ms per 32 ms frame, max prob %u/255, internal heap %u", cnt ? acc / 1000.f / cnt : 0.f, g_lastMax, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            acc = cnt = 0; g_lastMax = 0;
        }
        taskYIELD();
    }
}

}  // namespace

namespace mww {

bool begin() {
    if (!setupFrontend()) { log_e("mww: frontend init failed"); return false; }
    if (!setupModel()) return false;
    resetProbabilities();
    const size_t streamBytes = AUDIO_RATE * sizeof(int16_t);
    uint8_t* storage = (uint8_t*)heap_caps_malloc(streamBytes + 1, MALLOC_CAP_SPIRAM);
    g_stream = xStreamBufferCreateStatic(streamBytes, 1, storage, &g_streamCtl);
    xTaskCreatePinnedToCoreWithCaps(detectTask, "mww", 8192, nullptr, 3, nullptr, 1, MALLOC_CAP_SPIRAM);
    g_ok = true; g_enabled = true;
    return true;
}
bool available() { return g_ok; }
const char* wakeWord() { return MWW_WAKE_WORD; }
void feed(const int16_t* s, size_t n) { if (g_ok && g_enabled) xStreamBufferSend(g_stream, s, n * sizeof(int16_t), 0); }
bool wasDetected() { return g_detected.exchange(false); }
void setEnabled(bool on) { g_enabled = on; if (!on && g_stream) xStreamBufferReset(g_stream); }

}  // namespace mww

#else  // no model header

namespace mww {
bool begin() { return false; }
bool available() { return false; }
const char* wakeWord() { return ""; }
void feed(const int16_t*, size_t) {}
bool wasDetected() { return false; }
void setEnabled(bool) {}
}  // namespace mww

#endif
