#include "provisioning.h"
#include "config.h"
#include "settings.h"
#include "state.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <M5Unified.h>

namespace {

const char* kPage = R"HTML(<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Otter-chan setup</title><style>body{font-family:system-ui;margin:2em;max-width:26em}label{display:block;margin-top:1em}
input{width:100%;padding:.5em;font-size:1em}button{margin-top:1.5em;padding:.7em 1.4em;font-size:1em}</style></head><body>
<h2>🦦 Otter-chan setup</h2><form method=post action=/save>
<label>WiFi SSID<input name=ssid value="%SSID%"></label>
<label>WiFi password<input name=pass type=password value="%PASS%"></label>
<label>Server URL (ws://host:port/ws/device)<input name=url value="%URL%"></label>
<label>Device token<input name=token value="%TOKEN%"></label>
<label>Robot name<input name=name value="%NAME%"></label>
<button>Save &amp; reboot</button></form></body></html>)HTML";

String render() {
    String s = kPage;
    s.replace("%SSID%", g_settings.wifiSsid);
    s.replace("%PASS%", g_settings.wifiPass);
    s.replace("%URL%", g_settings.serverUrl);
    s.replace("%TOKEN%", g_settings.deviceToken);
    s.replace("%NAME%", g_settings.deviceName);
    return s;
}

bool applyJson(JsonDocument& d) {
    bool any = false;
    if (d["ssid"].is<const char*>())  { g_settings.wifiSsid = d["ssid"].as<String>(); any = true; }
    if (d["pass"].is<const char*>())  { g_settings.wifiPass = d["pass"].as<String>(); any = true; }
    if (d["url"].is<const char*>())   { g_settings.serverUrl = d["url"].as<String>(); any = true; }
    if (d["token"].is<const char*>()) { g_settings.deviceToken = d["token"].as<String>(); any = true; }
    if (d["name"].is<const char*>())  { g_settings.deviceName = d["name"].as<String>(); any = true; }
    if (d["volume"].is<int>())        { g_settings.volume = d["volume"].as<int>(); any = true; }
    if (d["tracking"].is<bool>())     { g_settings.tracking = d["tracking"].as<bool>(); any = true; }
    if (d["wake_word"].is<bool>())    { g_settings.wakeWord = d["wake_word"].as<bool>(); any = true; }
    if (any) g_settings.save();
    return any;
}

}  // namespace

namespace provisioning {

String apName() {
    uint64_t mac = ESP.getEfuseMac();
    char buf[32];
    snprintf(buf, sizeof(buf), "OtterChan-%04X", (unsigned)(mac & 0xFFFF));
    return buf;
}

void startPortal() {
    g_state.mode = Mode::Provision;
    String ap = apName();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap.c_str());
    IPAddress ip = WiFi.softAPIP();
    DNSServer dns; dns.start(53, "*", ip);
    WebServer http(80);
    bool saved = false;
    http.on("/", [&]() { http.send(200, "text/html", render()); });
    http.on("/save", HTTP_POST, [&]() {
        g_settings.wifiSsid = http.arg("ssid");
        g_settings.wifiPass = http.arg("pass");
        g_settings.serverUrl = http.arg("url");
        g_settings.deviceToken = http.arg("token");
        if (http.arg("name").length()) g_settings.deviceName = http.arg("name");
        g_settings.save();
        http.send(200, "text/html", "<h2>Saved. Rebooting…</h2>");
        saved = true;
    });
    http.onNotFound([&]() { http.sendHeader("Location", "http://" + ip.toString() + "/", true); http.send(302, "text/plain", ""); });
    http.begin();

    auto& d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setTextDatum(top_left);
    d.setTextSize(2);
    d.setCursor(8, 10);  d.print("Setup mode");
    d.setTextSize(1);
    d.setCursor(8, 40);  d.printf("1. Join WiFi: %s", ap.c_str());
    d.setCursor(8, 56);  d.printf("2. Open http://%s/", ip.toString().c_str());
    d.setCursor(8, 72);  d.print("   (or send JSON over USB serial)");
    d.setCursor(8, 100); d.print("Fields: WiFi, server ws:// URL, token");
    d.setCursor(8, 200); d.print("Reboots automatically after save.");

    uint32_t start = millis();
    while (!saved && millis() - start < PROVISION_PORTAL_MS) {
        dns.processNextRequest();
        http.handleClient();
        pollSerial();
        if (g_settings.hasWifi() && g_settings.hasServer() && Serial.available() == 0 && !saved) {
            // pollSerial may have saved a full config
        }
        delay(5);
    }
    delay(800);
    ESP.restart();
}

void pollSerial() {
    static String line;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (line.length() > 2 && line[0] == '{') {
                JsonDocument d;
                if (!deserializeJson(d, line)) {
                    bool ok = applyJson(d);
                    Serial.printf("{\"ok\":%s}\n", ok ? "true" : "false");
                    if (ok && d["reboot"] | true) { delay(300); ESP.restart(); }
                } else Serial.println("{\"ok\":false,\"error\":\"bad json\"}");
            } else if (line == "status") {
                Serial.printf("{\"mode\":\"%s\",\"wifi\":%d,\"server\":%d,\"ip\":\"%s\",\"url\":\"%s\",\"name\":\"%s\",\"fw\":\"%s\"}\n",
                              modeName(g_state.mode.load()), (int)g_state.wifiOk, (int)g_state.serverOk,
                              WiFi.localIP().toString().c_str(), g_settings.serverUrl.c_str(),
                              g_settings.deviceName.c_str(), OTTER_FW_VERSION);
            } else if (line == "reset") {
                g_settings.wifiSsid = ""; g_settings.save(); ESP.restart();
            }
            line = "";
        } else if (line.length() < 1024) line += c;
    }
}

}  // namespace provisioning
