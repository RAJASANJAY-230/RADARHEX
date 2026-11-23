# RADARHEX Live Radar Dashboard

This version sends C4001 radar readings through the ESP32 USB cable to a local
Python bridge. The bridge displays live data in the dashboard and stores every
valid reading in SQLite. Your laptop stays connected to its normal Wi-Fi because
the ESP32 does not create a Wi-Fi hotspot.

## Current hardware scope

- ESP32-WROOM-DA
- DFRobot C4001 24 GHz mmWave presence sensor
- USB cable from ESP32 to the laptop
- Other sensors are intentionally shown as offline until they are connected

### Wiring

| C4001 pin | ESP32 pin |
|---|---|
| VIN | 3V3 |
| GND | GND |
| TX | GPIO16 (RX2) |
| RX | GPIO17 (TX2) |
| OUT | Not connected |

Disconnect USB power before changing any wiring. Keep the radar antenna face
clear and do not place wires or metal directly in front of it.

## Step 1 — Upload the ESP32 program

1. Open `firmware/RADARHEX_Radar_USB/RADARHEX_Radar_USB.ino` in Arduino IDE.
2. No external radar library is required by the V1.1 non-blocking firmware.
3. Select **ESP32-WROOM-DA Module** and **COM14**.
4. Upload the sketch. If upload waits at `Connecting...`, hold **BOOT**, start
   upload, release BOOT when writing begins, then press **EN/RESET** once.
5. Open Serial Monitor at **115200 baud** for one quick test. You should see
   one JSON line about every 500 ms.
6. Close Serial Monitor completely. Only one program can use COM14 at a time.

The V1.1 firmware reads C4001 UART frames directly. It does not issue radar
configuration commands on every boot, preventing the library from waiting
indefinitely after a USB disconnect/reconnect.

Example live packet:

```json
{"sensorOnline":true,"detected":true,"targets":1,"distance":1.09,"speed":0.00,"energy":170056}
```

## Step 2 — Install the laptop bridge (one time)

1. Install Python 3 for Windows if it is not already installed. Enable
   **Add Python to PATH** during installation.
2. Double-click `setup.bat`.
3. Wait for **Setup complete**.

## Step 3 — Run RADARHEX

1. Leave the ESP32 connected by USB.
2. Close Arduino Serial Monitor and Serial Plotter.
3. Double-click `start.bat`. It uses COM14 and web port 8001, reuses an
   existing RADARHEX server when possible, and opens the browser automatically.
4. The dashboard should open at <http://127.0.0.1:8000>.
5. Click **Launch Dashboard**. The top bar should change to **CONNECTED**, and
   the radar panel will show a circular range return when presence is detected.

The server runs in a minimized command window. Double-click `stop.bat` when you
want to stop RADARHEX.

## Wi-Fi mode (phone hotspot)

The Wi-Fi version keeps the USB firmware as a fallback. The ESP32 and laptop
must both be connected to the same 2.4 GHz phone hotspot.

1. Open `firmware/RADARHEX_Radar_WiFi/RADARHEX_Radar_WiFi.ino`.
2. Replace `ENTER_YOUR_HOTSPOT_NAME` and `ENTER_YOUR_HOTSPOT_PASSWORD` locally.
   Do not share the password or commit it to a public repository.
3. The configured laptop address is `10.61.201.12` and the receiver URL is
   `http://10.61.201.12:8001/api/telemetry`.
4. Upload the Wi-Fi firmware once through USB.
5. Close Serial Monitor, allow Python through Windows Firewall on Private
   networks if prompted, and double-click `start-wifi.bat`.
6. The ESP32 may then be powered by a power bank; radar data travels over Wi-Fi.

If the phone assigns the laptop a different IPv4 address later, update
`TELEMETRY_URL` in the Wi-Fi sketch and upload it again.

## Data and API

- Local database: `radarhex.db` (created automatically)
- Current status: <http://127.0.0.1:8000/api/status>
- Recent database readings: <http://127.0.0.1:8000/api/history?limit=100>
- CSV export: <http://127.0.0.1:8000/api/export.csv>

The **Export Log** button downloads all database readings while live mode is
connected. No cloud account or API key is required.

## If COM14 changes

Windows can assign another COM number. Check Arduino IDE **Tools > Port**, then
either edit `COM14` in `start.bat` or run:

```bat
py backend.py --port COM7
```

Replace `COM7` with the port shown on your laptop.

## Troubleshooting

### Dashboard says ESP32 WAITING

- Close Arduino Serial Monitor and Serial Plotter.
- Confirm the USB cable is a data cable.
- Confirm the correct COM port in Device Manager or Arduino IDE.
- Press ESP32 **EN/RESET** once, then wait three seconds.
- Open <http://127.0.0.1:8000/api/status> and read `lastError`.

### Radar Link says NO DATA

- Recheck crossed UART wiring: radar TX goes to GPIO16; radar RX goes to GPIO17.
- Confirm radar VIN is on 3V3 and both boards share GND.
- Confirm the C4001 library sketch prints valid JSON at 115200 baud.

### `py` is not recognized

Install Python 3, or replace `py` with `python` in `setup.bat` and `start.bat`.

## Measurement limitations

The current C4001 connection reports presence, an estimated target count, one
range value, radial speed, and signal energy. It does **not** report bearing or
an x/y position, cannot identify rescuer versus victim, and should not be
presented as verified multi-person localisation. The dashboard therefore shows
a range ring rather than inventing a direction. Future versions can add more
radars, tagged-rescuer filtering, sensor fusion, and a trained classifier after
a labelled dataset has been collected safely.
