/*
 * AIRIS AIR QUALITY - ESP32 Blynk IoT Connected Air Quality Station
 * 
 * Hardware:
 *  - ESP32 Development Board
 *  - Sharp GP2Y1010AU0F Optical Dust Sensor
 *  - MQ-135 Gas & Air Quality Sensor
 * 
 * Pin Configuration:
 *  - GP2Y1010AU0F LED Trigger : GPIO 2  (D2)
 *  - GP2Y1010AU0F Vo (Analog) : GPIO 33 (ADC1_CH5 - Direct 3.3V connection)
 *  - MQ-135 Analog Out (AOUT) : GPIO 35 (ADC1_CH7 - via 10k/20k voltage divider)
 * 
 * Blynk Virtual Pin Mapping:
 *  - V11 : Air Pollution Reading from MQ-135 (Raw ADC / Pollution Index)
 *  - V13 : Dust Density from GP2Y Sharp Sensor (ug/m3)
 *  - V15 : CO2 Estimated (ppm)
 *  - V16 : NH3 (Ammonia) Estimated (ppm)
 *  - V17 : Benzene Estimated (ppm)
 *  - V18 : Alcohol Estimated (ppm)
 *  - V19 : Toluene Estimated (ppm)
 *  - V20 : Acetone Estimated (ppm)
 */

// --- Blynk Template & Auth Credentials ---
#define BLYNK_TEMPLATE_ID   "TMPL65_YHc2Ka"
#define BLYNK_TEMPLATE_NAME "AIRIS AIR QUALITY"
#define BLYNK_AUTH_TOKEN    "n9cYZkCacvmtfR7nuaZnLBR-_fMtr84Y"

#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>

// --- WiFi Credentials ---
char ssid[] = "DLink2G-137E_R7";
char pass[] = "admin_R7";

// --- Power Saving WiFi Configuration ---
// Default ESP32 Tx power is WIFI_POWER_19_5dBm (~240mA peak).
// Lowering to WIFI_POWER_11dBm (or 8_5dBm) reduces peak transmission spikes by ~40-50%
// while maintaining strong connection range (~15-20 meters through walls).
#define WIFI_TX_POWER   WIFI_POWER_11dBm

// --- Hardware Pins ---
#define PIN_DUST_LED    2     // D2: GP2Y1010 internal IR LED pulse trigger
#define PIN_DUST_VO     33    // GPIO 33: GP2Y1010 analog output
#define PIN_MQ135_AO    35    // GPIO 35: MQ-135 analog output via 10k/20k divider

// --- Sharp GP2Y1010AU0F Timing Parameters (Datasheet & ESP32 optimized) ---
// ESP32 analogRead() takes ~50-60us, so sampling at 260us delay aligns the ADC read
// right at the 280us pulse peak (pulse width total = 320us)
const int SAMPLING_TIME_US = 260;
const int DELTA_TIME_US    = 40;
const int SLEEP_TIME_US    = 9680;

// --- ESP32 ADC Parameters (12-bit: 0 - 4095) ---
const float ADC_REF_VOLTAGE = 3.3;
const float ADC_RESOLUTION  = 4095.0;

// --- MQ-135 Hardware & Calculation Parameters ---
// Hardware divider scaling factor: (10k + 20k) / 20k = 1.5
const float MQ135_DIVIDER_RATIO = 1.5;
const float MQ135_VC            = 5.0;   // MQ-135 operating voltage (5V)
const float MQ135_RL            = 10.0;  // Load resistance in kOhms on breakout module
const float MQ135_CLEAN_AIR_RATIO = 3.6; // Rs / Ro ratio in fresh clean air

// Ro baseline resistance in clean air
float mq135_Ro = 20.0;

// --- GP2Y1010AU0F Dust Calibration Parameters ---
// Sharp GP2Y1010AU0F: Sensitivity = 0.5V per 100 ug/m3 => multiplier = 200.0
// Calibrated to match your sensor's clean air resting Vo (~0.27V)
const float DUST_NOISE_OFFSET = 0.25;    // Baseline clean-air voltage offset
const float DUST_CONV_COEFF   = 200.0;   // Converts (V - Offset) directly to ug/m3

// --- MQ-135 Gas Power-law Regression Coefficients (PPM = a * (Rs/Ro)^b) ---
// Derived from standard MQ-135 datasheet sensitivity curves
const float CO2_A      = 110.47, CO2_B      = -2.862;
const float NH3_A      = 102.20, NH3_B      = -2.473;
const float BENZENE_A  = 44.947, BENZENE_B  = -3.445;
const float ALCOHOL_A  = 77.255, ALCOHOL_B  = -3.180;
const float TOLUENE_A  = 44.883, TOLUENE_B  = -3.480;
const float ACETONE_A  = 34.668, ACETONE_B  = -3.369;

// --- Blynk Timer ---
BlynkTimer timer;
const unsigned long SEND_INTERVAL_MS = 2000; // Update Blynk & Serial every 2 seconds

