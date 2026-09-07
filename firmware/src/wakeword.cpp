#include "wakeword.h"
#include "config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>
#include <atomic>

extern "C" {
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
}

namespace {

const esp_mn_iface_t* g_mn = nullptr;
model_iface_data_t* g_model = nullptr;
srmodel_list_t* g_models = nullptr;
StreamBufferHandle_t g_stream = nullptr;
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_detected{false};
std::atomic<bool> g_ok{false};
int g_chunk = 0;

// Command ids: anything >= 1 counts as the wake word. Spelled the way MultiNet's g2p likes it.
const struct { int id; const char* text; } kCommands[] = {
    {1, "TARQUIN"},
    {2, "HEY TARQUIN"},
    {3, "OK TARQUIN"},
    {4, "OKAY TARQUIN"},
    {5, "TAR QUIN"},
};

void detectTask(void*) {
    int16_t* buf = (int16_t*)heap_caps_malloc(g_chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    for (;;) {
        size_t got = xStreamBufferReceive(g_stream, buf, g_chunk * sizeof(int16_t), portMAX_DELAY);
        if (got < (size_t)g_chunk * sizeof(int16_t)) {
            // partial read (buffer reset); top up
            size_t need = g_chunk * sizeof(int16_t) - got;
            got += xStreamBufferReceive(g_stream, (uint8_t*)buf + got, need, portMAX_DELAY);
            if (got < (size_t)g_chunk * sizeof(int16_t)) continue;
        }
        if (!g_enabled) continue;
        esp_mn_state_t st = g_mn->detect(g_model, buf);
        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t* r = g_mn->get_results(g_model);
            if (r && r->num > 0) {
                log_i("wake: command %d '%s' p=%.2f", r->command_id[0], r->string ? r->string : "", r->prob[0]);
                if (r->command_id[0] >= 1) g_detected = true;
            }
            g_mn->clean(g_model);
        } else if (st == ESP_MN_STATE_TIMEOUT) {
            g_mn->clean(g_model);   // no command in the window; keep listening
        }
    }
}

}  // namespace

namespace wakeword {

bool begin() {
    g_models = esp_srmodel_init("model");
    if (!g_models || g_models->num == 0) {
        log_w("wake: no models in the 'model' partition; run `pio run -t upload_models`");
        return false;
    }
    char* name = esp_srmodel_filter(g_models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (!name) { log_w("wake: no English MultiNet model"); return false; }
    g_mn = esp_mn_handle_from_name(name);
    if (!g_mn) { log_w("wake: no handle for %s", name); return false; }
    g_model = g_mn->create(name, 6000);
    if (!g_model) { log_w("wake: model create failed"); return false; }
    esp_mn_commands_alloc((esp_mn_iface_t*)g_mn, g_model);
    esp_mn_commands_clear();
    for (auto& c : kCommands) esp_mn_commands_add(c.id, c.text);
    esp_mn_error_t* err = esp_mn_commands_update();
    if (err && err->num > 0) log_w("wake: %d command phrases rejected", err->num);
    g_mn->print_active_speech_commands(g_model);
    g_chunk = g_mn->get_samp_chunksize(g_model);
    g_stream = xStreamBufferCreate(AUDIO_RATE * 2 * sizeof(int16_t), 1);   // 2 s of slack
    xTaskCreatePinnedToCore(detectTask, "wake", 6144, nullptr, 4, nullptr, 1);
    g_ok = true;
    g_enabled = true;
    log_i("wake: MultiNet '%s' ready, chunk %d samples", name, g_chunk);
    return true;
}

bool available() { return g_ok; }

void feed(const int16_t* samples, size_t n) {
    if (!g_ok || !g_enabled) return;
    xStreamBufferSend(g_stream, samples, n * sizeof(int16_t), 0);   // drop if the detector is behind
}

bool wasDetected() { return g_detected.exchange(false); }

void setEnabled(bool on) {
    g_enabled = on;
    if (!on && g_stream) xStreamBufferReset(g_stream);
}

}  // namespace wakeword
