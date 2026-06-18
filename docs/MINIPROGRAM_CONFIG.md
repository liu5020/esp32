# Mini Program Configuration Plan

This document defines the first useful scope for the future mobile app or
WeChat mini program. The goal is not to build the full product UI yet. The goal
is to define a small configuration surface that can later be sent to the ESP32
over BLE and to the Orange Pi backend over HTTP.

## Timing

It is not too early to build the configuration framework. It is too early to
build a polished full app.

The current hardware and backend are still changing, so the first app version
should stay narrow:

- Discover and connect to the ESP32 over BLE.
- Configure WiFi credentials and backend URL.
- Configure backend AI mode.
- Show backend health and last job status.
- Avoid storing provider API keys on the phone when possible.

## Product Shape

Recommended first screens:

| Screen | Purpose |
| --- | --- |
| Device | Scan BLE, connect device, show firmware/device status. |
| Network | Send WiFi SSID/password and backend URL to the ESP32. |
| AI Mode | Choose local STT, cloud STT fallback, image provider, and print mode. |
| Advanced | Custom HTTP endpoints, custom provider/model names, thresholds. |
| Test | Trigger backend health, demo draw, latest preview, and print test later. |

## Configuration Ownership

Keep responsibilities separated:

| Config | Owner | Reason |
| --- | --- | --- |
| WiFi SSID/password | ESP32 | Device must join LAN without phone after setup. |
| Backend URL | ESP32 | Device needs to know where to upload WAV. |
| AI provider keys | Backend | Keys should not be stored on ESP32 or in the mini program. |
| STT/image model selection | Backend | Backend owns model availability and dependencies. |
| Style presets | Backend, optionally mirrored on ESP32 | Server can evolve prompts without firmware changes. |

## Device BLE Config

The first BLE protocol can be simple JSON chunks. A custom BLE service is
enough; no need to use a public standard service.

Proposed service:

```text
Service UUID:        7a8f0001-7d2a-4f2c-8f9d-0a1b2c3d4e5f
Config Write UUID:  7a8f0002-7d2a-4f2c-8f9d-0a1b2c3d4e5f
Status Notify UUID: 7a8f0003-7d2a-4f2c-8f9d-0a1b2c3d4e5f
```

The exact UUID values are less important than keeping the protocol stable.

Minimal device config payload:

```json
{
  "version": 1,
  "wifi": {
    "ssid": "Xiaomi_F165",
    "password": "..."
  },
  "backend": {
    "base_url": "https://api.jpxh.top",
    "access_token": "optional-public-backend-token"
  }
}
```

ESP32 should store only this data. The backend access token is only a gate for
your own public endpoint; it is not an AI provider API key. ESP32 should not
receive provider API keys.

## Backend Config

Backend config is currently `tools/stt_config.json` on the Orange Pi. The mini
program should eventually update it through authenticated backend endpoints,
not by editing files directly.

Current backend config shape:

```json
{
  "stt_provider": "local_sherpa_onnx",
  "stt_fallback_provider": "dashscope_paraformer",
  "stt_api_key": "",
  "stt_model": "paraformer-realtime-v2",
  "local_stt_model_dir": "tools/models/sherpa-onnx-paraformer-zh-small-2024-03-09",
  "image_provider": "aliyun_wanx",
  "image_api_key": "...",
  "image_model": "wan2.2-t2i-flash",
  "image_call_mode": "auto",
  "image_threshold": 210
}
```

Recommended future backend endpoints:

| Endpoint | Method | Purpose |
| --- | --- | --- |
| `/health` | GET | Existing health check. |
| `/config` | GET | Return safe config fields, never raw API keys. |
| `/config` | POST | Update provider/model/style settings. |
| `/config/key` | POST | Update API keys; response should never echo the key. |
| `/draw?text=...` | GET | Existing demo image generation. |
| `/stt` | POST | Existing WAV upload and sketch generation. |
| `/latest` | GET | Return latest text, provider, source image path, and preview metadata. |

For public deployments, keep `/health` public and require
`X-VoiceSketch-Token` or `Authorization: Bearer ...` for `/stt`, `/draw`, and
`/print`.

## AI Mode Options

Initial AI modes:

```json
[
  {
    "id": "local_first",
    "label": "Local STT + Cloud Image",
    "stt_provider": "local_sherpa_onnx",
    "stt_fallback_provider": "dashscope_paraformer",
    "image_provider": "aliyun_wanx"
  },
  {
    "id": "cloud_all",
    "label": "Cloud STT + Cloud Image",
    "stt_provider": "dashscope_paraformer",
    "stt_fallback_provider": "",
    "image_provider": "aliyun_wanx"
  },
  {
    "id": "custom_backend",
    "label": "Custom Backend API",
    "backend_base_url": "http://host:port"
  }
]
```

Do not make the mini program call AI provider APIs directly in the first
version. Custom provider endpoints should be configured on the backend.

## Custom API Contract

For a custom backend to be compatible with the ESP32, it should implement:

```text
POST /stt
Content-Type: audio/wav
Response: application/json
```

Minimum response:

```json
{
  "text": "draw a pig",
  "image": {
    "width": 160,
    "height": 160,
    "format": "1bpp_hex_msb_black1",
    "title": "sketch",
    "bitmap": "..."
  }
}
```

Optional fields:

```json
{
  "stt_provider": "local_sherpa_onnx",
  "image_provider": "aliyun_wanx",
  "print_image": {
    "width": 384,
    "height": 384,
    "format": "1bpp_hex_msb_black1",
    "saved": "tools/sketches/latest_print.pbm"
  }
}
```

## First Implementation Milestone

The first mini program scaffold exists under `miniapp/`, and the ESP32 firmware
now exposes the BLE provisioning service described above. The remaining work is
to connect the mini program to the backend config endpoints and test BLE writes
on a real phone.

Recommended order:

1. Test mini program BLE scan/connect/write on a real phone.
2. Add backend `/config` GET with safe fields only.
3. Add backend `/config` POST for non-secret mode/model/style fields.
4. Add backend `/config/key` POST for API keys.
5. Bind the mini program advanced/settings pages to those stable contracts.
