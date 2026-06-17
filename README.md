# ESP32-S3 INMP441 Voice Input Starter

This project reads an INMP441 I2S microphone on an ESP32-S3, shows microphone
status on the ST7789 screen, and exports short 16 kHz / 16-bit / mono WAV
recordings over Serial. It is meant as a clean base for later AI speech
recognition.

## Wiring

The microphone pins are chosen to avoid the TFT screen wiring you already used.

Screen wiring:

| ST7789 pin | ESP32-S3 pin |
| --- | --- |
| `VCC` | `3V3` |
| `GND` | `GND` |
| `SCL` / `SCLK` | `GPIO18` |
| `SDA` / `MOSI` | `GPIO16` |
| `CS` | `GPIO5` |
| `DC` | `GPIO17` |
| `RES` / `RST` | `GPIO4` |
| `BL` / `BLK` | `GPIO2` |

Microphone wiring:

| INMP441 pin | ESP32-S3 pin |
| --- | --- |
| `VCC` / `VDD` | `3V3` |
| `GND` | `GND` |
| `SCK` | `GPIO10` |
| `WS` | `GPIO11` |
| `SD` | `GPIO12` |
| `L/R` | `GND` |

Notes:

- Use `3V3`, not `5V`.
- `L/R -> GND` makes the mic output on the left I2S channel.
- If you wire `L/R -> 3V3`, change `MIC_CHANNEL_FORMAT` in `src/main.cpp` from
  `I2S_CHANNEL_FMT_ONLY_LEFT` to `I2S_CHANNEL_FMT_ONLY_RIGHT`.
- After flashing, the screen should show `VOICE MIC`, `mic ready`, and a live
  level bar. If the screen is blank, first check whether the firmware actually
  uploaded, then check the screen `BL` pin.

## Serial Commands

Open the serial monitor at `921600` baud.

| Command | Action |
| --- | --- |
| `h` | Show help |
| `v` | Toggle live volume meter |
| `w` | Test WiFi and update the screen |
| `r` | Record 5 seconds and print a WAV file as Base64 |
| `s` | Record 5 seconds and send it to the speech-to-text server |

The `r` command prints:

```text
-----BEGIN WAV BASE64-----
...
-----END WAV BASE64-----
```

Copy the Base64 body and decode it to a `.wav` file. Most AI speech-to-text
APIs can accept this WAV format directly, or you can strip the 44-byte WAV
header and send raw `audio/L16;rate=16000` PCM.

## Build And Upload

```powershell
pio run -d D:\Work\esp32\project\voice_ai_mic -t upload
pio device monitor -d D:\Work\esp32\project\voice_ai_mic -b 921600
```

If the live volume stays near zero, first check `L/R` and `GND`, then try
switching the channel format as described above.

## WiFi Test Screen

On boot, the firmware tries to connect to WiFi once and keeps the result on the
screen:

- `NO CONFIG`: edit `include/secrets.h`.
- `WIFI OK`: connected. The screen shows IP, RSSI, and a four-bar signal icon.
- `FAILED`: SSID/password, signal, or router compatibility problem.

Send `w` in the serial monitor to test WiFi again.

ESP32-S3 connects to 2.4 GHz WiFi. A router that uses the same SSID for 2.4 GHz
and 5 GHz is usually fine as long as the 2.4 GHz radio is enabled.

## Speech To Text

The firmware sends WAV audio to a local HTTP bridge instead of putting an AI API
key on the ESP32.

1. Copy `include/secrets.example.h` to `include/secrets.h`, then edit
   `include/secrets.h`.

   ```cpp
   #define WIFI_SSID "your wifi"
   #define WIFI_PASSWORD "your password"
   #define STT_SERVER_URL "http://your-computer-lan-ip:8787/stt"
   ```

2. Copy `tools/stt_config.example.json` to `tools/stt_config.json`, then edit
   `tools/stt_config.json`.

   ```json
   {
     "api_key": "your_api_key",
     "transcriptions_url": "https://api.groq.com/openai/v1/audio/transcriptions",
     "model": "whisper-large-v3-turbo",
     "language": "zh"
   }
   ```

   This default uses Groq's OpenAI-compatible Whisper endpoint. For OpenAI direct
   use, set:

   ```json
   {
     "api_key": "your_openai_api_key",
     "transcriptions_url": "https://api.openai.com/v1/audio/transcriptions",
     "model": "gpt-4o-mini-transcribe",
     "language": "zh"
   }
   ```

   The relay must support multipart `POST /v1/audio/transcriptions`. A text-only
   relay for `/v1/chat/completions` or `/v1/responses` will not work for speech
   to text.

3. Run the bridge on your computer.

   ```powershell
   python D:\Work\esp32\project\voice_ai_mic\tools\stt_bridge_openai.py
   ```

4. Rebuild and upload the ESP32 firmware.

5. Open serial monitor at `921600`, send `s`, and speak for 5 seconds.

The screen will show `speak now`, then `thinking`, then `stt ok` if the bridge
returns recognized text. The transcript is printed in the serial monitor.

If the ESP32 prints `Could not connect to STT server`, check:

- The bridge script is running and says `STT bridge listening`.
- `http://127.0.0.1:8787/health` opens on the computer.
- `STT_SERVER_URL` in `include/secrets.h` uses the computer's LAN IP, not
  `127.0.0.1`.
- Windows Firewall allows inbound connections to Python on private networks, or
  allows TCP port `8787`.
- The ESP32 and computer are on the same LAN, with guest/AP isolation disabled.
