/*******************************************************************************
 * AI-RIS Outdoor Weather Station Module — CONTINUOUS MODE (No Deep Sleep)
 * ────────────────────────────────────────────────────────────────────────
 * Firmware for NodeMCU ESP32 DevKit (30/38-pin)
 *
 * Sensors:
 *   • Waveshare BME280      – Temperature, Humidity, Pressure  (I2C 0x76)
 *   • DFRobot SEN0575       – Tipping-Bucket Rainfall          (I2C 0x1D)
 *   • NPN Anemometer        – Wind Speed via pulse counting     (GPIO 14)
 *
 * Cloud:  Blynk IoT (V0–V4) + Firebase RTDB (latest + history for ML)
 * Mode:   Continuous operation — samples and uploads every 15 seconds
 *         for live Blynk / AIRIS dashboard monitoring.
 *
 * Target: Arduino IDE / PlatformIO (ESP32 Arduino Core ≥ 2.x)
 * Setup:  Copy secrets.example.h → secrets.h before building.
 ******************************************************************************/

#include "secrets.h"

/* ─── Blynk Template Credentials (set BEFORE #include) ──────────────────── */
#define BLYNK_TEMPLATE_ID   BLYNK_TEMPLATE_ID_WX
#define BLYNK_TEMPLATE_NAME BLYNK_TEMPLATE_NAME_WX
#define BLYNK_AUTH_TOKEN    BLYNK_AUTH_TOKEN_WX

#define BLYNK_NO_FANCY_LOGO
#define BLYNK_PRINT Serial

/* ─── Library Includes ──────────────────────────────────────────────────── */
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <DFRobot_RainfallSensor.h>
#include <BlynkSimpleEsp32.h>
#include "firebase_publish.h"

/* ─── GPIO Pin Definitions ──────────────────────────────────────────────── */
#define PIN_I2C_SDA       21    // Shared I2C data  (BME280 + SEN0575)
#define PIN_I2C_SCL       22    // Shared I2C clock (BME280 + SEN0575)
#define PIN_ANEMOMETER    14    // NPN open-collector signal (FALLING edge)

/*
 * NPN Pull-Up Note:
 * A physical 5.1 kΩ pull-up resistor from GPIO 14 to 3.3V is required.
 * The NPN open-collector output sinks to GND on each tick — the external
 * resistor ensures a clean HIGH idle state.  INPUT_PULLUP (~45 kΩ) alone
 * is too weak at higher pulse frequencies.
 */

/* ─── I2C Addresses ─────────────────────────────────────────────────────── */
#define BME280_ADDR_PRIMARY   0x76
#define BME280_ADDR_FALLBACK  0x77
#define RAIN_SENSOR_ADDR      0x1D

/* ─── Anemometer Calibration ────────────────────────────────────────────── */
/*
 * 20 pulses/rev; 20 pulses/sec = 1.75 m/s
 * Wind Speed (m/s) = (pulseCount / sampleSec) × (1.75 / 20.0)
 */
#define PULSES_PER_REV        20
#define MPS_PER_HZ            (1.75f / 20.0f)  // 0.0875 m/s per Hz

/* ─── Timing Constants ──────────────────────────────────────────────────── */
#define PUBLISH_INTERVAL_MS   15000UL  // Push to Blynk every 15 seconds
#define WIND_SAMPLE_MS        10000UL  // 10-second pulse accumulation window

/* ─── Blynk Virtual Pin Map ─────────────────────────────────────────────── */
#define VPIN_TEMPERATURE  V0   // °C
#define VPIN_HUMIDITY     V1   // %
#define VPIN_PRESSURE     V2   // hPa
#define VPIN_RAINFALL     V3   // mm
#define VPIN_WINDSPEED    V4   // m/s

/* ─── Global Sensor Objects ─────────────────────────────────────────────── */
Adafruit_BME280            bme;
DFRobot_RainfallSensor_I2C rainSensor(&Wire);

/* ─── Sensor Status Flags ───────────────────────────────────────────────── */
bool bmeOk  = false;
bool rainOk = false;

/* ─── Volatile ISR Counter ──────────────────────────────────────────────── */
volatile uint32_t windPulseCount = 0;

/* ─── Timing State ──────────────────────────────────────────────────────── */
unsigned long lastPublishTime   = 0;
unsigned long windSampleStart   = 0;
bool          windSampling      = false;
bool          readyToPublish    = false;

/* ─── Latest Readings ───────────────────────────────────────────────────── */
float temperature = 0.0f;
float humidity    = 0.0f;
float pressure    = 0.0f;
float rainfall    = 0.0f;
float windSpeed   = 0.0f;

/* ─── Rain Tip Tracking (using getRawData instead of getRainfall) ───── */
uint32_t rainBaselineCount = 0;  // Raw tip count at boot
#define MM_PER_TIP  0.2822f

