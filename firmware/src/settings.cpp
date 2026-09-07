#include "settings.h"
#include "config.h"
#include <Preferences.h>

Settings g_settings;
static const char* NS = "otter";

void Settings::load() {
    Preferences p;
    p.begin(NS, true);
    wifiSsid    = p.getString("ssid", OTTER_WIFI_SSID);
    wifiPass    = p.getString("pass", OTTER_WIFI_PASS);
    serverUrl   = p.getString("url", OTTER_SERVER_URL);
    deviceToken = p.getString("token", OTTER_DEVICE_TOKEN);
    deviceName  = p.getString("name", OTTER_DEVICE_NAME);
    volume      = p.getUChar("vol", DEFAULT_VOLUME);
    tracking    = p.getBool("track", true);
    wakeWord    = p.getBool("wake", true);
    brightness  = p.getUChar("bright", 128);
    p.end();
}

void Settings::save() const {
    Preferences p;
    p.begin(NS, false);
    p.putString("ssid", wifiSsid);
    p.putString("pass", wifiPass);
    p.putString("url", serverUrl);
    p.putString("token", deviceToken);
    p.putString("name", deviceName);
    p.putUChar("vol", volume);
    p.putBool("track", tracking);
    p.putBool("wake", wakeWord);
    p.putUChar("bright", brightness);
    p.end();
}
