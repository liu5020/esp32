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
import io
import json
import math
import os
from pathlib import Path
import shutil
import socket
import sys
import time
from datetime import datetime
import uuid
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


DEFAULT_TRANSCRIPTIONS_URL = "https://api.openai.com/v1/audio/transcriptions"
DEFAULT_IMAGE_GENERATION_URL = "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"
DEFAULT_LEGACY_IMAGE_GENERATION_URL = "https://dashscope.aliyuncs.com/api/v1/services/aigc/text2image/image-synthesis"
DEFAULT_IMAGE_TASK_URL_TEMPLATE = "https://dashscope.aliyuncs.com/api/v1/tasks/{task_id}"
DEFAULT_IMAGE_NEGATIVE_PROMPT = (
    "color, grayscale, shadow, shading, gradient, photo, realistic texture, text, "
    "watermark, logo, low contrast, messy background, filled black background"
)
DEFAULT_CONFIG_PATH = Path(__file__).with_name("stt_config.json")
DEFAULT_RECORDINGS_DIR = Path(__file__).with_name("recordings")
DEFAULT_SKETCHES_DIR = Path(__file__).with_name("sketches")
SKETCH_WIDTH = 160
SKETCH_HEIGHT = 160
PRINT_WIDTH = 384
PRINT_HEIGHT = 384
CONFIG: dict[str, object] = {}
RECORDINGS_DIR = DEFAULT_RECORDINGS_DIR
SKETCHES_DIR = DEFAULT_SKETCHES_DIR
MAX_UPLOAD_BYTES = 2 * 1024 * 1024


def parse_bool(value: object, default: bool = False) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        lowered = value.strip().lower()
        if lowered in ("1", "true", "yes", "on"):
            return True
        if lowered in ("0", "false", "no", "off"):
            return False
    return default


