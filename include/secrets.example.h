#pragma once

// Copy this file to include/secrets.h and fill in your local values.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// The local bridge script in tools/stt_bridge_openai.py listens on /stt.
// Replace 192.168.1.100 with your computer's LAN IP address.
#define STT_SERVER_URL "http://192.168.1.100:8787/stt"

