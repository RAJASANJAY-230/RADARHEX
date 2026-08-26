"""RADARHEX local telemetry bridge.

Reads newline-delimited JSON from an ESP32 serial port, stores valid radar
samples in SQLite, serves the single-file dashboard, and pushes live telemetry
to the browser over WebSocket.
"""

from __future__ import annotations

import argparse
import asyncio
import csv
import io
import json
import sqlite3
import threading
import time
import webbrowser
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import serial
import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse


ROOT = Path(__file__).resolve().parent
DASHBOARD_FILE = ROOT / "RadarHex.html"
DATABASE_FILE = ROOT / "radarhex.db"


class BridgeState:
    def __init__(self) -> None:
        self.port = "COM14"
        self.baud = 115200
        self.serial_connected = False
        self.radar_online = False
        self.last_error: str | None = None
        self.latest: dict[str, Any] | None = None
        self.loop: asyncio.AbstractEventLoop | None = None
        self.clients: set[WebSocket] = set()
        self.stop_event = threading.Event()
        self.reader_thread: threading.Thread | None = None
        self.database_lock = threading.Lock()
        self.mode = "usb"
        self.transport = "none"
        self.last_wifi_packet_at = 0.0
        self.wifi_device = ""
        # A vibration module may pulse for only a few milliseconds. Keep the
        # warning active for three seconds so the browser renders it reliably.
        self.vibration_latched_until = 0.0


state = BridgeState()


def utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat()


def as_number(value: Any, default: float | None = None) -> float | None:
    try:
        if value is None:
            return default
        return float(value)
    except (TypeError, ValueError):
        return default


