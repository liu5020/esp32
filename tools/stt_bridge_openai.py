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
import math
import os
from pathlib import Path
import shutil
import socket
import sys
from datetime import datetime
import uuid
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


DEFAULT_TRANSCRIPTIONS_URL = "https://api.openai.com/v1/audio/transcriptions"
DEFAULT_CONFIG_PATH = Path(__file__).with_name("stt_config.json")
DEFAULT_RECORDINGS_DIR = Path(__file__).with_name("recordings")
DEFAULT_SKETCHES_DIR = Path(__file__).with_name("sketches")
SKETCH_WIDTH = 160
SKETCH_HEIGHT = 160
PRINT_WIDTH = 384
PRINT_HEIGHT = 384
CONFIG: dict[str, str] = {}
RECORDINGS_DIR = DEFAULT_RECORDINGS_DIR
SKETCHES_DIR = DEFAULT_SKETCHES_DIR
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


def get_bitmap_pixel(bitmap: bytes | bytearray, width: int, x: int, y: int) -> bool:
    index = y * width + x
    return (bitmap[index // 8] & (1 << (7 - (index % 8)))) != 0


class MonoCanvas:
    def __init__(self, width: int, height: int) -> None:
        self.width = width
        self.height = height
        self.data = bytearray((width * height + 7) // 8)

    def point(self, x: int, y: int, thickness: int = 1) -> None:
        radius = max(0, thickness // 2)
        for dy in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                self._set_pixel(x + dx, y + dy)

    def _set_pixel(self, x: int, y: int) -> None:
        if x < 0 or y < 0 or x >= self.width or y >= self.height:
            return
        index = y * self.width + x
        self.data[index // 8] |= 1 << (7 - (index % 8))

    def line(self, x0: int, y0: int, x1: int, y1: int, thickness: int = 1) -> None:
        dx = abs(x1 - x0)
        dy = -abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx + dy

        while True:
            self.point(x0, y0, thickness)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def rect(self, x: int, y: int, width: int, height: int, thickness: int = 1) -> None:
        self.line(x, y, x + width - 1, y, thickness)
        self.line(x, y, x, y + height - 1, thickness)
        self.line(x + width - 1, y, x + width - 1, y + height - 1, thickness)
        self.line(x, y + height - 1, x + width - 1, y + height - 1, thickness)

    def polyline(self, points: list[tuple[int, int]], closed: bool = False, thickness: int = 1) -> None:
        if len(points) < 2:
            return
        for start, end in zip(points, points[1:]):
            self.line(start[0], start[1], end[0], end[1], thickness)
        if closed:
            self.line(points[-1][0], points[-1][1], points[0][0], points[0][1], thickness)

    def circle(self, cx: int, cy: int, radius: int, thickness: int = 1) -> None:
        points: list[tuple[int, int]] = []
        for step in range(49):
            angle = (math.tau * step) / 48
            points.append((round(cx + math.cos(angle) * radius), round(cy + math.sin(angle) * radius)))
        self.polyline(points, closed=True, thickness=thickness)

    def ellipse(self, cx: int, cy: int, rx: int, ry: int, thickness: int = 1) -> None:
        points: list[tuple[int, int]] = []
        for step in range(65):
            angle = (math.tau * step) / 64
            points.append((round(cx + math.cos(angle) * rx), round(cy + math.sin(angle) * ry)))
        self.polyline(points, closed=True, thickness=thickness)

    def arc(self, cx: int, cy: int, rx: int, ry: int, start_deg: int, end_deg: int, thickness: int = 1) -> None:
        points: list[tuple[int, int]] = []
        steps = max(8, abs(end_deg - start_deg) // 6)
        for step in range(steps + 1):
            deg = start_deg + (end_deg - start_deg) * step / steps
            angle = math.radians(deg)
            points.append((round(cx + math.cos(angle) * rx), round(cy + math.sin(angle) * ry)))
        self.polyline(points, thickness=thickness)

    def to_hex(self) -> str:
        return self.data.hex()


def draw_sun(canvas: MonoCanvas) -> None:
    canvas.circle(126, 28, 13, 2)
    for angle in range(0, 360, 45):
        rad = math.radians(angle)
        canvas.line(
            round(126 + math.cos(rad) * 18),
            round(28 + math.sin(rad) * 18),
            round(126 + math.cos(rad) * 25),
            round(28 + math.sin(rad) * 25),
            1,
        )


def draw_ground(canvas: MonoCanvas) -> None:
    canvas.line(8, 140, 152, 140, 2)
    for x in range(18, 148, 18):
        canvas.line(x, 140, x + 5, 133, 1)


def draw_house(canvas: MonoCanvas) -> None:
    draw_sun(canvas)
    draw_ground(canvas)
    canvas.rect(42, 78, 76, 60, 2)
    canvas.polyline([(36, 80), (80, 42), (124, 80)], thickness=2)
    canvas.rect(72, 105, 18, 33, 2)
    canvas.rect(52, 92, 16, 16, 1)
    canvas.rect(96, 92, 16, 16, 1)
    canvas.line(60, 92, 60, 108, 1)
    canvas.line(52, 100, 68, 100, 1)


def draw_tree(canvas: MonoCanvas) -> None:
    draw_sun(canvas)
    draw_ground(canvas)
    canvas.rect(73, 90, 15, 50, 2)
    canvas.circle(80, 64, 27, 2)
    canvas.circle(59, 78, 24, 2)
    canvas.circle(101, 78, 24, 2)
    canvas.line(80, 100, 65, 122, 1)
    canvas.line(81, 103, 98, 119, 1)


def draw_flower(canvas: MonoCanvas) -> None:
    draw_sun(canvas)
    draw_ground(canvas)
    canvas.line(80, 82, 80, 140, 2)
    canvas.ellipse(65, 112, 18, 7, 1)
    canvas.ellipse(95, 112, 18, 7, 1)
    for angle in range(0, 360, 60):
        rad = math.radians(angle)
        canvas.ellipse(round(80 + math.cos(rad) * 18), round(65 + math.sin(rad) * 18), 10, 15, 1)
    canvas.circle(80, 65, 10, 2)


def draw_cat(canvas: MonoCanvas) -> None:
    draw_ground(canvas)
    canvas.ellipse(80, 101, 30, 37, 2)
    canvas.circle(80, 65, 32, 2)
    canvas.polyline([(54, 46), (63, 21), (73, 45)], closed=True, thickness=2)
    canvas.polyline([(87, 45), (98, 21), (106, 46)], closed=True, thickness=2)
    canvas.circle(69, 61, 3, 2)
    canvas.circle(91, 61, 3, 2)
    canvas.polyline([(77, 72), (83, 72), (80, 77)], closed=True, thickness=1)
    canvas.arc(72, 78, 8, 8, 10, 90, 1)
    canvas.arc(88, 78, 8, 8, 90, 170, 1)
    canvas.line(50, 70, 24, 62, 1)
    canvas.line(50, 75, 24, 75, 1)
    canvas.line(50, 80, 25, 89, 1)
    canvas.line(110, 70, 136, 62, 1)
    canvas.line(110, 75, 136, 75, 1)
    canvas.line(110, 80, 135, 89, 1)
    canvas.arc(113, 108, 31, 28, -70, 95, 2)


def draw_dog(canvas: MonoCanvas) -> None:
    draw_ground(canvas)
    canvas.ellipse(80, 101, 34, 36, 2)
    canvas.circle(80, 66, 30, 2)
    canvas.ellipse(52, 66, 12, 25, 2)
    canvas.ellipse(108, 66, 12, 25, 2)
    canvas.circle(69, 62, 3, 2)
    canvas.circle(91, 62, 3, 2)
    canvas.ellipse(80, 75, 9, 6, 1)
    canvas.line(80, 81, 80, 90, 1)
    canvas.arc(72, 88, 8, 7, 10, 80, 1)
    canvas.arc(88, 88, 8, 7, 100, 170, 1)
    canvas.arc(113, 98, 25, 20, -50, 80, 2)


def draw_fish(canvas: MonoCanvas) -> None:
    canvas.line(12, 136, 148, 136, 1)
    canvas.ellipse(76, 82, 44, 24, 2)
    canvas.polyline([(118, 82), (148, 58), (148, 106)], closed=True, thickness=2)
    canvas.circle(48, 77, 3, 2)
    canvas.arc(73, 84, 18, 13, 210, 330, 1)
    canvas.line(72, 59, 91, 36, 1)
    canvas.line(74, 105, 92, 128, 1)
    for y in (33, 47, 121):
        canvas.arc(30, y, 12, 5, 200, 340, 1)


def draw_car(canvas: MonoCanvas) -> None:
    draw_sun(canvas)
    canvas.line(10, 130, 150, 130, 2)
    canvas.rect(38, 85, 84, 30, 2)
    canvas.polyline([(54, 85), (70, 62), (96, 62), (112, 85)], thickness=2)
    canvas.line(80, 62, 80, 85, 1)
    canvas.circle(55, 119, 12, 2)
    canvas.circle(105, 119, 12, 2)
    canvas.circle(55, 119, 4, 1)
    canvas.circle(105, 119, 4, 1)


def draw_person(canvas: MonoCanvas) -> None:
    draw_sun(canvas)
    draw_ground(canvas)
    canvas.circle(80, 49, 20, 2)
    canvas.line(80, 69, 80, 106, 2)
    canvas.line(80, 81, 52, 96, 2)
    canvas.line(80, 81, 108, 96, 2)
    canvas.line(80, 106, 61, 138, 2)
    canvas.line(80, 106, 99, 138, 2)
    canvas.arc(80, 55, 10, 7, 20, 160, 1)
    canvas.circle(73, 46, 2, 1)
    canvas.circle(87, 46, 2, 1)


def draw_mountain(canvas: MonoCanvas) -> None:
    draw_sun(canvas)
    canvas.line(8, 140, 152, 140, 2)
    canvas.polyline([(16, 139), (62, 58), (105, 139)], thickness=2)
    canvas.polyline([(67, 139), (110, 78), (148, 139)], thickness=2)
    canvas.polyline([(51, 78), (62, 58), (73, 78)], thickness=1)
    canvas.polyline([(99, 94), (110, 78), (122, 95)], thickness=1)
    canvas.arc(35, 44, 9, 4, 200, 340, 1)
    canvas.arc(55, 40, 9, 4, 200, 340, 1)


def draw_star(canvas: MonoCanvas) -> None:
    points: list[tuple[int, int]] = []
    for i in range(10):
        radius = 46 if i % 2 == 0 else 19
        angle = math.radians(-90 + i * 36)
        points.append((round(80 + math.cos(angle) * radius), round(78 + math.sin(angle) * radius)))
    canvas.polyline(points, closed=True, thickness=2)
    for x, y in [(34, 40), (126, 38), (43, 124), (119, 121)]:
        canvas.line(x - 5, y, x + 5, y, 1)
        canvas.line(x, y - 5, x, y + 5, 1)


def draw_generic(canvas: MonoCanvas) -> None:
    canvas.rect(18, 32, 124, 78, 2)
    canvas.polyline([(48, 110), (40, 132), (68, 110)], thickness=2)
    canvas.circle(52, 70, 9, 2)
    canvas.circle(80, 70, 9, 2)
    canvas.circle(108, 70, 9, 2)
    draw_star(canvas)


def generate_preview_sketch(text: str) -> dict[str, object]:
    canvas = MonoCanvas(SKETCH_WIDTH, SKETCH_HEIGHT)
    lowered = text.lower()

    if any(word in lowered for word in ("房", "屋", "家", "house", "home")):
        title = "house"
        draw_house(canvas)
    elif any(word in lowered for word in ("树", "tree")):
        title = "tree"
        draw_tree(canvas)
    elif any(word in lowered for word in ("花", "flower")):
        title = "flower"
        draw_flower(canvas)
    elif any(word in lowered for word in ("猫", "cat")):
        title = "cat"
        draw_cat(canvas)
    elif any(word in lowered for word in ("狗", "dog")):
        title = "dog"
        draw_dog(canvas)
    elif any(word in lowered for word in ("鱼", "fish")):
        title = "fish"
        draw_fish(canvas)
    elif any(word in lowered for word in ("车", "car")):
        title = "car"
        draw_car(canvas)
    elif any(word in lowered for word in ("人", "小孩", "男孩", "女孩", "person", "boy", "girl")):
        title = "person"
        draw_person(canvas)
    elif any(word in lowered for word in ("山", "mountain")):
        title = "mountain"
        draw_mountain(canvas)
    elif any(word in lowered for word in ("星", "star")):
        title = "star"
        draw_star(canvas)
    else:
        title = "doodle"
        draw_generic(canvas)

    return {
        "width": SKETCH_WIDTH,
        "height": SKETCH_HEIGHT,
        "format": "1bpp_hex_msb_black1",
        "title": title,
        "bitmap": canvas.to_hex(),
    }


def scale_sketch_nearest(sketch: dict[str, object], width: int, height: int) -> dict[str, object]:
    source_width = int(sketch["width"])
    source_height = int(sketch["height"])
    source_bitmap = bytes.fromhex(str(sketch["bitmap"]))
    canvas = MonoCanvas(width, height)

    for y in range(height):
        source_y = min(source_height - 1, (y * source_height) // height)
        for x in range(width):
            source_x = min(source_width - 1, (x * source_width) // width)
            if get_bitmap_pixel(source_bitmap, source_width, source_x, source_y):
                canvas._set_pixel(x, y)

    return {
        "width": width,
        "height": height,
        "format": "1bpp_hex_msb_black1",
        "title": str(sketch.get("title", "sketch")),
        "bitmap": canvas.to_hex(),
    }


def generate_print_sketch(preview_sketch: dict[str, object]) -> dict[str, object]:
    return scale_sketch_nearest(preview_sketch, PRINT_WIDTH, PRINT_HEIGHT)


def generate_sketch_pair(text: str) -> tuple[dict[str, object], dict[str, object]]:
    preview_sketch = generate_preview_sketch(text)
    print_sketch = generate_print_sketch(preview_sketch)
    return preview_sketch, print_sketch


def save_sketch_pbm(sketch: dict[str, object], client_ip: str, label: str, latest_name: str) -> Path:
    SKETCHES_DIR.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    safe_ip = client_ip.replace(":", "_").replace(".", "-")
    path = SKETCHES_DIR / f"{timestamp}_{safe_ip}_{label}.pbm"
    width = int(sketch["width"])
    height = int(sketch["height"])
    bitmap = bytes.fromhex(str(sketch["bitmap"]))
    path.write_bytes(f"P4\n{width} {height}\n".encode("ascii") + bitmap)

    latest_path = SKETCHES_DIR / latest_name
    try:
        shutil.copyfile(path, latest_path)
    except OSError:
        pass
    return path


def save_sketch_pair(preview_sketch: dict[str, object], print_sketch: dict[str, object], client_ip: str) -> tuple[Path, Path]:
    preview_path = save_sketch_pbm(preview_sketch, client_ip, "preview", "latest_preview.pbm")
    print_path = save_sketch_pbm(print_sketch, client_ip, "print", "latest_print.pbm")

    # Backward-compatible alias used by earlier local testing.
    try:
        shutil.copyfile(preview_path, SKETCHES_DIR / "latest.pbm")
    except OSError:
        pass

    return preview_path, print_path


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
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/health":
            self.send_json(200, {"ok": True, "model": CONFIG.get("model", "")})
            return

        if parsed.path == "/draw":
            query = urllib.parse.parse_qs(parsed.query)
            text = query.get("text", ["house"])[0]
            preview_sketch, print_sketch = generate_sketch_pair(text)
            preview_path, print_path = save_sketch_pair(preview_sketch, print_sketch, self.client_address[0])
            print(f"demo preview sketch: {preview_path}", flush=True)
            print(f"demo print sketch: {print_path}", flush=True)
            self.send_json(200, {
                "text": text,
                "image": preview_sketch,
                "print_image": {
                    "width": print_sketch["width"],
                    "height": print_sketch["height"],
                    "format": print_sketch["format"],
                    "saved": str(print_path),
                },
            })
            return

        if parsed.path == "/print":
            query = urllib.parse.parse_qs(parsed.query)
            text = query.get("text", ["house"])[0]
            preview_sketch, print_sketch = generate_sketch_pair(text)
            _, print_path = save_sketch_pair(preview_sketch, print_sketch, self.client_address[0])
            print(f"print sketch: {print_path}", flush=True)
            self.send_json(200, {"text": text, "image": print_sketch})
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
        preview_sketch, print_sketch = generate_sketch_pair(text)
        preview_path, print_path = save_sketch_pair(preview_sketch, print_sketch, self.client_address[0])
        print(f"saved preview sketch: {preview_path}", flush=True)
        print(f"saved print sketch: {print_path}", flush=True)
        self.send_json(200, {
            "text": text,
            "image": preview_sketch,
            "print_image": {
                "width": print_sketch["width"],
                "height": print_sketch["height"],
                "format": print_sketch["format"],
                "saved": str(print_path),
            },
        })

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
    global CONFIG, RECORDINGS_DIR, SKETCHES_DIR

    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--config", default=str(DEFAULT_CONFIG_PATH))
    parser.add_argument("--recordings-dir", default=str(DEFAULT_RECORDINGS_DIR))
    parser.add_argument("--sketches-dir", default=str(DEFAULT_SKETCHES_DIR))
    args = parser.parse_args()

    config_path = Path(args.config)
    CONFIG = load_config(config_path)
    RECORDINGS_DIR = Path(args.recordings_dir)
    SKETCHES_DIR = Path(args.sketches_dir)

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"STT bridge listening on http://{args.host}:{args.port}/stt", flush=True)
    print(f"config={config_path}", flush=True)
    print(f"endpoint={CONFIG.get('transcriptions_url')}", flush=True)
    print(f"model={CONFIG.get('model')} language={CONFIG.get('language') or '(auto)'}", flush=True)
    print(f"api_key={mask_secret(CONFIG.get('api_key', ''))}", flush=True)
    print(f"recordings_dir={RECORDINGS_DIR}", flush=True)
    print(f"sketches_dir={SKETCHES_DIR}", flush=True)
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
