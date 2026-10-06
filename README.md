# AIRIS — Yard Weather & Air Quality Monitoring

Final Year Project firmware and **TV andon dashboard** for a multi-station ESP32 deployment on a ~30-acre yard.

Each board publishes to **Blynk** (existing mobile dashboards) **and** **Firebase Realtime Database**. A static **GitHub Pages** site reads live data, shows **yard averages**, and keeps **per-station detail** for QA and ML export.

---

## Table of contents

1. [System overview](#1-system-overview)
2. [Hardware](#2-hardware)
3. [Repository layout](#3-repository-layout)
4. [Secrets & config](#4-secrets--config)
5. [Firebase setup](#5-firebase-setup)
6. [Flash firmware (Arduino IDE)](#6-flash-firmware-arduino-ide)
7. [Dashboard (andon / GitHub Pages)](#7-dashboard-andon--github-pages)
8. [Data model & averaging rules](#8-data-model--averaging-rules)
9. [AQI calculation](#9-aqi-calculation)
10. [ML history export](#10-ml-history-export)
11. [Troubleshooting](#11-troubleshooting)
12. [Limitations (FYP honesty)](#12-limitations-fyp-honesty)

---

## 1. System overview

```text
Weather A/B ESP32 ──┐
                    ├──► Blynk (per-device tokens)
Air A/B ESP32 ──────┤
                    └──► Firebase RTDB ──► GitHub Pages andon dashboard
                         (latest + history)   (yard avg + station strip)
```

| Layer | Role |
|-------|------|
| ESP32 weather ×2 | Temp, humidity, pressure, rain, wind → Blynk + Firebase |
| ESP32 air ×2 | Dust, MQ-135 gases, composite AQI → Blynk + Firebase |
| Firebase Spark | Latest snapshots + append-only history (free tier) |
| `dashboard/` | Full-screen TV andon: Yard AQI/Weather averages + station health |

---

## 2. Hardware

### Station IDs

| `STATION_ID` | UI name | Sketch folder | Sensors |
|---|---|---|---|
| `weather-a` | Weather A | `AIRIS_WeatherStation_Continuous/` | BME280, DFRobot SEN0575, NPN anemometer |
| `weather-b` | Weather B | same | same |
| `air-a` | Air Quality A | `AIRIS_AirQuality_Blynk/` | Sharp GP2Y1010AU0F, MQ-135 |
| `air-b` | Air Quality B | same | same |

### Air quality pins (ESP32)

| Signal | GPIO |
|--------|------|
| GP2Y LED trigger (active LOW) | 2 |
| GP2Y Vo (analog) | 33 |
| MQ-135 AOUT (via 10k/20k divider) | 35 |

### Weather pins (ESP32)

| Signal | GPIO |
|--------|------|
| I2C SDA (BME280 + rain) | 21 |
| I2C SCL | 22 |
| Anemometer (FALLING, needs ~5.1 kΩ pull-up to 3.3 V) | 14 |

BME280 address: `0x76` (fallback `0x77`). Rain sensor I2C: `0x1D`.

---

## 3. Repository layout

| Path | Purpose |
|------|---------|
| `AIRIS_AirQuality_Blynk/` | Arduino sketch (open **this folder** in IDE) |
| `AIRIS_WeatherStation_Continuous/` | Arduino sketch (open **this folder** in IDE) |
| `firebase_publish.h` | Shared HTTPS PUT/POST helpers (also copied into each sketch folder) |
| `secrets.example.h` | Template for Wi‑Fi / Blynk / Firebase / `STATION_ID` |
| `dashboard/` | Andon web UI for GitHub Pages |
| `.gitignore` | Ignores `secrets.h`, `dashboard/config.js` |

**Do not open the repo root as an Arduino sketch.** Each sketch folder must contain:

- the `.ino`
- `secrets.h` (local, gitignored)
- `firebase_publish.h`

---

## 4. Secrets & config

### First-time copy

```bash
copy secrets.example.h secrets.h
copy secrets.example.h AIRIS_AirQuality_Blynk\secrets.h
copy secrets.example.h AIRIS_WeatherStation_Continuous\secrets.h
copy dashboard\config.example.js dashboard\config.js
```

Edit **the `secrets.h` inside the sketch folder you are flashing**.

### Fields in `secrets.h`

| Define | Meaning |
|--------|---------|
| `WIFI_SSID` / `WIFI_PASS` | Site Wi‑Fi |
| `BLYNK_*_AQ` / `BLYNK_*_WX` | Template ID, name, auth token |
| `FIREBASE_HOST` | Host only, no `https://`, e.g. `myproj-default-rtdb.asia-southeast1.firebasedatabase.app` |
| `FIREBASE_AUTH` | Database secret or ID token |
| `STATION_ID` | `weather-a` \| `weather-b` \| `air-a` \| `air-b` |

### Dashboard config

Edit `dashboard/config.js` with the Firebase **web app** config from the console (`apiKey`, `databaseURL`, etc.).

Never commit `secrets.h` or `dashboard/config.js`.

---

## 5. Firebase setup

1. Create a Firebase project → enable **Realtime Database** (pick a nearby region).
2. Start with open rules for bring-up, then tighten:

```json
{
  "rules": {
    ".read": true,
    ".write": true
  }
}
```

3. Project settings → add a **Web** app → paste config into `dashboard/config.js`.
4. Copy database URL host + secret into each board’s `secrets.h`.

### Spark free-tier limits (approx.)

| Resource | Limit |
|----------|-------|
| Concurrent connections | 100 |
| Stored data | 1 GB |
| Downloaded | 10 GB / month |

With weather history ~15 s and air history ~5 min, 1 GB lasts on the order of **years** for latest + history (order-of-magnitude; prune if needed).

---

## 6. Flash firmware (Arduino IDE)

### Requirements

- Arduino IDE 2.x + ESP32 board package ≥ 2.x
- Libraries: **Blynk**
- Weather only: **Adafruit BME280**, **Adafruit Unified Sensor**, **DFRobot Rainfall Sensor**

### Steps

1. Board: **ESP32 Dev Module** (or your exact module).
2. Edit sketch-folder `secrets.h` (`STATION_ID`, unique Blynk token, Wi‑Fi, Firebase).
3. File → Open → select the `.ino` **inside** its folder.
4. Upload. Serial Monitor **115200 baud**.

### Expected Serial

```text
Station ID: weather-a
[NET]  Wi-Fi OK ...
[NET]  Blynk connected.
[FB] Waiting for NTP OK
[FB] PUT /airis/weather/weather-a -> HTTP 200
```

If Firebase is still placeholders: `[FB] Skipped — set FIREBASE_HOST/AUTH` (Blynk can still work).

### Flash matrix (4 boards)

| Board | Open folder | `STATION_ID` | Blynk token |
|-------|-------------|--------------|-------------|
| Weather A | `AIRIS_WeatherStation_Continuous` | `weather-a` | unique |
| Weather B | `AIRIS_WeatherStation_Continuous` | `weather-b` | unique |
| Air A | `AIRIS_AirQuality_Blynk` | `air-a` | unique |
| Air B | `AIRIS_AirQuality_Blynk` | `air-b` | unique |

Change `secrets.h` → upload → next board. Never share `STATION_ID` or Blynk tokens.

### Publish intervals

| Station | Blynk / latest Firebase | History append |
|---------|-------------------------|----------------|
| Weather | ~15 s | every publish (~15 s) |
| Air | ~2 s | every ~5 min |

---

## 7. Dashboard (andon / GitHub Pages)

TV-oriented full-viewport UI:

- Header: **AIRIS**, title, **NORMAL / CAUTION / ALERT** lamp, live clock
- **Yard AQI** (huge) + **Yard Weather** tiles
- Secondary air metrics (dust, CO₂, NH₃, VOC proxy)
- Station strip: Weather A/B temp, Air A/B AQI

### Local preview

```bash
cd dashboard
python -m http.server 8080
```

Open `http://localhost:8080`. Without `config.js` filled, demo data is shown.

### GitHub Pages

Repo **Settings → Pages** → deploy from branch → folder **`/dashboard`**.

Office TV: open the Pages URL fullscreen (F11 / kiosk mode).

---

## 8. Data model & averaging rules

### Firebase paths

```text
/airis/weather/weather-a | weather-b
/airis/air/air-a | air-b
/airis/history/weather/<id>/{pushId}
/airis/history/air/<id>/{pushId}
```

No server-side `/airis/yard` node — the browser averages live snapshots.

### Freshness

| Type | Stale after |
|------|-------------|
| Weather | 60 s |
| Air | 10 s |

### Yard average

- Mean of **fresh** A+B for each metric
- Both live → `2/2 AVG`
- One live → that station’s values + `1/2 AVG`
- None → error / offline styling

---

## 9. AQI calculation

Per air station:

1. **Dust sub-index** — EPA PM2.5-style breakpoints on GP2Y µg/m³ (optical density, not certified PM2.5)
2. **Gas sub-index** — MQ-135 `Rs/Ro` mapped to 0–500 bands
3. **Station AQI** = `max(dust, gas)` (worst wins)

**Yard AQI** on the andon = mean of live stations’ AQI values.

MQ-135 multi-gas PPM fields are **curve estimates from one resistance**, not true speciation.

Blynk air pins: V11 AQI, V13 dust, V15–V20 gas estimates.  
Blynk weather pins: V0–V4 temp, humidity, pressure, rain, wind.

---

## 10. ML history export

```text
https://<FIREBASE_HOST>/airis/history/weather/weather-a.json?auth=<SECRET>
https://<FIREBASE_HOST>/airis/history/weather/weather-b.json?auth=<SECRET>
https://<FIREBASE_HOST>/airis/history/air/air-a.json?auth=<SECRET>
https://<FIREBASE_HOST>/airis/history/air/air-b.json?auth=<SECRET>
```

Or Console → Realtime Database → export JSON under `/airis/history`.

```python
import json, pandas as pd

def load_station(path, station):
    with open(path) as f:
        raw = json.load(f) or {}
    rows = [{"pushId": k, "station": station, **v} for k, v in raw.items()]
    return pd.DataFrame(rows)

a = load_station("weather_a.json", "weather-a")
b = load_station("weather_b.json", "weather-b")
df = pd.concat([a, b], ignore_index=True).sort_values("ts")
# Time-align and average for yard features, or keep station as a model input
```

`ts` / `updatedAt` are Unix seconds **UTC** (NTP on the ESP32).

---

## 11. Troubleshooting

| Symptom | Check |
|---------|--------|
| Arduino compiles both sketches / duplicate `setup` | Open the **sketch subfolder**, not repo root |
| `Station ID` warning at boot | `STATION_ID` prefix must match sketch (`weather-*` or `air-*`) |
| `[FB] Skipped` | Fill `FIREBASE_HOST` / `FIREBASE_AUTH` (no `https://` on host) |
| Firebase HTTP 401/403 | Secret wrong, or rules block writes |
| Dashboard “Demo preview” | Fill `dashboard/config.js` |
| Yard shows `1/2 AVG` | Other station offline/stale or wrong `STATION_ID` |
| Blynk widgets fight | Two boards sharing one auth token |
| Rain/BME280 init fail | I2C wiring; rain needs ~2 s power-up (firmware waits) |
| Anemometer always 0 | External pull-up on GPIO 14 |

---

## 12. Limitations (FYP honesty)

- GP2Y dust ≠ certified PM2.5 FEM; AQI breakpoints are for a usable project index
- MQ-135 cannot separate CO₂/NH₃/VOCs chemically; dashboard PPMs are estimates
- Andon “Normal” bands are tropical-yard heuristics for glanceable status, not regulatory limits
- Dual ESP32s of the same type **must** use different `STATION_ID`s or they overwrite each other

---

## License / course use

Final Year Project scaffolding for AIRIS — weather + air quality monitoring with Blynk dual-publish and a Firebase-backed yard andon dashboard.
