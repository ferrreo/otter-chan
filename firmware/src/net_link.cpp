#include "net_link.h"
#include "config.h"
#include "settings.h"
#include "state.h"
#include "certs.h"
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace {

WebSocketsClient g_ws;
net::JsonHandler g_onJson;
net::BinHandler g_onBin;
std::atomic<bool> g_connected{false};
std::atomic<bool> g_forceReconnect{false};
QueueHandle_t g_txQueue;   // items: TxItem*
SemaphoreHandle_t g_wsMutex;

struct TxItem {
    bool binary;
    size_t len;
    uint8_t data[];   // flexible
};

bool parseUrl(const String& url, bool& ssl, String& host, uint16_t& port, String& path) {
    String u = url;
    if (u.startsWith("wss://")) { ssl = true; u = u.substring(6); }
    else if (u.startsWith("ws://")) { ssl = false; u = u.substring(5); }
    else return false;
    int slash = u.indexOf('/');
    String hp = slash >= 0 ? u.substring(0, slash) : u;
    path = slash >= 0 ? u.substring(slash) : "/";
    int colon = hp.indexOf(':');
    host = colon >= 0 ? hp.substring(0, colon) : hp;
    port = colon >= 0 ? hp.substring(colon + 1).toInt() : (ssl ? 443 : 80);
    return host.length() > 0;
}

void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED: {
            g_connected = true;
            g_state.serverOk = true;
            log_i("ws connected");
            JsonDocument d;
            d["type"] = "hello";
            d["name"] = g_settings.deviceName;
            d["fw"] = OTTER_FW_VERSION;
            d["rate"] = AUDIO_RATE;
            d["mode"] = modeName(g_state.mode.load());
            d["wake_word"] = g_settings.wakeWord;
            d["tracking"] = g_settings.tracking;
            String s; serializeJson(d, s);
            g_ws.sendTXT(s);
            break;
        }
        case WStype_DISCONNECTED:
            if (g_connected) log_w("ws disconnected");
            g_connected = false;
            g_state.serverOk = false;
            break;
        case WStype_TEXT: {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, payload, length);
            if (err) { log_w("bad json: %s", err.c_str()); break; }
            if (g_onJson) g_onJson(doc);
            break;
        }
        case WStype_BIN:
            if (length >= 1 && g_onBin) g_onBin(payload[0], payload + 1, length - 1);
            break;
        case WStype_ERROR:
            log_w("ws error");
            break;
        default: break;
    }
}

void connectWs() {
    bool ssl; String host, path; uint16_t port;
    if (!parseUrl(g_settings.serverUrl, ssl, host, port, path)) {
        log_e("bad server url: %s", g_settings.serverUrl.c_str());
        return;
    }
    String auth = "Authorization: Bearer " + g_settings.deviceToken + "\r\nX-Otter-Name: " + g_settings.deviceName;
    g_ws.setExtraHeaders(auth.c_str());
    g_ws.setReconnectInterval(WS_RECONNECT_MS);
    g_ws.enableHeartbeat(15000, 4000, 2);
    g_ws.onEvent(wsEvent);
    if (ssl) g_ws.beginSslWithCA(host.c_str(), port, path.c_str(), ISRG_ROOT_X1);
    else g_ws.begin(host.c_str(), port, path.c_str());
    log_i("ws -> %s:%u%s", host.c_str(), port, path.c_str());
}

void netTask(void*) {
    bool started = false;
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            g_state.wifiOk = false;
            if (started) { g_ws.disconnect(); started = false; g_connected = false; g_state.serverOk = false; }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        g_state.wifiOk = true;
        g_state.rssi = WiFi.RSSI();
        if (g_forceReconnect.exchange(false) && started) { g_ws.disconnect(); started = false; }
        if (!started) { connectWs(); started = true; }
        g_ws.loop();
        // drain tx queue
        TxItem* it;
        int budget = 8;
        while (budget-- && xQueueReceive(g_txQueue, &it, 0) == pdTRUE) {
            if (g_connected) {
                if (it->binary) g_ws.sendBIN(it->data, it->len);
                else g_ws.sendTXT(it->data, it->len);
            }
            free(it);
            g_ws.loop();
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

bool enqueue(bool binary, const uint8_t* a, size_t alen, const uint8_t* b, size_t blen, bool dropIfFull) {
    if (!g_connected) return false;
    if (dropIfFull && uxQueueSpacesAvailable(g_txQueue) < 2) return false;
    TxItem* it = (TxItem*)heap_caps_malloc(sizeof(TxItem) + alen + blen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!it) return false;
    it->binary = binary; it->len = alen + blen;
    if (alen) memcpy(it->data, a, alen);
    if (blen) memcpy(it->data + alen, b, blen);
    if (xQueueSend(g_txQueue, &it, dropIfFull ? 0 : pdMS_TO_TICKS(50)) != pdTRUE) { free(it); return false; }
    return true;
}

}  // namespace

namespace net {

void begin(JsonHandler onJson, BinHandler onBin) {
    g_onJson = onJson; g_onBin = onBin;
    g_txQueue = xQueueCreate(64, sizeof(TxItem*));
    g_wsMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 4, nullptr, 0);
}

bool connected() { return g_connected; }

void sendJson(const JsonDocument& doc) {
    String s; serializeJson(doc, s);
    enqueue(false, (const uint8_t*)s.c_str(), s.length(), nullptr, 0, false);
}

void sendJson(const char* type) {
    JsonDocument d; d["type"] = type; sendJson(d);
}

bool sendBin(uint8_t tag, const uint8_t* data, size_t len, bool dropIfFull) {
    return enqueue(true, &tag, 1, data, len, dropIfFull);
}

void reconnect() { g_forceReconnect = true; }

}  // namespace net
