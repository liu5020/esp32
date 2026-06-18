# ESP32-S3 INMP441 Voice Input Starter

This project reads an INMP441 I2S microphone on an ESP32-S3, shows microphone
status on the ST7789 screen, and exports short 16 kHz / 16-bit / mono WAV
recordings over Serial. It is meant as a clean base for later AI speech
recognition.

Project architecture and graduation-design roadmap are recorded in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

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

Button wiring:

| Button pin | ESP32-S3 pin |
| --- | --- |
| one side | `GPIO13` |
| other side | `GND` |

Speaker wiring, for a MAX98357A I2S amplifier module:

| MAX98357A pin | ESP32-S3 pin |
| --- | --- |
| `VIN` / `VCC` | `3V3` or `5V`, depending on your module |
| `GND` | `GND` |
| `BCLK` / `BCK` | `GPIO14` |
| `LRC` / `LRCLK` / `WS` | `GPIO15` |
| `DIN` | `GPIO21` |
| speaker `+/-` | connect to the amplifier module output |

Notes:

- Use `3V3`, not `5V`, for the INMP441 microphone.
- Power the MAX98357A module according to its board label/spec. Many boards
  accept `3V3` or `5V`.
- `L/R -> GND` makes the mic output on the left I2S channel.
- If you wire `L/R -> 3V3`, change `MIC_CHANNEL_FORMAT` in `src/main.cpp` from
  `I2S_CHANNEL_FMT_ONLY_LEFT` to `I2S_CHANNEL_FMT_ONLY_RIGHT`.
- Do not connect a bare speaker directly to ESP32 GPIO pins. The 8 ohm / 2 W
  speaker shown in the project notes needs an amplifier module such as
  MAX98357A, PAM8403, or NS4168. Speaker beeps are disabled in code until an
  amplifier module is connected.
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
| `b` | Test the speaker beep |
| `d` | Fetch a demo sketch from the bridge and show it on the screen |
| `r` | Record 5 seconds and print a WAV file as Base64 |
| `s` | Record 5 seconds and send it to the speech-to-text server |

Pressing the GPIO13 button also records 5 seconds and sends it to the
speech-to-text server.

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

   If `tools/stt_config.json` already exists, run this helper to add any new
   missing fields without replacing your existing STT key:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\tools\prepare_local_config.ps1
   ```

   ```json
   {
     "api_key": "your_api_key",
     "transcriptions_url": "https://api.groq.com/openai/v1/audio/transcriptions",
     "model": "whisper-large-v3-turbo",
     "language": "zh",
     "image_provider": "aliyun_wanx",
     "image_api_key": "your_dashscope_api_key",
     "image_generation_url": "https://dashscope.aliyuncs.com/api/v1/services/aigc/text2image/image-synthesis",
     "image_task_url_template": "https://dashscope.aliyuncs.com/api/v1/tasks/{task_id}",
     "image_model": "wan2.2-t2i-flash",
     "image_call_mode": "auto",
     "image_payload_format": "auto",
     "image_size": "1024*1024",
     "image_prompt_extend": true,
     "image_watermark": false,
     "image_task_poll_seconds": 2,
     "image_task_timeout_seconds": 180,
     "image_threshold": 210,
     "image_fallback_local": true
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

   To edit the config locally and sync it to the Orange Pi without using SSH
   editors:

   ```powershell
   notepad .\tools\stt_config.json
   powershell -ExecutionPolicy Bypass -File .\tools\sync_orangepi_config.ps1
   ```

3. Install the bridge dependencies.

   ```powershell
   python -m pip install -r D:\Work\esp32\project\voice_ai_mic\tools\requirements.txt
   ```

4. Run the bridge on your computer.

   ```powershell
   python D:\Work\esp32\project\voice_ai_mic\tools\stt_bridge_openai.py
   ```

5. Rebuild and upload the ESP32 firmware.

6. Open serial monitor at `921600`, send `s`, and speak for 5 seconds.

The screen will show `speak now`, then `thinking`, then `stt ok` if the bridge
returns recognized text. The transcript is printed in the serial monitor. The
bridge also returns a 160x160 one-bit sketch preview. When the preview is
present, the ESP32 keeps it on the screen until the next recording or command.

The bridge saves every uploaded WAV under `tools/recordings/` and also updates
`tools/recordings/latest.wav`. Listen to `latest.wav` first when transcription
quality is poor. If the WAV sounds clean but transcription is wrong, tune the
STT provider/model. If the WAV is too quiet, noisy, clipped, or full of clicks,
fix the microphone wiring/gain/conversion first.

The sketch preview is intentionally black-and-white because the same bitmap
format can later be sent to a thermal printer. The bridge saves generated PBM
files under `tools/sketches/` and updates:

- `tools/sketches/latest_source.png`: latest AI source image, when an image API
  is used.
- `tools/sketches/latest_preview.pbm`: 160x160 screen preview.
- `tools/sketches/latest_print.pbm`: 384x384 thermal-printer preview.
- `tools/sketches/latest.pbm`: compatibility alias for the screen preview.

Send `d` in the serial monitor to test the screen drawing path without
recording audio. The bridge first tries the configured image provider. With
`image_provider` set to `aliyun_wanx`, it calls Alibaba Cloud Model Studio /
DashScope text-to-image, downloads the generated PNG, and converts it to one-bit
screen and printer bitmaps. The default `image_model` is `wan2.2-t2i-flash`
because it is intended as a lower-cost fast image model. `image_call_mode:
auto` uses synchronous calls for `wan2.6-t2i` and asynchronous task polling for
other WanX image models; `image_payload_format: auto` switches between the
newer messages payload and the older prompt payload. If the image API is not
configured or fails and
`image_fallback_local` is true, the bridge falls back to the local rule-based
placeholder for common words such as cat, dog, house, tree, flower, car, fish,
person, mountain, and star.

If the ESP32 prints `Could not connect to STT server`, check:

- The bridge script is running and says `STT bridge listening`.
- `http://127.0.0.1:8787/health` opens on the computer.
- `STT_SERVER_URL` in `include/secrets.h` uses the computer's LAN IP, not
  `127.0.0.1`.
- Windows Firewall allows inbound connections to Python on private networks, or
  allows TCP port `8787`.
- The ESP32 and computer are on the same LAN, with guest/AP isolation disabled.