def load_config(path: Path) -> dict[str, object]:
    config: dict[str, object] = {
        "stt_provider": "dashscope_paraformer",
        "stt_api_key": "",
        "stt_model": "paraformer-realtime-v2",
        "stt_format": "wav",
        "stt_sample_rate": 16000,
        "api_key": "",
        "transcriptions_url": DEFAULT_TRANSCRIPTIONS_URL,
        "model": "gpt-4o-mini-transcribe",
        "language": "zh",
        "image_provider": "aliyun_wanx",
        "image_api_key": "",
        "image_generation_url": DEFAULT_LEGACY_IMAGE_GENERATION_URL,
        "image_task_url_template": DEFAULT_IMAGE_TASK_URL_TEMPLATE,
        "image_model": "wan2.2-t2i-flash",
        "image_call_mode": "auto",
        "image_payload_format": "auto",
        "image_size": "1024*1024",
        "image_prompt_extend": True,
        "image_watermark": False,
        "image_negative_prompt": DEFAULT_IMAGE_NEGATIVE_PROMPT,
        "image_task_poll_seconds": 2,
        "image_task_timeout_seconds": 180,
        "image_threshold": 210,
        "image_autocontrast_cutoff": 2,
        "image_fallback_local": True,
    }

    if path.exists():
        # Windows PowerShell 5 writes UTF-8 JSON with a BOM by default.
        # utf-8-sig accepts both BOM and non-BOM files.
        with path.open("r", encoding="utf-8-sig") as f:
            file_config = json.load(f)
        for key in config:
            value = file_config.get(key)
            if value is not None and value != "":
                config[key] = value

    # Environment variables still work and override the file when present.
    config["stt_provider"] = os.environ.get("STT_PROVIDER", str(config["stt_provider"]))
    config["stt_api_key"] = (
        os.environ.get("STT_API_KEY")
        or os.environ.get("DASHSCOPE_API_KEY")
        or os.environ.get("ALIYUN_API_KEY")
        or config["stt_api_key"]
    )
    config["stt_model"] = (
        os.environ.get("DASHSCOPE_STT_MODEL")
        or os.environ.get("ALIYUN_STT_MODEL")
        or os.environ.get("STT_MODEL")
        or config["stt_model"]
    )
    config["stt_format"] = os.environ.get("STT_FORMAT", str(config["stt_format"]))
    config["stt_sample_rate"] = os.environ.get("STT_SAMPLE_RATE", config["stt_sample_rate"])
    config["api_key"] = os.environ.get("STT_API_KEY") or os.environ.get("OPENAI_API_KEY") or config["api_key"]
    config["transcriptions_url"] = os.environ.get("STT_TRANSCRIPTIONS_URL", config["transcriptions_url"])
    config["model"] = os.environ.get("STT_MODEL") or os.environ.get("OPENAI_TRANSCRIBE_MODEL", config["model"])
    config["language"] = os.environ.get("STT_LANGUAGE") or os.environ.get("OPENAI_TRANSCRIBE_LANGUAGE", config["language"])
    config["image_provider"] = os.environ.get("IMAGE_PROVIDER", str(config["image_provider"]))
    config["image_api_key"] = (
        os.environ.get("IMAGE_API_KEY")
        or os.environ.get("DASHSCOPE_API_KEY")
        or os.environ.get("ALIYUN_API_KEY")
        or config["image_api_key"]
    )
    config["image_generation_url"] = os.environ.get("IMAGE_GENERATION_URL", str(config["image_generation_url"]))
    config["image_task_url_template"] = os.environ.get(
        "IMAGE_TASK_URL_TEMPLATE", str(config["image_task_url_template"])
    )
    config["image_model"] = os.environ.get("IMAGE_MODEL", str(config["image_model"]))
    config["image_call_mode"] = os.environ.get("IMAGE_CALL_MODE", str(config["image_call_mode"]))
    config["image_payload_format"] = os.environ.get("IMAGE_PAYLOAD_FORMAT", str(config["image_payload_format"]))
    config["image_size"] = os.environ.get("IMAGE_SIZE", str(config["image_size"]))
    if "IMAGE_PROMPT_EXTEND" in os.environ:
        config["image_prompt_extend"] = parse_bool(os.environ.get("IMAGE_PROMPT_EXTEND"), True)
    if "IMAGE_WATERMARK" in os.environ:
        config["image_watermark"] = parse_bool(os.environ.get("IMAGE_WATERMARK"), False)
    if "IMAGE_NEGATIVE_PROMPT" in os.environ:
        config["image_negative_prompt"] = os.environ["IMAGE_NEGATIVE_PROMPT"]
    if "IMAGE_THRESHOLD" in os.environ:
        config["image_threshold"] = os.environ["IMAGE_THRESHOLD"]
    if "IMAGE_TASK_POLL_SECONDS" in os.environ:
        config["image_task_poll_seconds"] = os.environ["IMAGE_TASK_POLL_SECONDS"]
    if "IMAGE_TASK_TIMEOUT_SECONDS" in os.environ:
        config["image_task_timeout_seconds"] = os.environ["IMAGE_TASK_TIMEOUT_SECONDS"]
    if "IMAGE_FALLBACK_LOCAL" in os.environ:
        config["image_fallback_local"] = parse_bool(os.environ.get("IMAGE_FALLBACK_LOCAL"), True)
    return config


def get_config_str(key: str, default: str = "") -> str:
    value = CONFIG.get(key, default)
    if value is None:
        return default
    return str(value)


def get_config_bool(key: str, default: bool = False) -> bool:
    return parse_bool(CONFIG.get(key, default), default)


def get_config_int(key: str, default: int) -> int:
    try:
        return int(CONFIG.get(key, default))
    except (TypeError, ValueError):
        return default


def get_stt_provider() -> str:
    return get_config_str("stt_provider", "dashscope_paraformer").strip().lower()


def get_stt_api_key() -> str:
    key = get_config_str("stt_api_key")
    if key:
        return key
    if get_stt_provider() in ("dashscope", "dashscope_paraformer", "aliyun", "aliyun_paraformer"):
        return get_config_str("image_api_key")
    return get_config_str("api_key")


def mask_secret(value: str) -> str:
    if not value or value == "PUT_YOUR_API_KEY_HERE":
        return "(not set)"
    if len(value) <= 10:
        return f"{value[:3]}... len={len(value)}"
    return f"{value[:6]}...{value[-4:]} len={len(value)}"


