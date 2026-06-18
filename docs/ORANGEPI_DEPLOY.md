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
| Public Health URL | `https://api.jpxh.top/health` |
| Sketch demo URL | `http://192.168.31.58:8787/draw?text=cat` |
| Print preview URL | `http://192.168.31.58:8787/print?text=cat` |
| ESP32 STT URL | `http://192.168.31.58:8787/stt` |

The server stores uploaded WAV files in:

```text
~/voice_ai_mic_server/tools/recordings
```

The server stores generated PBM sketch previews in:

```text
~/voice_ai_mic_server/tools/sketches
```

The latest generated images are:

```text
~/voice_ai_mic_server/tools/sketches/latest_source.png
~/voice_ai_mic_server/tools/sketches/latest_preview.pbm
~/voice_ai_mic_server/tools/sketches/latest_print.pbm
~/voice_ai_mic_server/tools/sketches/latest.pbm
```

`latest_source.png` is the latest AI-generated source image, when an image API
is used. `latest_preview.pbm` is the 160x160 screen preview.
`latest_print.pbm` is the 384x384 thermal-printer preview.

## Files Deployed

Only these files are needed for the current bridge:

```text
tools/stt_bridge_openai.py
tools/stt_config.json
tools/orangepi_bridge.sh
tools/requirements.txt
```

`tools/stt_config.json` contains the API key and must stay out of Git.

Install the Python dependency used to convert AI images into one-bit thermal
printer bitmaps:

```bash
cd ~/voice_ai_mic_server
python3 -m pip install --user -r tools/requirements.txt
```

## Image Generation Config

The bridge uses Alibaba Cloud DashScope Paraformer for speech-to-text and
Alibaba Cloud Model Studio / DashScope text-to-image for sketch generation. Add
these fields to the ignored `tools/stt_config.json` on the Orange Pi:

```json
{
  "stt_provider": "local_sherpa_onnx",
  "stt_fallback_provider": "dashscope_paraformer",
  "stt_api_key": "",
  "stt_model": "paraformer-realtime-v2",
  "stt_format": "wav",
  "stt_sample_rate": 16000,
  "local_stt_model_dir": "tools/models/sherpa-onnx-paraformer-zh-small-2024-03-09",
  "local_stt_model_file": "model.int8.onnx",
  "local_stt_tokens_file": "tokens.txt",
  "local_stt_num_threads": 4,
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
  "image_fallback_local": true,
  "public_access_token": "generate_a_long_random_token_for_public_use"
}
```

The bridge uses local sherpa-onnx speech-to-text first, then falls back to
Alibaba Cloud DashScope Paraformer if local recognition fails. If `stt_api_key`
is empty, the fallback STT path reuses `image_api_key`, so one DashScope API key
is enough for both cloud fallback and image generation. If the image API fails,
`image_fallback_local: true` keeps the old local sketch generator working.

Download the local STT model on the Orange Pi:

```bash
cd ~/voice_ai_mic_server
mkdir -p tools/models
cd tools/models
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-paraformer-zh-small-2024-03-09.tar.bz2
tar xjf sherpa-onnx-paraformer-zh-small-2024-03-09.tar.bz2
```

This model is about 80 MB after extraction. It is fast enough for local testing,
but can misrecognize similar words; set `stt_provider` to
`dashscope_paraformer` if cloud accuracy is preferred.

The default image model is `wan2.2-t2i-flash` for lower-cost testing.
`image_call_mode: auto` uses synchronous calls for `wan2.6-t2i` and asynchronous
task polling for other WanX image models. `image_payload_format: auto` switches
between the newer messages payload and the older prompt payload.

You do not need to edit JSON inside SSH. Edit the ignored local config on
Windows, then upload and restart:

```powershell
cd D:\Work\esp32\project\voice_ai_mic
powershell -ExecutionPolicy Bypass -File .\tools\prepare_local_config.ps1
notepad .\tools\stt_config.json
powershell -ExecutionPolicy Bypass -File .\tools\sync_orangepi_config.ps1
```

`prepare_local_config.ps1` keeps existing keys and only adds missing fields from
the example config. `sync_orangepi_config.ps1` copies the config to the Orange
Pi and restarts the bridge.

## Public HTTPS Endpoint

`https://api.jpxh.top/health` is reachable from the public internet and returns
bridge health, so the domain is already suitable as an external entry point. Do
not leave the drawing and STT endpoints open on a public domain. Set
`public_access_token` in the ignored `tools/stt_config.json` before using:

```json
{
  "public_access_token": "a_long_random_value"
}
```

When this token is set:

- `GET /health` remains public for monitoring.
- `POST /stt`, `GET /draw`, and `GET /print` require either
  `X-VoiceSketch-Token: <token>` or `Authorization: Bearer <token>`.
- ESP32 and the mini program should use backend base URL
  `https://api.jpxh.top`, not `/stt`.

If you use Cloudflare or another reverse proxy, keep Python bound behind the
proxy when possible and avoid exposing raw port `8787` directly to the internet.

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
Invoke-WebRequest -UseBasicParsing -Uri "http://192.168.31.58:8787/print?text=cat"
Invoke-WebRequest -UseBasicParsing -Uri "https://api.jpxh.top/health"
```

From the Orange Pi:

```bash
curl http://127.0.0.1:8787/health
curl "http://127.0.0.1:8787/draw?text=cat"
curl "http://127.0.0.1:8787/print?text=cat"
```

## Next Server Step

The backend now creates a thermal-printer preview target:

```text
recognized text -> 160x160 screen bitmap + 384-dot-wide printer bitmap
```

The current backend can use Alibaba Cloud Model Studio / DashScope for AI
line-art generation, then converts the generated PNG into clean one-bit bitmaps.
Next, tune the prompt and `image_threshold` using real printed samples.
