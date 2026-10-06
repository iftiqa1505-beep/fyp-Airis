#pragma once

/*
 * Copy this file to secrets.h and fill in your values.
 * secrets.h is gitignored — do not commit real credentials.
 */

// --- Wi-Fi ---
#define WIFI_SSID           "YOUR_WIFI_SSID"
#define WIFI_PASS           "YOUR_WIFI_PASSWORD"

// --- Blynk: Air Quality station ---
#define BLYNK_TEMPLATE_ID_AQ    "TMPLxxxxxxxx"
#define BLYNK_TEMPLATE_NAME_AQ  "AIRIS AIR QUALITY"
#define BLYNK_AUTH_TOKEN_AQ     "YOUR_AIR_QUALITY_BLYNK_TOKEN"

// --- Blynk: Weather station ---
#define BLYNK_TEMPLATE_ID_WX    "TMPLxxxxxxxx"
#define BLYNK_TEMPLATE_NAME_WX  "AIRIS WEATHER STATION"
#define BLYNK_AUTH_TOKEN_WX     "YOUR_WEATHER_BLYNK_TOKEN"

/*
 * Firebase Realtime Database
 * Host only — no https:// and no trailing slash, e.g.:
 *   myproject-default-rtdb.asia-southeast1.firebasedatabase.app
 * Auth: Database secret (Project Settings → Service accounts → Database secrets)
 *       or a valid ID token. Leave empty to skip Firebase publish.
 */
#define FIREBASE_HOST   "YOUR_PROJECT-default-rtdb.REGION.firebasedatabase.app"
#define FIREBASE_AUTH   "YOUR_DATABASE_SECRET_OR_TOKEN"

/*
 * Station identity — set ONE value per physical board before flashing:
 *   weather-a | weather-b | air-a | air-b
 * Each board also needs its own Blynk auth token (do not share tokens).
 */
#define STATION_ID   "weather-a"