// Function to calculate gas PPM using power law: PPM = a * (Ratio)^b
float calculatePPM(float ratio, float a, float b) {
  if (ratio <= 0.0) return 0.0;
  return a * pow(ratio, b);
}

// Function to pulse the IR LED and take an oversampled dust reading
float readDustSensorVo() {
  const int SAMPLES = 20;
  long totalRaw = 0;

  for (int i = 0; i < SAMPLES; i++) {
    digitalWrite(PIN_DUST_LED, LOW); // Active-LOW: Turn LED ON
    delayMicroseconds(SAMPLING_TIME_US);

    totalRaw += analogRead(PIN_DUST_VO);
    delayMicroseconds(DELTA_TIME_US);

    digitalWrite(PIN_DUST_LED, HIGH); // Turn LED OFF
    delayMicroseconds(SLEEP_TIME_US);
  }

  float avgRaw = (float)totalRaw / SAMPLES;
  return (avgRaw / ADC_RESOLUTION) * ADC_REF_VOLTAGE;
}

// Function to take an averaged reading from the MQ-135
float readMQ135Voltage(int &rawOut) {
  const int SAMPLES = 20;
  long totalRaw = 0;

  for (int i = 0; i < SAMPLES; i++) {
    totalRaw += analogRead(PIN_MQ135_AO);
    delay(5);
  }

  rawOut = totalRaw / SAMPLES;
  float pinVoltage = ((float)rawOut / ADC_RESOLUTION) * ADC_REF_VOLTAGE;
  // Multiply by 1.5 to recover actual module output voltage before divider
  return pinVoltage * MQ135_DIVIDER_RATIO;
}

// Calculates True Air Quality Index (AQI 0 - 500) from MQ-135 ratio (Rs / Ro)
// Clean air: ratio ~3.6 -> AQI 15 - 35 (Good)
// Polluted air: ratio drops towards 0.5 -> AQI rises up to 500 (Hazardous)
int calculateMQ135AQI(float ratio) {
  if (ratio >= 3.6) {
    // 0 - 50: Good (Fresh / Clean Room Air)
    float aqi = 15.0 + (3.6 / ratio) * 20.0;
    return (int)constrain(aqi, 10, 45);
  } else if (ratio >= 2.2) {
    // 51 - 100: Moderate
    float aqi = 50.0 + ((3.6 - ratio) / (3.6 - 2.2)) * 50.0;
    return (int)constrain(aqi, 51, 100);
  } else if (ratio >= 1.5) {
    // 101 - 150: Unhealthy for Sensitive Groups
    float aqi = 101.0 + ((2.2 - ratio) / (2.2 - 1.5)) * 49.0;
    return (int)constrain(aqi, 101, 150);
  } else if (ratio >= 1.0) {
    // 151 - 200: Unhealthy
    float aqi = 151.0 + ((1.5 - ratio) / (1.5 - 1.0)) * 49.0;
    return (int)constrain(aqi, 151, 200);
  } else if (ratio >= 0.6) {
    // 201 - 300: Very Unhealthy
    float aqi = 201.0 + ((1.0 - ratio) / (1.0 - 0.6)) * 99.0;
    return (int)constrain(aqi, 201, 300);
  } else {
    // 301 - 500: Hazardous
    float aqi = 301.0 + ((0.6 - ratio) / 0.6) * 199.0;
    return (int)constrain(aqi, 301, 500);
  }
}

// Quick clean-air baseline calibration for Ro (only accurate when heater is warm!)
void calibrateMQ135() {
  Serial.print("Checking MQ-135 baseline in clean air...");
  int rawDummy = 0;
  float totalVoltage = 0.0;
  const int CALIB_SAMPLES = 25;

  for (int i = 0; i < CALIB_SAMPLES; i++) {
    totalVoltage += readMQ135Voltage(rawDummy);
    delay(50);
  }

  float avgVout = totalVoltage / CALIB_SAMPLES;
  // If sensor output is within typical warm operating voltage (0.4V - 2.5V)
  if (avgVout > 0.3 && avgVout < 3.0) {
    float rs_clean = MQ135_RL * (MQ135_VC - avgVout) / avgVout;
    mq135_Ro = rs_clean / MQ135_CLEAN_AIR_RATIO;
    Serial.print(" Done! Calibrated Ro = ");
    Serial.print(mq135_Ro, 2);
    Serial.println(" kOhm");
  } else {
    mq135_Ro = 20.0; // Reliable fixed default for standard MQ-135 breakout board
    Serial.println(" Sensor still warming up, using default Ro = 20.0 kOhm");
  }
}