def as_integer(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def normalize_packet(raw: dict[str, Any]) -> dict[str, Any]:
    sensor_online = bool(raw.get("sensorOnline", True))
    detected = sensor_online and bool(
        raw.get("detected", raw.get("radarDetected", False))
    )
    target_count = max(0, as_integer(raw.get("targets", raw.get("targetCount", 0))))
    distance = as_number(raw.get("distance")) if detected else None
    speed = as_number(raw.get("speed")) if detected else None
    energy = as_integer(raw.get("energy", 0)) if detected else 0
    vibration_online = bool(raw.get("vibrationOnline", False))
    vibration_signal = bool(raw.get("vibrationDetected", False)) or bool(
        as_integer(raw.get("vibrationRaw", 0))
    )
    if vibration_online and vibration_signal:
        state.vibration_latched_until = time.monotonic() + 3.0
    vibration_detected = (
        vibration_online and time.monotonic() <= state.vibration_latched_until
    )
    ultrasonic_online = bool(raw.get("ultrasonicOnline", False))
    obstacle_detected = ultrasonic_online and bool(
        raw.get("obstacleDetected", False)
    )
    ultrasonic_distance = as_number(raw.get("ultrasonicDistance"))
    left_distance = as_number(raw.get("leftDistance"))
    right_distance = as_number(raw.get("rightDistance"))
    scan_direction = str(raw.get("scanDirection", "CENTRE")).upper()
    servo_angle = as_integer(raw.get("servoAngle", 90))
    battery = as_number(raw.get("battery", raw.get("batteryPercent")))
    if battery is not None:
        battery = max(0.0, min(100.0, battery))
    battery_voltage = as_number(raw.get("batteryVoltage"))
    battery_connected = bool(
        raw.get("batteryConnected", battery is not None or battery_voltage is not None)
    )

    valid_range = distance is not None and distance >= 0
    if not valid_range:
        detected = False
        target_count = 0
        distance = None
        speed = None
        energy = 0

    motion = "—"
    if detected and speed is not None:
        motion = "STATIC" if abs(speed) < 0.03 else "MOVING"

    targets: list[dict[str, Any]] = []
    if detected and distance is not None:
        targets.append(
            {
                "id": "C4001-RANGE",
                "distance": round(distance, 3),
                "confidence": None,
                "rangeOnly": True,
                "x": 50,
                "y": 50,
            }
        )

    return {
        "timestamp": utc_timestamp(),
        "source": "hardware",
        "live": True,
        "detected": detected,
        "targetCount": target_count,
        "targets": targets,
        "nearestDistance": round(distance, 3) if distance is not None else None,
        "motion": motion,
        "confidence": None,
        "bearing": None,
        "speed": round(speed, 3) if speed is not None else None,
        "energy": energy,
        "battery": round(battery, 1) if battery is not None else None,
        "batteryVoltage": (
            round(battery_voltage, 2) if battery_voltage is not None else None
        ),
        "batteryConnected": battery_connected,
        "wifi": as_number(raw.get("wifiRssi")),
        "temperature": None,
        "cpu": None,
        "latency": 0,
        "packetLoss": None,
        "vibrationDetected": vibration_detected,
        "vibrationRaw": as_integer(raw.get("vibrationRaw", 0)),
        "ultrasonicOnline": ultrasonic_online,
        "obstacleDetected": obstacle_detected,
        "ultrasonicDistance": (
            round(ultrasonic_distance, 1)
            if ultrasonic_distance is not None
            else None
        ),
        "leftDistance": round(left_distance, 1) if left_distance is not None else None,
        "rightDistance": (
            round(right_distance, 1) if right_distance is not None else None
        ),
        "scanDirection": scan_direction,
        "servoAngle": servo_angle,
        "sensors": {
            "radar": "active" if sensor_online else "offline",
            "microphone": "offline",
            "gas": "offline",
            "vibration": (
                "warning"
                if vibration_detected
                else "active" if vibration_online else "offline"
            ),
            "ultrasonic": (
                "warning"
                if obstacle_detected
                else "active" if ultrasonic_online else "offline"
            ),
        },
        "raw": raw,
    }


def initialize_database() -> None:
    with sqlite3.connect(DATABASE_FILE) as connection:
        connection.execute(
            """
            CREATE TABLE IF NOT EXISTS radar_readings (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT NOT NULL,
                detected INTEGER NOT NULL,
                target_count INTEGER NOT NULL,
                distance REAL,
                speed REAL,
                energy INTEGER,
                raw_json TEXT NOT NULL
            )
            """
        )
        connection.execute(
            "CREATE INDEX IF NOT EXISTS idx_radar_timestamp "
            "ON radar_readings(timestamp)"
        )
        connection.commit()


def save_reading(telemetry: dict[str, Any]) -> None:
    with state.database_lock, sqlite3.connect(DATABASE_FILE) as connection:
        connection.execute(
            """
            INSERT INTO radar_readings
                (timestamp, detected, target_count, distance, speed, energy, raw_json)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            """,
            (
                telemetry["timestamp"],
                1 if telemetry["detected"] else 0,
                telemetry["targetCount"],
                telemetry["nearestDistance"],
                telemetry["speed"],
                telemetry["energy"],
                json.dumps(telemetry["raw"], separators=(",", ":")),
            ),
        )
        connection.commit()


def status_message() -> dict[str, Any]:
    wifi_connected = (
        state.last_wifi_packet_at > 0
        and time.monotonic() - state.last_wifi_packet_at <= 5.0
    )
    link_connected = state.serial_connected or wifi_connected
    radar_online = state.radar_online
    if state.transport == "wifi" and not wifi_connected:
        radar_online = False
    return {
        "type": "status",
        "serialConnected": state.serial_connected,
        "wifiConnected": wifi_connected,
        "linkConnected": link_connected,
        "radarOnline": radar_online,
        "transport": state.transport if link_connected else "none",
        "mode": state.mode,
        "deviceId": state.wifi_device or None,
        "port": state.port,
        "lastError": state.last_error,
    }


async def broadcast(message: dict[str, Any]) -> None:
    disconnected: list[WebSocket] = []
    for client in list(state.clients):
        try:
            await client.send_json(message)
        except Exception:
            disconnected.append(client)
    for client in disconnected:
        state.clients.discard(client)


def schedule_broadcast(message: dict[str, Any]) -> None:
    if state.loop is None or state.loop.is_closed():
        return
    asyncio.run_coroutine_threadsafe(broadcast(message), state.loop)


def serial_reader() -> None:
    while not state.stop_event.is_set():
        try:
            with serial.Serial(state.port, state.baud, timeout=1) as connection:
                # Opening a serial port usually resets the ESP32.
                time.sleep(2.0)
                connection.reset_input_buffer()
                state.serial_connected = True
                state.last_error = None
                schedule_broadcast(status_message())

                while not state.stop_event.is_set():
                    line = connection.readline().decode("utf-8", errors="ignore").strip()
                    if not line or not line.startswith("{"):
                        continue

                    try:
                        raw = json.loads(line)
                    except json.JSONDecodeError:
                        continue

                    telemetry = normalize_packet(raw)
                    telemetry["transport"] = "usb"
                    state.latest = telemetry
                    state.radar_online = bool(raw.get("sensorOnline", True))
                    state.transport = "usb"
                    save_reading(telemetry)
                    schedule_broadcast(
                        {
                            "type": "telemetry",
                            "transport": "usb",
                            "port": state.port,
                            "data": telemetry,
                        }
                    )

        except serial.SerialException as error:
            state.serial_connected = False
            state.radar_online = False
            state.last_error = str(error)
            schedule_broadcast(status_message())
            state.stop_event.wait(2.0)
        except Exception as error:  # Keep the bridge alive after malformed input.
            state.serial_connected = False
            state.radar_online = False
            state.last_error = f"Unexpected bridge error: {error}"
            schedule_broadcast(status_message())
            state.stop_event.wait(2.0)


async def status_publisher() -> None:
    while not state.stop_event.is_set():
        await broadcast(status_message())
        await asyncio.sleep(2.0)


@asynccontextmanager
async def lifespan(_: FastAPI):
    initialize_database()
    state.loop = asyncio.get_running_loop()
    state.stop_event.clear()
    if state.mode != "wifi":
        state.reader_thread = threading.Thread(
            target=serial_reader,
            name="radarhex-serial-reader",
            daemon=True,
        )
        state.reader_thread.start()
    publisher = asyncio.create_task(status_publisher())
    try:
        yield
    finally:
        state.stop_event.set()
        publisher.cancel()
        if state.reader_thread and state.reader_thread.is_alive():
            state.reader_thread.join(timeout=2.5)


app = FastAPI(title="RADARHEX Local Bridge", version="2.0.0", lifespan=lifespan)


@app.get("/")
async def dashboard() -> FileResponse:
    return FileResponse(
        DASHBOARD_FILE,
        media_type="text/html",
        headers={
            "Cache-Control": "no-store, no-cache, must-revalidate, max-age=0",
            "Pragma": "no-cache",
            "Expires": "0",
        },
    )


@app.get("/api/status")
async def api_status() -> JSONResponse:
    return JSONResponse(
        {
            **status_message(),
            "database": DATABASE_FILE.name,
            "latest": state.latest,
        }
    )


@app.post("/api/telemetry")
async def receive_wifi_telemetry(raw: dict[str, Any]) -> JSONResponse:
    telemetry = normalize_packet(raw)
    telemetry["transport"] = "wifi"
    state.latest = telemetry
    state.last_wifi_packet_at = time.monotonic()
    state.wifi_device = str(raw.get("deviceId", "RHX-ESP32"))[:64]
    state.transport = "wifi"
    state.radar_online = bool(raw.get("sensorOnline", True))
    state.last_error = None
    save_reading(telemetry)
    await broadcast(
        {
            "type": "telemetry",
            "transport": "wifi",
            "deviceId": state.wifi_device,
            "data": telemetry,
        }
    )
    return JSONResponse({"ok": True, "radarOnline": state.radar_online})


@app.get("/api/history")
async def api_history(limit: int = 100) -> JSONResponse:
    safe_limit = min(max(limit, 1), 5000)
    with state.database_lock, sqlite3.connect(DATABASE_FILE) as connection:
        connection.row_factory = sqlite3.Row
        rows = connection.execute(
            """
            SELECT id, timestamp, detected, target_count, distance, speed, energy
            FROM radar_readings
            ORDER BY id DESC
            LIMIT ?
            """,
            (safe_limit,),
        ).fetchall()
    return JSONResponse([dict(row) for row in rows])


@app.get("/api/export.csv")
async def export_csv() -> StreamingResponse:
    with state.database_lock, sqlite3.connect(DATABASE_FILE) as connection:
        rows = connection.execute(
            """
            SELECT timestamp, detected, target_count, distance, speed, energy
            FROM radar_readings
            ORDER BY id ASC
            """
        ).fetchall()

    output = io.StringIO()
    writer = csv.writer(output)
    writer.writerow(
        ["timestamp", "detected", "target_count", "distance_m", "speed_m_s", "energy"]
    )
    writer.writerows(rows)
    output.seek(0)

    filename = f"radarhex-readings-{datetime.now().strftime('%Y%m%d-%H%M%S')}.csv"
    return StreamingResponse(
        iter([output.getvalue()]),
        media_type="text/csv",
        headers={"Content-Disposition": f'attachment; filename="{filename}"'},
    )


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket) -> None:
    await websocket.accept()
    state.clients.add(websocket)
    await websocket.send_json(status_message())
    if state.latest is not None:
        await websocket.send_json(
            {
                "type": "telemetry",
                "transport": state.latest.get("transport", state.transport),
                "port": state.port,
                "data": state.latest,
            }
        )

    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        state.clients.discard(websocket)
    except Exception:
        state.clients.discard(websocket)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the RADARHEX local bridge")
    parser.add_argument("--port", default="COM14", help="ESP32 serial port")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--web-port", type=int, default=8000, help="Dashboard port")
    parser.add_argument(
        "--mode",
        choices=("usb", "wifi"),
        default="usb",
        help="Telemetry transport to use",
    )
    parser.add_argument("--no-browser", action="store_true", help="Do not open Chrome")
    return parser.parse_args()


if __name__ == "__main__":
    arguments = parse_arguments()
    state.port = arguments.port
    state.baud = arguments.baud
    state.mode = arguments.mode

    if not arguments.no_browser:
        threading.Timer(
            1.5,
            lambda: webbrowser.open(f"http://127.0.0.1:{arguments.web_port}"),
        ).start()

    uvicorn.run(
        app,
        host="0.0.0.0",
        port=arguments.web_port,
        log_level="info",
    )