def print_config_warnings() -> None:
    api_key = get_stt_api_key()
    endpoint = get_config_str("transcriptions_url")
    stt_provider = get_stt_provider()
    image_provider = get_config_str("image_provider", "local").lower()
    image_api_key = get_config_str("image_api_key")

    if stt_provider in ("openai", "openai_compatible", "groq") and "api.groq.com" in endpoint and not api_key.startswith("gsk_"):
        print("WARNING: Groq endpoint is selected, but api_key does not start with gsk_.", flush=True)

    if "..." in api_key:
        print("WARNING: STT api key contains '...'. The dashboard masked key cannot be used.", flush=True)

    if stt_provider in ("dashscope", "dashscope_paraformer", "aliyun", "aliyun_paraformer"):
        if not api_key or api_key == "PUT_YOUR_DASHSCOPE_API_KEY_HERE":
            print("WARNING: DashScope STT key is not set. Set stt_api_key or image_api_key.", flush=True)

    if image_provider in ("aliyun", "aliyun_wanx", "dashscope", "wanx"):
        if not image_api_key or image_api_key == "PUT_YOUR_DASHSCOPE_API_KEY_HERE":
            print("WARNING: image_api_key is not set. Image generation will fall back to local sketches.", flush=True)
        elif "..." in image_api_key:
            print("WARNING: image_api_key contains '...'. The dashboard masked key cannot be used.", flush=True)


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


def save_source_image(image_bytes: bytes) -> Path:
    SKETCHES_DIR.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    path = SKETCHES_DIR / f"{timestamp}_source.png"
    path.write_bytes(image_bytes)
    try:
        shutil.copyfile(path, SKETCHES_DIR / "latest_source.png")
    except OSError:
        pass
    return path


def build_image_prompt(text: str) -> str:
    subject = text.strip() or "a cute simple doodle"
    return (
        "Create a child-friendly black-and-white coloring-book outline drawing. "
        f"Draw only this harmless subject: {subject}. "
        "Use a pure white background, centered composition, simple rounded shapes, "
        "bold clean black outlines, no color, no gray, no shading, no text, no watermark, "
        "no realistic details, no scary expression."
    )


def extract_first_url(value: object) -> str:
    if isinstance(value, str) and value.startswith(("http://", "https://")):
        return value
    if isinstance(value, dict):
        for key in ("image", "url"):
            found = extract_first_url(value.get(key))
            if found:
                return found
        for child in value.values():
            found = extract_first_url(child)
            if found:
                return found
    if isinstance(value, list):
        for child in value:
            found = extract_first_url(child)
            if found:
                return found
    return ""


def extract_aliyun_image_url(payload: dict[str, object]) -> str:
    output = payload.get("output")
    if isinstance(output, dict):
        choices = output.get("choices")
        if isinstance(choices, list):
            for choice in choices:
                if not isinstance(choice, dict):
                    continue
                message = choice.get("message")
                if not isinstance(message, dict):
                    continue
                content = message.get("content")
                if not isinstance(content, list):
                    continue
                for item in content:
                    if isinstance(item, dict) and isinstance(item.get("image"), str):
                        return item["image"]

        found_url = extract_first_url(output)
        if found_url:
            return found_url

    code = payload.get("code")
    message = payload.get("message")
    if code or message:
        raise RuntimeError(f"Aliyun image response error: {code or 'unknown'} {message or ''}".strip())
    raise RuntimeError(f"Aliyun image response did not contain an image URL: {payload!r}")


def get_image_payload_format() -> str:
    configured = get_config_str("image_payload_format", "auto").strip().lower()
    if configured in ("legacy", "messages"):
        return configured
    model = get_config_str("image_model", "wan2.2-t2i-flash").strip().lower()
    return "messages" if model == "wan2.6-t2i" else "legacy"


def get_aliyun_generation_url() -> str:
    configured = get_config_str("image_generation_url", "")
    model = get_config_str("image_model", "wan2.2-t2i-flash").strip().lower()
    if model == "wan2.6-t2i":
        if not configured or configured == DEFAULT_LEGACY_IMAGE_GENERATION_URL:
            return DEFAULT_IMAGE_GENERATION_URL
        return configured
    if not configured or configured == DEFAULT_IMAGE_GENERATION_URL:
        return DEFAULT_LEGACY_IMAGE_GENERATION_URL
    return configured


def build_aliyun_image_payload(text: str) -> dict[str, object]:
    model = get_config_str("image_model", "wan2.2-t2i-flash")
    prompt = build_image_prompt(text)
    parameters = {
        "n": 1,
        "size": get_config_str("image_size", "1024*1024"),
        "watermark": get_config_bool("image_watermark", False),
    }

    if get_image_payload_format() == "messages":
        parameters["prompt_extend"] = get_config_bool("image_prompt_extend", True)
        parameters["negative_prompt"] = get_config_str("image_negative_prompt", DEFAULT_IMAGE_NEGATIVE_PROMPT)
        return {
            "model": model,
            "input": {
                "messages": [
                    {
                        "role": "user",
                        "content": [
                            {
                                "text": prompt,
                            }
                        ],
                    }
                ]
            },
            "parameters": parameters,
        }

    return {
        "model": model,
        "input": {
            "prompt": prompt,
            "negative_prompt": get_config_str("image_negative_prompt", DEFAULT_IMAGE_NEGATIVE_PROMPT),
        },
        "parameters": parameters,
    }


