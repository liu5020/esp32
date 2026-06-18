# Voice Sketch Mini Program

This is the first WeChat mini program scaffold for the voice-to-sketch printer.
It is intentionally a configuration console, not the final polished product UI.

## Framework Choice

The mini program uses native WeChat Mini Program pages first, with a structure
that can adopt TDesign Miniprogram components later. This keeps BLE and LAN
HTTP calls close to the native `wx.*` APIs while avoiding extra build steps in
the early hardware phase.

## Pages

- Device: scan and connect to the ESP32 over BLE.
- Network: save WiFi and backend URL, then send them to the device.
- AI Mode: choose the backend mode and provider shape.
- Advanced: keep custom endpoints and local tuning values.
- Test: call backend health and draw endpoints from the phone.

## Open In WeChat DevTools

1. Open WeChat DevTools.
2. Import this `miniapp` folder.
3. Use a test AppID or your real mini program AppID.
4. In local network tests, make sure the phone and Orange Pi are on the same
   WiFi/LAN.
5. If HTTP LAN requests are blocked in preview, enable the dev setting that
   skips request domain validation while testing.

## Optional TDesign Setup

`package.json` already declares `tdesign-miniprogram`. When the UI becomes
stable enough to polish:

```powershell
npm install
```

Then open WeChat DevTools and run "Tools -> Build npm". The current scaffold
does not require TDesign components to run.

## Current Limits

- ESP32 BLE config firmware is implemented, but it still needs real-phone
  testing through WeChat DevTools preview or a development build.
- Backend `/config` and `/latest` endpoints are planned but not required for
  the current test page.
- API keys should stay on the Orange Pi backend. Do not store them in the mini
  program.
