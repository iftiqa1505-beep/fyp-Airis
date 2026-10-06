#pragma once

/*
 * Shared Firebase Realtime Database helpers for AIRIS ESP32 firmware.
 * Requires: WiFi connected, secrets.h with FIREBASE_HOST + FIREBASE_AUTH,
 *           #include <HTTPClient.h>, #include <WiFiClientSecure.h>, <time.h>
 */

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

#ifndef FIREBASE_HOST
#error "Include secrets.h before firebase_publish.h"
#endif

static bool firebaseConfigured() {
  if (strlen(FIREBASE_HOST) == 0) return false;
  if (strstr(FIREBASE_HOST, "YOUR_PROJECT") != nullptr) return false;
  if (strlen(FIREBASE_AUTH) == 0) return false;
  if (strstr(FIREBASE_AUTH, "YOUR_DATABASE") != nullptr) return false;
  return true;
}

static void firebaseSyncTime() {
  // UTC — best for ML export; convert timezone later in Python if needed
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print(F("[FB] Waiting for NTP"));
  for (int i = 0; i < 20; i++) {
    time_t now = time(nullptr);
    if (now > 1700000000) {
      Serial.println(F(" OK"));
      return;
    }
    Serial.print('.');
    delay(500);
  }
  Serial.println(F(" timeout (will still publish; ts may be 0)"));
}

static unsigned long firebaseUnixTime() {
  time_t now = time(nullptr);
  if (now < 1700000000) return 0;
  return (unsigned long)now;
}

static bool firebaseHttpWrite(const char* method, const char* path, const char* jsonBody) {
  if (!firebaseConfigured()) {
    Serial.println(F("[FB] Skipped — set FIREBASE_HOST/AUTH in secrets.h"));
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[FB] Skipped — WiFi down"));
    return false;
  }

  char url[256];
  snprintf(url, sizeof(url), "https://%s%s.json?auth=%s",
           FIREBASE_HOST, path, FIREBASE_AUTH);

  WiFiClientSecure client;
  client.setInsecure();  // FYP simplicity; pin certs for production

  HTTPClient https;
  if (!https.begin(client, url)) {
    Serial.println(F("[FB] begin() failed"));
    return false;
  }

  https.addHeader("Content-Type", "application/json");
  int code;
  if (strcmp(method, "POST") == 0) {
    code = https.POST(jsonBody);
  } else {
    code = https.PUT(jsonBody);
  }

  bool ok = (code >= 200 && code < 300);
  Serial.printf("[FB] %s %s -> HTTP %d\n", method, path, code);
  if (!ok) {
    Serial.println(https.getString());
  }
  https.end();
  return ok;
}

// PUT latest snapshot (dashboard)
static bool firebasePutLatest(const char* path, const char* jsonBody) {
  return firebaseHttpWrite("PUT", path, jsonBody);
}

// POST append history node (ML export) — Firebase assigns push ID
static bool firebasePostHistory(const char* path, const char* jsonBody) {
  return firebaseHttpWrite("POST", path, jsonBody);
}