/* ─── ISR: Anemometer Pulse ─────────────────────────────────────────────── */
void IRAM_ATTR onWindPulse() {
    windPulseCount++;
}

/* ─── Helper: Initialise BME280 ─────────────────────────────────────────── */
bool initBME280() {
    if (bme.begin(BME280_ADDR_PRIMARY, &Wire)) {
        Serial.printf("[BME280] Found at 0x%02X\n", BME280_ADDR_PRIMARY);
        return true;
    }
    if (bme.begin(BME280_ADDR_FALLBACK, &Wire)) {
        Serial.printf("[BME280] Found at 0x%02X (fallback)\n", BME280_ADDR_FALLBACK);
        return true;
    }
    Serial.println(F("[BME280] ERROR: Sensor not detected. "
                     "Check wiring — SDA=GPIO21, SCL=GPIO22, CS→3.3V"));
    return false;
}

/* ─── Helper: Initialise Rain Sensor ────────────────────────────────────── */
bool initRainSensor() {
    /*
     * The SEN0575 internal MCU needs ~2 s after power-on before it can
     * handle full register-level I2C commands.  Without this delay,
     * begin() fails even though the I2C address ACK succeeds.
     */
    Serial.println(F("[RAIN] Waiting 2 s for sensor power-up…"));
    delay(2000);

    uint8_t retries = 10;
    while (retries--) {
        // begin() returns true on success, false on failure
        if (rainSensor.begin()) {
            Serial.print(F("[RAIN] SEN0575 initialised  |  FW: "));
            Serial.println(rainSensor.getFirmwareVersion());
            return true;
        }
        Serial.printf("[RAIN] Init attempt failed, %u retries left\n", retries);
        delay(1000);
    }
    Serial.println(F("[RAIN] ERROR: SEN0575 not detected. "
                     "Check I2C wiring (+→3.3V, D/T→GPIO21, C/R→GPIO22)."));
    return false;
}

/* ─── Helper: Start Wind Measurement Window ─────────────────────────────── */
void startWindSample() {
    windPulseCount = 0;
    attachInterrupt(digitalPinToInterrupt(PIN_ANEMOMETER), onWindPulse, FALLING);
    windSampleStart = millis();
    windSampling = true;
}

/* ─── Helper: Finish Wind Measurement & Read All Sensors ────────────────── */
void finishSampling() {
    // ── Wind speed ──
    detachInterrupt(digitalPinToInterrupt(PIN_ANEMOMETER));
    uint32_t pulses = windPulseCount;
    unsigned long elapsed = millis() - windSampleStart;
    windSampling = false;

    if (elapsed > 0) {
        float freqHz = (float)pulses / (elapsed / 1000.0f);
        windSpeed = freqHz * MPS_PER_HZ;
    } else {
        windSpeed = 0.0f;
    }
    Serial.printf("[WIND]  Pulses: %lu  |  Window: %lu ms  |  Speed: %.2f m/s\n",
                  (unsigned long)pulses, elapsed, windSpeed);

    // ── BME280 ──
    if (bmeOk) {
        temperature = bme.readTemperature();       // °C
        humidity    = bme.readHumidity();           // %RH
        pressure    = bme.readPressure() / 100.0f;  // Pa → hPa

        Serial.printf("[BME280] Temp: %.2f °C  |  Hum: %.1f %%  |  "
                      "Press: %.2f hPa\n",
                      temperature, humidity, pressure);
    } else {
        Serial.println(F("[BME280] Skipped — sensor unavailable."));
    }

    // ── Rainfall (using getRawData — proven reliable) ──
    if (rainOk) {
        uint32_t currentTips = rainSensor.getRawData();
        if (currentTips >= rainBaselineCount) {
            rainfall = (currentTips - rainBaselineCount) * MM_PER_TIP;
        }
        Serial.printf("[RAIN]  Tips: %lu (since boot)  |  Rain: %.2f mm\n",
                      (unsigned long)(currentTips - rainBaselineCount), rainfall);
    } else {
        Serial.println(F("[RAIN] Skipped — sensor unavailable."));
    }
}

/*******************************************************************************
 *                                 SETUP
 ******************************************************************************/
