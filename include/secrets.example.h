#pragma once

// Copy this file to include/secrets.h and fill in your local fallback values.
// The firmware can now receive WiFi/backend settings over BLE and save them to
// NVS. These values are used only before BLE provisioning, or after clearing the
// saved BLE config with the serial command `c`.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// The local bridge script in tools/stt_bridge_openai.py listens on /stt.
// Replace 192.168.1.100 with your computer's LAN IP address.
// BLE provisioning stores the backend base URL, for example
// http://192.168.31.58:8787, and the firmware appends /stt automatically.
#define STT_SERVER_URL "http://192.168.1.100:8787/stt"
