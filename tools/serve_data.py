#!/usr/bin/env python3
from __future__ import annotations

import argparse
import copy
import json
import math
import threading
import time
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlparse


PROJECT_ROOT = Path(__file__).resolve().parent.parent
DATA_DIR = PROJECT_ROOT / "data"
LOCK = threading.Lock()

AXES = [
    {"id": "x", "label": "X"},
    {"id": "y", "label": "Y"},
    {"id": "z", "label": "Z"},
    {"id": "rx", "label": "Rx"},
    {"id": "ry", "label": "Ry"},
    {"id": "rz", "label": "Rz"},
    {"id": "slider", "label": "Slider"},
    {"id": "wheel", "label": "Wheel"},
    {"id": "dial", "label": "Dial"},
]

DEFAULT_CONFIG = {
    "ap": {"ssid": "ESP32-S3-JOY", "passwordSet": False},
    "network": {"devMode": False, "mode": "mock", "ssid": "localhost", "ip": "127.0.0.1"},
    "app": {"pollMs": 200, "ppmChannelCount": 8},
    "storageOk": True,
    "axes": [
        {"id": axis["id"], "label": axis["label"], "trim": 0, "deadZone": 0, "expo": 0}
        for axis in AXES
    ],
    "channels": [
        {
            "index": index + 1,
            "source": AXES[index]["id"] if index < len(AXES) else "none",
            "invert": False,
        }
        for index in range(8)
    ],
}

CURRENT_CONFIG = copy.deepcopy(DEFAULT_CONFIG)


def clamp(value: float, lower: float, upper: float) -> float:
    return max(lower, min(upper, value))


def apply_axis_config(normalized: float, axis_config: dict[str, Any]) -> float:
    value = clamp(normalized + (axis_config["trim"] / 100.0), -1.0, 1.0)
    dead_zone = clamp(axis_config["deadZone"] / 100.0, 0.0, 0.95)
    if abs(value) <= dead_zone:
        return 0.0
    if dead_zone > 0.0:
        sign = -1.0 if value < 0.0 else 1.0
        value = sign * ((abs(value) - dead_zone) / (1.0 - dead_zone))
    expo = clamp(axis_config["expo"] / 100.0, -1.0, 1.0)
    return clamp((value * (1.0 - expo)) + ((value ** 3) * expo), -1.0, 1.0)


def generate_state() -> dict[str, Any]:
    now = time.time()
    buttons = [False] * 32
    buttons[int(now * 2) % len(buttons)] = True
    buttons[int(now * 3.5) % len(buttons)] = True

    axes = []
    for index, axis in enumerate(AXES):
        phase = now * (0.65 + (index * 0.08))
        normalized = math.sin(phase)
        processed = apply_axis_config(normalized, CURRENT_CONFIG["axes"][index])
        raw = int(normalized * 32767)
        axes.append(
            {
                "id": axis["id"],
                "label": axis["label"],
                "present": True,
                "raw": raw,
                "logicalMin": -32768,
                "logicalMax": 32767,
                "normalized": round(normalized, 4),
                "processed": round(processed, 4),
            }
        )

    axis_values = {axis["id"]: axis["processed"] for axis in axes}
    channels = []
    for channel in CURRENT_CONFIG["channels"]:
        source = channel["source"]
        if source == "none":
            value = 0.0
            source_label = "Nessuno"
        else:
            value = axis_values.get(source, 0.0)
            source_label = next(
                (axis["label"] for axis in AXES if axis["id"] == source),
                source,
            )
        if channel["invert"]:
            value = -value
        pulse = int(round(1500 + (value * 500)))
        channels.append(
            {
                "index": channel["index"],
                "source": source,
                "sourceLabel": source_label,
                "invert": channel["invert"],
                "pulse": pulse,
            }
        )

    return {
        "connected": True,
        "sequence": int(now * 10),
        "device": {"address": 1, "interface": 0, "protocol": "MOCK"},
        "axes": axes,
        "buttons": buttons,
        "channels": channels,
    }


def validate_payload(payload: dict[str, Any]) -> dict[str, Any]:
    next_config = copy.deepcopy(CURRENT_CONFIG)

    axes = payload.get("axes", [])
    if isinstance(axes, list):
        axis_map = {axis["id"]: axis for axis in axes if isinstance(axis, dict)}
        for axis in next_config["axes"]:
            incoming = axis_map.get(axis["id"])
            if not incoming:
                continue
            axis["trim"] = int(clamp(int(incoming.get("trim", axis["trim"])), -100, 100))
            axis["deadZone"] = int(clamp(int(incoming.get("deadZone", axis["deadZone"])), 0, 95))
            axis["expo"] = int(clamp(int(incoming.get("expo", axis["expo"])), -100, 100))

    channels = payload.get("channels", [])
    valid_sources = {"none"} | {axis["id"] for axis in AXES}
    if isinstance(channels, list):
        for index, incoming in enumerate(channels):
            if index >= len(next_config["channels"]) or not isinstance(incoming, dict):
                continue
            source = incoming.get("source", next_config["channels"][index]["source"])
            next_config["channels"][index]["source"] = source if source in valid_sources else "none"
            next_config["channels"][index]["invert"] = bool(incoming.get("invert", False))

    return next_config


def zero_current_config() -> dict[str, Any]:
    next_config = copy.deepcopy(CURRENT_CONFIG)
    state = generate_state()
    for index, axis in enumerate(state["axes"]):
        next_config["axes"][index]["trim"] = int(clamp(round(-axis["normalized"] * 100), -100, 100))
    return next_config


class RequestHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, directory=str(DATA_DIR), **kwargs)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/api/config":
            with LOCK:
                self.send_json(copy.deepcopy(CURRENT_CONFIG))
            return
        if parsed.path == "/api/state":
            with LOCK:
                self.send_json(generate_state())
            return
        super().do_GET()

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path not in {"/api/config", "/api/config/zero", "/api/config/reset"}:
            self.send_error(HTTPStatus.NOT_FOUND, "Unknown endpoint")
            return

        with LOCK:
            global CURRENT_CONFIG
            if parsed.path == "/api/config":
                length = int(self.headers.get("Content-Length", "0"))
                raw_body = self.rfile.read(length).decode("utf-8") if length else "{}"
                payload = json.loads(raw_body or "{}")
                CURRENT_CONFIG = validate_payload(payload)
            elif parsed.path == "/api/config/zero":
                CURRENT_CONFIG = zero_current_config()
            else:
                CURRENT_CONFIG = copy.deepcopy(DEFAULT_CONFIG)

            self.send_json(copy.deepcopy(CURRENT_CONFIG))

    def send_json(self, payload: dict[str, Any]) -> None:
        encoded = json.dumps(payload).encode("utf-8")
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(encoded)


def main() -> None:
    parser = argparse.ArgumentParser(description="Serve data/ with mock ESP32 APIs")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), RequestHandler)
    print(f"Serving {DATA_DIR} on http://{args.host}:{args.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