// Main periodic function called by BlynkTimer
void sendSensorReadings() {
  // 1. Read GP2Y1010AU0F Dust Sensor
  float dustVo = readDustSensorVo();
  float dustDensity = 0.0;
  if (dustVo > DUST_NOISE_OFFSET) {
    dustDensity = (dustVo - DUST_NOISE_OFFSET) * DUST_CONV_COEFF;
  }

  // 2. Read MQ-135 Gas Sensor
  int mq135Raw = 0;
  float mq135ActualVoltage = readMQ135Voltage(mq135Raw);

  // Clamp sensor voltage to avoid division by zero or unrealistic spikes
  if (mq135ActualVoltage < 0.05) mq135ActualVoltage = 0.05;
  if (mq135ActualVoltage > 4.95) mq135ActualVoltage = 4.95;

  // Calculate sensor resistance Rs and ratio (Rs / Ro)
  float rs = MQ135_RL * (MQ135_VC - mq135ActualVoltage) / mq135ActualVoltage;
  float ratio = rs / mq135_Ro;

  // Calculate true Air Quality Index (AQI 0 - 500)
  int aqiMQ135 = calculateMQ135AQI(ratio);

  // 3. Compute Individual Gas Concentrations (PPM)
  // Note: 400.0 is the natural atmospheric background CO2 level on Earth
  float ppmCO2     = calculatePPM(ratio, CO2_A, CO2_B) + 400.0;
  float ppmNH3     = calculatePPM(ratio, NH3_A, NH3_B);
  float ppmBenzene = calculatePPM(ratio, BENZENE_A, BENZENE_B);
  float ppmAlcohol = calculatePPM(ratio, ALCOHOL_A, ALCOHOL_B);
  float ppmToluene = calculatePPM(ratio, TOLUENE_A, TOLUENE_B);
  float ppmAcetone = calculatePPM(ratio, ACETONE_A, ACETONE_B);

  // 4. Send Data to Blynk Virtual Pins
  // V11: Converted to actual AQI (0-500 scale). In a clean room, this will now show ~15 - 35 (Good)!
  Blynk.virtualWrite(V11, aqiMQ135);
  Blynk.virtualWrite(V13, dustDensity);   // V13: Dust density (ug/m3)
  Blynk.virtualWrite(V15, ppmCO2);        // V15: Estimated CO2 (ppm)
  Blynk.virtualWrite(V16, ppmNH3);        // V16: Estimated NH3 (ppm)
  Blynk.virtualWrite(V17, ppmBenzene);    // V17: Benzene (ppm)
  Blynk.virtualWrite(V18, ppmAlcohol);    // V18: Alcohol estimated (ppm)
  Blynk.virtualWrite(V19, ppmToluene);    // V19: Toluene (ppm)
  Blynk.virtualWrite(V20, ppmAcetone);    // V20: Acetone (ppm)

  // 5. Output Diagnostics to Serial Monitor
  Serial.println("=========================================================");
  Serial.print("[Blynk] Data pushed at millis: ");
  Serial.println(millis());

  Serial.printf("[DUST]   Vo: %.3f V | Density: %.1f ug/m3 -> [V13]\n", dustVo, dustDensity);
  Serial.printf("[MQ-135] Raw ADC: %d | Voltage: %.2f V | Ratio: %.2f\n", mq135Raw, mq135ActualVoltage, ratio);
  Serial.printf("[AQI]    Air Quality Index: %d (0-500 scale) -> [V11]\n", aqiMQ135);
  Serial.printf("[GASES]  CO2: %.1f ppm [V15] | NH3: %.2f ppm [V16] | Benzene: %.3f ppm [V17]\n", ppmCO2, ppmNH3, ppmBenzene);
  Serial.printf("         Alcohol: %.2f ppm [V18] | Toluene: %.3f ppm [V19] | Acetone: %.3f ppm [V20]\n", ppmAlcohol, ppmToluene, ppmAcetone);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // GP2Y1010AU0F LED setup
  pinMode(PIN_DUST_LED, OUTPUT);
  digitalWrite(PIN_DUST_LED, HIGH); // Default OFF (active LOW)

  // ADC Attenuation configuration (0 - 3.3V input range)
  analogSetPinAttenuation(PIN_DUST_VO, ADC_11db);
  analogSetPinAttenuation(PIN_MQ135_AO, ADC_11db);

  Serial.println("=========================================================");
  Serial.println(" AIRIS AIR QUALITY - ESP32 Blynk Station Initializing... ");
  Serial.println("=========================================================");

  // Calibrate MQ-135 clean air baseline
  calibrateMQ135();

  // Connect to WiFi and Blynk Cloud
  Serial.println("Connecting to Blynk...");
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  // Lower WiFi Transmit Power & enable Modem Sleep to save power
  WiFi.setTxPower(WIFI_TX_POWER);
  WiFi.setSleep(true); // Modem Sleep: reduces idle WiFi current from ~80mA to ~20-30mA
  Serial.println("Power Saving Active: WiFi Tx Power set to 11 dBm & Modem Sleep enabled.");

  // Setup periodic non-blocking sensor transmission
  timer.setInterval(SEND_INTERVAL_MS, sendSensorReadings);

  Serial.println("System Ready! Streaming data to Blynk Cloud...");
}

void loop() {
  Blynk.run();
  timer.run();
}