void setup() {
    Serial.begin(115200);
    delay(100);

    Serial.println(F("\n════════════════════════════════════════════════"));
    Serial.println(F("  AI-RIS Weather Station — CONTINUOUS MODE"));
    Serial.printf("  Station ID: %s\n", STATION_ID);
    Serial.println(F("════════════════════════════════════════════════"));
    if (strncmp(STATION_ID, "weather-", 8) != 0) {
        Serial.println(F("ERROR: Set STATION_ID to \"weather-a\" or \"weather-b\" in secrets.h before flashing!"));
    }

    /* ── I2C & Sensors ───────────────────────────────────────────────────── */
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(100000);

    bmeOk  = initBME280();
    rainOk = initRainSensor();

    // Capture the starting tip count so we only track NEW rainfall
    if (rainOk) {
        rainBaselineCount = rainSensor.getRawData();
        Serial.printf("[RAIN] Baseline tip count: %lu\n",
                      (unsigned long)rainBaselineCount);
    }

    /* ── Anemometer GPIO ─────────────────────────────────────────────────── */
    pinMode(PIN_ANEMOMETER, INPUT_PULLUP);

    /* ── Wi-Fi & Blynk ───────────────────────────────────────────────────── */
    Serial.println(F("[NET]  Connecting to Wi-Fi…"));
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart) < 15000UL) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[NET]  Wi-Fi OK  |  IP: %s  |  RSSI: %d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        Serial.println(F("[NET]  WARNING: Wi-Fi failed — will retry in loop."));
    }

    Blynk.config(BLYNK_AUTH_TOKEN);
    Blynk.connect(8000);

    if (Blynk.connected()) {
        Serial.println(F("[NET]  Blynk connected."));
    } else {
        Serial.println(F("[NET]  Blynk not connected — will retry in loop."));
    }

    firebaseSyncTime();

    Serial.println(F("[SYS]  Initialisation complete. Entering main loop.\n"));

    /* ── Kick off the first wind sample immediately ──────────────────────── */
    startWindSample();
    lastPublishTime = millis();
}

/*******************************************************************************
 *                                  LOOP
 * Runs continuously.  Non-blocking wind sampling overlaps with Blynk
 * housekeeping.  Every PUBLISH_INTERVAL_MS the sensors are read and
 * telemetry is pushed to the Blynk dashboard.
 ******************************************************************************/
void loop() {

    /* ── Keep Blynk alive (handles server heartbeat & reconnection) ───── */
    Blynk.run();

    /* ── Check if the wind sampling window has elapsed ───────────────────── */
    if (windSampling && (millis() - windSampleStart) >= WIND_SAMPLE_MS) {
        finishSampling();
        readyToPublish = true;
    }

    /* ── Publish telemetry at the defined interval ───────────────────────── */
    if (readyToPublish && (millis() - lastPublishTime) >= PUBLISH_INTERVAL_MS) {
        readyToPublish = false;
        lastPublishTime = millis();

        Serial.println(F("────────────────────────────────────────────────"));
        Serial.printf("  Temp: %.2f °C  |  Hum: %.1f %%  |  "
                      "Press: %.2f hPa\n",
                      temperature, humidity, pressure);
        Serial.printf("  Rain: %.2f mm  |  Wind: %.2f m/s\n",
                      rainfall, windSpeed);
        Serial.println(F("────────────────────────────────────────────────"));

        if (Blynk.connected()) {
            Blynk.virtualWrite(VPIN_TEMPERATURE, temperature);
            Blynk.virtualWrite(VPIN_HUMIDITY,    humidity);
            Blynk.virtualWrite(VPIN_PRESSURE,    pressure);
            Blynk.virtualWrite(VPIN_RAINFALL,    rainfall);
            Blynk.virtualWrite(VPIN_WINDSPEED,   windSpeed);
            Serial.println(F("[NET]  Data pushed to Blynk (V0–V4)."));
        } else {
            Serial.println(F("[NET]  Blynk offline — skipping upload, retrying…"));
            Blynk.connect(4000);  // Quick reconnect attempt
        }

        /* ── Dual-publish to Firebase (latest + history every ~15 s) ─────── */
        {
            unsigned long ts = firebaseUnixTime();
            char pathLatest[64];
            char pathHist[80];
            snprintf(pathLatest, sizeof(pathLatest), "/airis/weather/%s", STATION_ID);
            snprintf(pathHist, sizeof(pathHist), "/airis/history/weather/%s", STATION_ID);

            char json[320];
            snprintf(json, sizeof(json),
                     "{\"stationId\":\"%s\",\"temperature\":%.2f,\"humidity\":%.1f,"
                     "\"pressure\":%.2f,\"rainfall\":%.2f,\"windSpeed\":%.2f,\"updatedAt\":%lu}",
                     STATION_ID, temperature, humidity, pressure, rainfall, windSpeed, ts);
            firebasePutLatest(pathLatest, json);

            char hist[320];
            snprintf(hist, sizeof(hist),
                     "{\"stationId\":\"%s\",\"temperature\":%.2f,\"humidity\":%.1f,"
                     "\"pressure\":%.2f,\"rainfall\":%.2f,\"windSpeed\":%.2f,\"ts\":%lu}",
                     STATION_ID, temperature, humidity, pressure, rainfall, windSpeed, ts);
            firebasePostHistory(pathHist, hist);
        }

        /* ── Start the next wind sampling window ─────────────────────────── */
        startWindSample();
    }
}
