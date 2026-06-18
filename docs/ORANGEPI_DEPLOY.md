# Orange Pi Backend Deployment

This project can run the local Python bridge on an Orange Pi so the ESP32 does
not depend on the Windows development computer.

## Current LAN Deployment

| Item | Value |
| --- | --- |
| Host | `192.168.31.58` |
| User | `HwHiAiUser` |
| App directory | `~/voice_ai_mic_server` |
| Port | `8787` |
| Health URL | `http://192.168.31.58:8787/health` |
| Sketch demo URL | `http://192.168.31.58:8787/draw?text=cat` |
| ESP32 STT URL | `http://192.168.31.58:8787/stt` |

The server stores uploaded WAV files in:

```text
~/voice_ai_mic_server/tools/recordings
```

The server stores generated PBM sketch previews in:

```text
~/voice_ai_mic_server/tools/sketches
```

## Files Deployed

Only these files are needed for the current bridge:

```text
tools/stt_bridge_openai.py
tools/stt_config.json
tools/orangepi_bridge.sh
```

`tools/stt_config.json` contains the API key and must stay out of Git.

## Manage The Service

SSH into the Orange Pi:

```powershell
ssh HwHiAiUser@192.168.31.58
```

Then use:

```bash
cd ~/voice_ai_mic_server
./tools/orangepi_bridge.sh status
./tools/orangepi_bridge.sh start
./tools/orangepi_bridge.sh stop
./tools/orangepi_bridge.sh restart
./tools/orangepi_bridge.sh logs
```

The current service is started with `nohup`, so it survives SSH logout but does
not automatically start after reboot. A systemd user service can be added later
when the backend API stabilizes.

## ESP32 Configuration

The ignored local firmware config should point to the Orange Pi:

```cpp
#define STT_SERVER_URL "http://192.168.31.58:8787/stt"
```

After changing `include/secrets.h`, rebuild and upload the ESP32 firmware.

## Quick Checks

From Windows:

```powershell
Invoke-WebRequest -UseBasicParsing -Uri "http://192.168.31.58:8787/health"
Invoke-WebRequest -UseBasicParsing -Uri "http://192.168.31.58:8787/draw?text=cat"
```

From the Orange Pi:

```bash
curl http://127.0.0.1:8787/health
curl "http://127.0.0.1:8787/draw?text=cat"
```

## Next Server Step

The next backend feature should be a thermal-printer preview target:

```text
recognized text -> 160x160 screen bitmap + 384-dot-wide printer bitmap
```

The 384-dot bitmap can be saved as `tools/sketches/latest_print.pbm` before the
printer arrives.
