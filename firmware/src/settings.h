// Persistent settings (NVS)
#pragma once
#include <Arduino.h>

struct Settings {
    String wifiSsid;
    String wifiPass;
    String serverUrl;      // ws://host:port/ws/device or wss://
    String deviceToken;
    String deviceName;
    uint8_t volume     = 160;
    bool    tracking   = true;
    bool    wakeWord   = true;
    uint8_t brightness = 128;

    void load();
    void save() const;
    bool hasWifi() const { return wifiSsid.length() > 0; }
    bool hasServer() const { return serverUrl.length() > 0 && deviceToken.length() > 0; }
};

extern Settings g_settings;