def is_async_image_call() -> bool:
    mode = get_config_str("image_call_mode", "auto").strip().lower()
    if mode in ("async", "task"):
        return True
    if mode == "sync":
        return False
    model = get_config_str("image_model", "wan2.2-t2i-flash").strip().lower()
    return model != "wan2.6-t2i"


def open_aliyun_json(payload: dict[str, object], async_call: bool = False) -> dict[str, object]:
    api_key = get_config_str("image_api_key")
    if not api_key or api_key == "PUT_YOUR_DASHSCOPE_API_KEY_HERE":
        raise RuntimeError("image_api_key is not set")

    headers = {
        "Authorization": f"Bearer {api_key}",
        "Accept": "application/json",
        "Content-Type": "application/json",
        "User-Agent": "esp32-voice-ai-mic/1.0",
    }
    if async_call:
        headers["X-DashScope-Async"] = "enable"

    request = urllib.request.Request(
        get_aliyun_generation_url(),
        data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        headers=headers,
        method="POST",
    )

    try:
        with urllib.request.urlopen(request, timeout=90) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Aliyun image HTTP error {exc.code}: {detail}") from exc


def get_aliyun_task_id(payload: dict[str, object]) -> str:
    output = payload.get("output")
    if isinstance(output, dict) and isinstance(output.get("task_id"), str):
        return output["task_id"]
    if isinstance(payload.get("task_id"), str):
        return str(payload["task_id"])
    raise RuntimeError(f"Aliyun async response did not contain task_id: {payload!r}")


def poll_aliyun_task(task_id: str) -> dict[str, object]:
    api_key = get_config_str("image_api_key")
    timeout_seconds = max(10, get_config_int("image_task_timeout_seconds", 180))
    poll_seconds = max(1, get_config_int("image_task_poll_seconds", 2))
    deadline = time.monotonic() + timeout_seconds
    template = get_config_str("image_task_url_template", DEFAULT_IMAGE_TASK_URL_TEMPLATE)
    url = template.format(task_id=urllib.parse.quote(task_id, safe=""))

    while True:
        request = urllib.request.Request(
            url,
            headers={
                "Authorization": f"Bearer {api_key}",
                "Accept": "application/json",
                "User-Agent": "esp32-voice-ai-mic/1.0",
            },
            method="GET",
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                payload = json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"Aliyun task HTTP error {exc.code}: {detail}") from exc

        output = payload.get("output")
        status = ""
        if isinstance(output, dict):
            status = str(output.get("task_status", "")).upper()

        if status in ("SUCCEEDED", "SUCCESS"):
            return payload
        if status in ("FAILED", "CANCELED", "CANCELLED"):
            code = output.get("code") if isinstance(output, dict) else payload.get("code")
            message = output.get("message") if isinstance(output, dict) else payload.get("message")
            raise RuntimeError(f"Aliyun image task failed: {code or status} {message or ''}".strip())
        if time.monotonic() >= deadline:
            raise RuntimeError(f"Aliyun image task timed out after {timeout_seconds}s: {task_id}")

        time.sleep(poll_seconds)


def request_aliyun_image_bytes(text: str) -> bytes:
    payload = build_aliyun_image_payload(text)
    if is_async_image_call():
        start_payload = open_aliyun_json(payload, async_call=True)
        task_id = get_aliyun_task_id(start_payload)
        print(f"aliyun image async task: {task_id}", flush=True)
        response_payload = poll_aliyun_task(task_id)
    else:
        response_payload = open_aliyun_json(payload, async_call=False)

    image_url = extract_aliyun_image_url(response_payload)
    image_request = urllib.request.Request(
        image_url,
        headers={
            "User-Agent": "esp32-voice-ai-mic/1.0",
        },
    )
    with urllib.request.urlopen(image_request, timeout=90) as response:
        return response.read()


def flatten_image_on_white(image: object) -> object:
    from PIL import Image

    if image.mode in ("RGBA", "LA") or "transparency" in image.info:
        rgba = image.convert("RGBA")
        background = Image.new("RGBA", rgba.size, "WHITE")
        background.alpha_composite(rgba)
        return background.convert("RGB")
    return image.convert("RGB")


