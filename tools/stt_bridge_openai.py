#!/usr/bin/env python3
"""
Local speech-to-text bridge for the ESP32 voice_ai_mic project.

The ESP32 posts audio/wav to http://<computer-ip>:8787/stt.
This bridge forwards that WAV to an OpenAI-compatible audio transcription API
and returns:

    {"text": "..."}

You can configure it with tools/stt_config.json or environment variables.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import sys
from datetime import datetime
import uuid
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


DEFAULT_TRANSCRIPTIONS_URL = "https://api.openai.com/v1/audio/transcriptions"
DEFAULT_CONFIG_PATH = Path(__file__).with_name("stt_config.json")
DEFAULT_RECORDINGS_DIR = Path(__file__).with_name("recordings")
CONFIG: dict[str, str] = {}
RECORDINGS_DIR = DEFAULT_RECORDINGS_DIR
MAX_UPLOAD_BYTES = 2 * 1024 * 1024


def load_config(path: Path) -> dict[str, str]:
    config = {
        "api_key": "",
        "transcriptions_url": DEFAULT_TRANSCRIPTIONS_URL,
        "model": "gpt-4o-mini-transcribe",
        "language": "zh",
    }

    if path.exists():
        with path.open("r", encoding="utf-8") as f:
            file_config = json.load(f)
        for key in config:
            value = file_config.get(key)
            if isinstance(value, str) and value:
                config[key] = value

    # Environment variables still work and override the file when present.
    config["api_key"] = os.environ.get("STT_API_KEY") or os.environ.get("OPENAI_API_KEY") or config["api_key"]
    config["transcriptions_url"] = os.environ.get("STT_TRANSCRIPTIONS_URL", config["transcriptions_url"])
    config["model"] = os.environ.get("STT_MODEL") or os.environ.get("OPENAI_TRANSCRIBE_MODEL", config["model"])
    config["language"] = os.environ.get("STT_LANGUAGE") or os.environ.get("OPENAI_TRANSCRIBE_LANGUAGE", config["language"])
    return config


def mask_secret(value: str) -> str:
    if not value or value == "PUT_YOUR_API_KEY_HERE":
        return "(not set)"
    if len(value) <= 10:
        return f"{value[:3]}... len={len(value)}"
    return f"{value[:6]}...{value[-4:]} len={len(value)}"


def print_config_warnings() -> None:
    api_key = CONFIG.get("api_key", "")
    endpoint = CONFIG.get("transcriptions_url", "")

    if "api.groq.com" in endpoint and not api_key.startswith("gsk_"):
        print("WARNING: Groq endpoint is selected, but api_key does not start with gsk_.", flush=True)

    if "..." in api_key:
        print("WARNING: api_key contains '...'. The dashboard masked key cannot be used.", flush=True)


def save_recording(wav_bytes: bytes, client_ip: str) -> Path:
    RECORDINGS_DIR.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    safe_ip = client_ip.replace(":", "_").replace(".", "-")
    path = RECORDINGS_DIR / f"{timestamp}_{safe_ip}.wav"
    path.write_bytes(wav_bytes)

    latest_path = RECORDINGS_DIR / "latest.wav"
    try:
        shutil.copyfile(path, latest_path)
    except OSError:
        pass

    return path


def multipart_form_data(fields: dict[str, str], file_field: str, filename: str, content_type: str, data: bytes) -> tuple[bytes, str]:
    boundary = f"----esp32voice{uuid.uuid4().hex}"
    chunks: list[bytes] = []

    for name, value in fields.items():
        chunks.append(f"--{boundary}\r\n".encode("ascii"))
        chunks.append(f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode("ascii"))
        chunks.append(value.encode("utf-8"))
        chunks.append(b"\r\n")

    chunks.append(f"--{boundary}\r\n".encode("ascii"))
    chunks.append(
        f'Content-Disposition: form-data; name="{file_field}"; filename="{filename}"\r\n'.encode("ascii")
    )
    chunks.append(f"Content-Type: {content_type}\r\n\r\n".encode("ascii"))
    chunks.append(data)
    chunks.append(b"\r\n")
    chunks.append(f"--{boundary}--\r\n".encode("ascii"))

    return b"".join(chunks), f"multipart/form-data; boundary={boundary}"


def transcribe_with_openai(wav_bytes: bytes) -> str:
    api_key = CONFIG.get("api_key", "")
    if not api_key or api_key == "PUT_YOUR_API_KEY_HERE":
        raise RuntimeError(f"api_key is not set. Edit {DEFAULT_CONFIG_PATH}")

    fields = {
        "model": CONFIG.get("model", "gpt-4o-mini-transcribe"),
        "response_format": "json",
    }
    language = CONFIG.get("language", "")
    if language:
        fields["language"] = language

    body, content_type = multipart_form_data(
        fields=fields,
        file_field="file",
        filename="esp32-recording.wav",
        content_type="audio/wav",
        data=wav_bytes,
    )

    request = urllib.request.Request(
        CONFIG.get("transcriptions_url", DEFAULT_TRANSCRIPTIONS_URL),
        data=body,
        headers={
            "Authorization": f"Bearer {api_key}",
            "Accept": "application/json",
            "Content-Type": content_type,
            "Content-Length": str(len(body)),
            "User-Agent": "esp32-voice-ai-mic/1.0",
        },
        method="POST",
    )

    with urllib.request.urlopen(request, timeout=60) as response:
        payload = json.loads(response.read().decode("utf-8"))

    text = payload.get("text")
    if not isinstance(text, str):
        raise RuntimeError(f"OpenAI response did not contain text: {payload!r}")
    return text


class Handler(BaseHTTPRequestHandler):
    server_version = "ESP32STTBridge/1.0"

    def do_GET(self) -> None:
        if self.path == "/health":
            self.send_json(200, {"ok": True, "model": CONFIG.get("model", "")})
            return
        self.send_json(404, {"error": "not found"})

    def do_POST(self) -> None:
        if self.path != "/stt":
            self.send_json(404, {"error": "not found"})
            return

        content_length = self.headers.get("Content-Length")
        if not content_length:
            self.send_json(411, {"error": "Content-Length required"})
            return

        try:
            size = int(content_length)
        except ValueError:
            self.send_json(400, {"error": "invalid Content-Length"})
            return

        if size <= 44 or size > MAX_UPLOAD_BYTES:
            self.send_json(413, {"error": f"bad upload size: {size}"})
            return

        wav_bytes = self.rfile.read(size)
        print(f"received {len(wav_bytes)} bytes from {self.client_address[0]}", flush=True)
        saved_path = save_recording(wav_bytes, self.client_address[0])
        print(f"saved wav: {saved_path}", flush=True)

        try:
            text = transcribe_with_openai(wav_bytes)
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            print(f"STT HTTP error {exc.code}: {detail}", file=sys.stderr, flush=True)
            self.send_json(502, {"error": "stt_http_error", "status": exc.code, "detail": detail})
            return
        except Exception as exc:
            print(f"transcription error: {exc}", file=sys.stderr, flush=True)
            self.send_json(500, {"error": str(exc)})
            return

        print(f"transcript: {text}", flush=True)
        self.send_json(200, {"text": text})

    def send_json(self, status: int, payload: dict[str, object]) -> None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"{self.address_string()} - {fmt % args}", flush=True)


def main() -> int:
    global CONFIG, RECORDINGS_DIR

    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--config", default=str(DEFAULT_CONFIG_PATH))
    parser.add_argument("--recordings-dir", default=str(DEFAULT_RECORDINGS_DIR))
    args = parser.parse_args()

    config_path = Path(args.config)
    CONFIG = load_config(config_path)
    RECORDINGS_DIR = Path(args.recordings_dir)

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"STT bridge listening on http://{args.host}:{args.port}/stt", flush=True)
    print(f"config={config_path}", flush=True)
    print(f"endpoint={CONFIG.get('transcriptions_url')}", flush=True)
    print(f"model={CONFIG.get('model')} language={CONFIG.get('language') or '(auto)'}", flush=True)
    print(f"api_key={mask_secret(CONFIG.get('api_key', ''))}", flush=True)
    print(f"recordings_dir={RECORDINGS_DIR}", flush=True)
    print_config_warnings()
    print("Try these health URLs from a browser on this computer:", flush=True)
    print(f"  http://127.0.0.1:{args.port}/health", flush=True)
    try:
        host_name = socket.gethostname()
        for ip in sorted(set(socket.gethostbyname_ex(host_name)[2])):
            if not ip.startswith("127."):
                print(f"  http://{ip}:{args.port}/health", flush=True)
    except OSError:
        pass
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