def image_bytes_to_sketch(image_bytes: bytes, width: int, height: int, title: str) -> dict[str, object]:
    try:
        from PIL import Image, ImageOps
    except ImportError as exc:
        raise RuntimeError("Pillow is required for AI image conversion. Install with: python -m pip install pillow") from exc

    image = Image.open(io.BytesIO(image_bytes))
    image.load()
    image = flatten_image_on_white(image)
    resample = getattr(Image, "Resampling", Image).LANCZOS
    fitted = ImageOps.contain(image, (width, height), resample)
    canvas_image = Image.new("RGB", (width, height), "white")
    canvas_image.paste(fitted, ((width - fitted.width) // 2, (height - fitted.height) // 2))

    gray = ImageOps.grayscale(canvas_image)
    cutoff = get_config_int("image_autocontrast_cutoff", 2)
    gray = ImageOps.autocontrast(gray, cutoff=max(0, cutoff))

    corners = [
        gray.getpixel((0, 0)),
        gray.getpixel((width - 1, 0)),
        gray.getpixel((0, height - 1)),
        gray.getpixel((width - 1, height - 1)),
    ]
    if sum(corners) / len(corners) < 128:
        gray = ImageOps.invert(gray)

    pixels = gray.load()
    threshold = max(0, min(255, get_config_int("image_threshold", 210)))
    canvas = MonoCanvas(width, height)
    for y in range(height):
        for x in range(width):
            if pixels[x, y] < threshold:
                canvas._set_pixel(x, y)

    return {
        "width": width,
        "height": height,
        "format": "1bpp_hex_msb_black1",
        "title": title,
        "bitmap": canvas.to_hex(),
    }


def generate_aliyun_wanx_sketch_pair(text: str) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    image_bytes = request_aliyun_image_bytes(text)
    source_path = save_source_image(image_bytes)
    print(f"saved source image: {source_path}", flush=True)
    preview_sketch = image_bytes_to_sketch(image_bytes, SKETCH_WIDTH, SKETCH_HEIGHT, "aliyun_wanx")
    print_sketch = image_bytes_to_sketch(image_bytes, PRINT_WIDTH, PRINT_HEIGHT, "aliyun_wanx")
    return preview_sketch, print_sketch, {"image_provider": "aliyun_wanx", "source_image": str(source_path)}


def generate_local_sketch_pair(text: str) -> tuple[dict[str, object], dict[str, object]]:
    preview_sketch = generate_preview_sketch(text)
    print_sketch = generate_print_sketch(preview_sketch)
    return preview_sketch, print_sketch


def generate_sketch_pair(text: str) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    provider = get_config_str("image_provider", "local").strip().lower()
    if provider in ("aliyun", "aliyun_wanx", "dashscope", "wanx"):
        try:
            return generate_aliyun_wanx_sketch_pair(text)
        except Exception as exc:
            if get_config_bool("image_fallback_local", True):
                print(f"image generation failed, falling back to local sketch: {exc}", file=sys.stderr, flush=True)
                preview_sketch, print_sketch = generate_local_sketch_pair(text)
                return preview_sketch, print_sketch, {
                    "image_provider": "local_fallback",
                    "image_error": str(exc)[:500],
                }
            raise

    preview_sketch, print_sketch = generate_local_sketch_pair(text)
    return preview_sketch, print_sketch, {"image_provider": "local"}


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


def extract_dashscope_text(result: object) -> str:
    sentences: object = None
    if hasattr(result, "get_sentence"):
        sentences = result.get_sentence()
    if not sentences and isinstance(result, dict):
        output = result.get("output")
        if isinstance(output, dict):
            sentences = output.get("sentence")

    parts: list[str] = []
    if isinstance(sentences, dict):
        text = sentences.get("text")
        if isinstance(text, str) and text.strip():
            parts.append(text.strip())
    elif isinstance(sentences, list):
        for sentence in sentences:
            if not isinstance(sentence, dict):
                continue
            text = sentence.get("text")
            if isinstance(text, str) and text.strip():
                parts.append(text.strip())

    text = "".join(parts).strip()
    if not text:
        raise RuntimeError(f"DashScope response did not contain text: {result!r}")
    return text


def transcribe_with_dashscope(wav_path: Path) -> str:
    api_key = get_stt_api_key()
    if not api_key or api_key == "PUT_YOUR_DASHSCOPE_API_KEY_HERE":
        raise RuntimeError("DashScope STT key is not set. Set stt_api_key or image_api_key.")

    try:
        import dashscope
        from dashscope.audio.asr import Recognition, RecognitionCallback
    except ImportError as exc:
        raise RuntimeError("dashscope is required for Aliyun STT. Install with: python -m pip install dashscope") from exc

    dashscope.api_key = api_key

    class Callback(RecognitionCallback):
        pass

    recognizer = Recognition(
        model=get_config_str("stt_model", "paraformer-realtime-v2"),
        callback=Callback(),
        format=get_config_str("stt_format", "wav"),
        sample_rate=get_config_int("stt_sample_rate", 16000),
    )
    result = recognizer.call(str(wav_path))
    status_code = result.get("status_code") if isinstance(result, dict) else getattr(result, "status_code", None)
    if status_code is not None and str(status_code) not in ("200", "HTTPStatus.OK"):
        code = result.get("code", "") if isinstance(result, dict) else getattr(result, "code", "")
        message = result.get("message", "") if isinstance(result, dict) else getattr(result, "message", "")
        raise RuntimeError(f"DashScope STT error: {code} {message}".strip())
    return extract_dashscope_text(result)


def transcribe_audio(wav_bytes: bytes, wav_path: Path) -> str:
    provider = get_stt_provider()
    if provider in ("dashscope", "dashscope_paraformer", "aliyun", "aliyun_paraformer"):
        return transcribe_with_dashscope(wav_path)
    return transcribe_with_openai(wav_bytes)


class Handler(BaseHTTPRequestHandler):
    server_version = "ESP32STTBridge/1.0"

    def do_GET(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/health":
            self.send_json(200, {
                "ok": True,
                "stt_provider": get_stt_provider(),
                "stt_model": get_config_str("stt_model"),
                "image_provider": get_config_str("image_provider", "local"),
                "image_model": get_config_str("image_model"),
            })
            return

        if parsed.path == "/draw":
            query = urllib.parse.parse_qs(parsed.query)
            text = query.get("text", ["house"])[0]
            preview_sketch, print_sketch, image_meta = generate_sketch_pair(text)
            preview_path, print_path = save_sketch_pair(preview_sketch, print_sketch, self.client_address[0])
            print(f"demo preview sketch: {preview_path}", flush=True)
            print(f"demo print sketch: {print_path}", flush=True)
            payload = {
                "text": text,
                **image_meta,
                "image": preview_sketch,
                "print_image": {
                    "width": print_sketch["width"],
                    "height": print_sketch["height"],
                    "format": print_sketch["format"],
                    "saved": str(print_path),
                },
            }
            self.send_json(200, payload)
            return

        if parsed.path == "/print":
            query = urllib.parse.parse_qs(parsed.query)
            text = query.get("text", ["house"])[0]
            preview_sketch, print_sketch, image_meta = generate_sketch_pair(text)
            _, print_path = save_sketch_pair(preview_sketch, print_sketch, self.client_address[0])
            print(f"print sketch: {print_path}", flush=True)
            self.send_json(200, {"text": text, **image_meta, "image": print_sketch})
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
            text = transcribe_audio(wav_bytes, saved_path)
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
        preview_sketch, print_sketch, image_meta = generate_sketch_pair(text)
        preview_path, print_path = save_sketch_pair(preview_sketch, print_sketch, self.client_address[0])
        print(f"saved preview sketch: {preview_path}", flush=True)
        print(f"saved print sketch: {print_path}", flush=True)
        self.send_json(200, {
            "text": text,
            **image_meta,
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
    print(
        f"stt_provider={get_stt_provider()} "
        f"stt_model={get_config_str('stt_model')} "
        f"stt_format={get_config_str('stt_format')} "
        f"stt_sample_rate={get_config_int('stt_sample_rate', 16000)}",
        flush=True,
    )
    print(f"stt_api_key={mask_secret(get_stt_api_key())}", flush=True)
    print(f"openai_compatible_endpoint={get_config_str('transcriptions_url')}", flush=True)
    print(f"openai_compatible_model={get_config_str('model')} language={get_config_str('language') or '(auto)'}", flush=True)
    print(
        f"image_provider={get_config_str('image_provider', 'local')} "
        f"image_model={get_config_str('image_model')}",
        flush=True,
    )
    print(f"image_api_key={mask_secret(get_config_str('image_api_key'))}", flush=True)
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
