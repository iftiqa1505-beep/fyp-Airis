(function () {
  // Freshness uses browser receive time (not ESP clock). Boards publish ~15s.
  const WEATHER_STALE_MS = 90000;
  const AIR_STALE_MS = 90000;

  const WEATHER_METRICS = [
    { key: "temperature", id: "temp", label: "Temp", unit: "°C", digits: 1, ok: [24, 34], warnPad: 3, expected: "Normal 24–34" },
    { key: "humidity", id: "hum", label: "Humidity", unit: "%", digits: 0, ok: [60, 90], warnPad: 8, expected: "Normal 60–90" },
    { key: "pressure", id: "press", label: "Pressure", unit: "hPa", digits: 0, ok: [1005, 1015], warnPad: 8, expected: "Normal 1005–1015" },
    { key: "rainfall", id: "rain", label: "Rain", unit: "mm", digits: 1, ok: [0, 20], warnPad: 30, expected: "Dry ~0 mm" },
    { key: "windSpeed", id: "wind", label: "Wind", unit: "m/s", digits: 1, ok: [0, 5], warnPad: 5, expected: "Normal 0–5" }
  ];

  // Andon secondary row: glanceable air metrics only (VOCs stay in Firebase/history)
  const AIR_METRICS = [
    { key: "dust", id: "dust", label: "Dust", unit: "µg/m³", digits: 0, ok: [0, 12], warnPad: 23, expected: "Normal 0–12" },
    { key: "co2", id: "co2", label: "CO₂", unit: "ppm", digits: 0, ok: [380, 600], warnPad: 400, expected: "Outdoor ~400–450" },
    { key: "nh3", id: "nh3", label: "NH₃", unit: "ppm", digits: 2, ok: [0, 0.1], warnPad: 0.9, expected: "Outdoor &lt; 0.1" },
    { key: "alcohol", id: "alcohol", label: "VOC proxy", unit: "ppm", digits: 2, ok: [0, 0.5], warnPad: 2, expected: "Outdoor ~0" }
  ];

  const state = {
    "weather-a": null,
    "weather-b": null,
    "air-a": null,
    "air-b": null
  };

  function fmt(n, digits) {
    if (n === null || n === undefined || Number.isNaN(Number(n))) return "—";
    return Number(n).toFixed(digits);
  }

  function aqiCategory(aqi) {
    const v = Number(aqi);
    if (Number.isNaN(v)) return "—";
    if (v <= 50) return "Good";
    if (v <= 100) return "Moderate";
    if (v <= 150) return "Unhealthy (sensitive)";
    if (v <= 200) return "Unhealthy";
    if (v <= 300) return "Very unhealthy";
    return "Hazardous";
  }

  function aqiLevel(aqi) {
    const v = Number(aqi);
    if (Number.isNaN(v)) return "wait";
    if (v <= 50) return "ok";
    if (v <= 100) return "warn";
    return "bad";
  }

  function metricLevel(def, value) {
    const v = Number(value);
    if (Number.isNaN(v) || !def.ok) return "wait";
    const [lo, hi] = def.ok;
    if (v >= lo && v <= hi) return "ok";
    const pad = def.warnPad || 0;
    if (v >= lo - pad && v <= hi + pad) return "warn";
    return "bad";
  }

  function worstLevel(levels) {
    if (levels.indexOf("bad") >= 0) return "bad";
    if (levels.indexOf("warn") >= 0) return "warn";
    if (levels.indexOf("ok") >= 0) return "ok";
    return "wait";
  }

  function setBadge(el, stateName, text) {
    if (!el) return;
    el.className = "badge " + stateName;
    el.textContent = text;
  }

  function formatUpdated(epochSec) {
    if (!epochSec) return "Updated —";
    const d = new Date(Number(epochSec) * 1000);
    if (Number.isNaN(d.getTime())) return "Updated —";
    return "Updated " + d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
  }

  function isFresh(data, staleMs) {
    if (!data) return false;
    const receivedAt = Number(data.receivedAtMs);
    if (!receivedAt) return false;
    return Date.now() - receivedAt <= staleMs;
  }

  function freshness(data, staleMs) {
    return isFresh(data, staleMs) ? "live" : "stale";
  }

  function buildMetricCards() {
    document.querySelectorAll(".metrics[data-kind]").forEach((host) => {
      const prefix = host.getAttribute("data-prefix");
      const kind = host.getAttribute("data-kind");
      const defs = kind === "weather" ? WEATHER_METRICS : AIR_METRICS;
      host.innerHTML = defs.map((m) => (
        '<div class="metric" id="' + prefix + "-" + m.id + '-card">' +
          '<span class="label">' + m.label + "</span>" +
          '<div class="value-row">' +
            '<span class="value" id="' + prefix + "-" + m.id + '">—</span>' +
            '<span class="unit">' + m.unit + "</span>" +
          "</div>" +
          '<span class="expected">' + m.expected + "</span>" +
        "</div>"
      )).join("");
    });
  }

  function applyMetricLevels(prefix, defs, data) {
    defs.forEach((m) => {
      const card = document.getElementById(prefix + "-" + m.id + "-card");
      if (!card) return;
      if (!data) {
        card.setAttribute("data-level", "wait");
        return;
      }
      card.setAttribute("data-level", metricLevel(m, data[m.key]));
    });
  }

  function fillWeather(prefix, data, statusId, updatedId, badgeOverride) {
    const status = document.getElementById(statusId);
    const updated = document.getElementById(updatedId);
    if (!data) {
      WEATHER_METRICS.forEach((m) => {
        const el = document.getElementById(prefix + "-" + m.id);
        if (el) el.textContent = "—";
      });
      if (updated) updated.textContent = "Updated —";
      setBadge(status, "error", "OFFLINE");
      applyMetricLevels(prefix, WEATHER_METRICS, null);
      return;
    }
    WEATHER_METRICS.forEach((m) => {
      const el = document.getElementById(prefix + "-" + m.id);
      if (el) el.textContent = fmt(data[m.key], m.digits);
    });
    applyMetricLevels(prefix, WEATHER_METRICS, data);
    if (updated) updated.textContent = formatUpdated(data.updatedAt);
    if (badgeOverride) {
      setBadge(status, badgeOverride.state, badgeOverride.text);
    } else {
      const st = freshness(data, WEATHER_STALE_MS);
      setBadge(status, st, st === "live" ? "LIVE" : "STALE");
    }
  }

  function fillAir(prefix, data, statusId, updatedId, badgeOverride) {
    const status = document.getElementById(statusId);
    const updated = document.getElementById(updatedId);
    const aqiEl = document.getElementById(prefix + "-aqi");
    const aqiLabel = document.getElementById(prefix + "-aqi-label");
    if (!data) {
      if (aqiEl) aqiEl.textContent = "—";
      if (aqiLabel) aqiLabel.textContent = "—";
      AIR_METRICS.forEach((m) => {
        const el = document.getElementById(prefix + "-" + m.id);
        if (el) el.textContent = "—";
      });
      if (updated) updated.textContent = "Updated —";
      setBadge(status, "error", "OFFLINE");
      applyMetricLevels(prefix, AIR_METRICS, null);
      return;
    }
    if (aqiEl) aqiEl.textContent = fmt(data.aqi, 0);
    if (aqiLabel) aqiLabel.textContent = aqiCategory(data.aqi);
    AIR_METRICS.forEach((m) => {
      const el = document.getElementById(prefix + "-" + m.id);
      if (el) el.textContent = fmt(data[m.key], m.digits);
    });
    applyMetricLevels(prefix, AIR_METRICS, data);
    if (updated) updated.textContent = formatUpdated(data.updatedAt);
    if (badgeOverride) {
      setBadge(status, badgeOverride.state, badgeOverride.text);
    } else {
      const st = freshness(data, AIR_STALE_MS);
      setBadge(status, st, st === "live" ? "LIVE" : "STALE");
    }
  }

  function meanFields(rows, keys) {
    if (!rows.length) return null;
    const out = {};
    keys.forEach((k) => {
      let sum = 0;
      let n = 0;
      rows.forEach((r) => {
        const v = Number(r[k]);
        if (!Number.isNaN(v)) {
          sum += v;
          n += 1;
        }
      });
      out[k] = n ? sum / n : null;
    });
    out.updatedAt = Math.max.apply(null, rows.map((r) => Number(r.updatedAt) || 0));
    out.receivedAtMs = Math.max.apply(null, rows.map((r) => Number(r.receivedAtMs) || 0));
    return out;
  }

  function liveRows(ids, staleMs) {
    return ids
      .map((id) => state[id])
      .filter((d) => isFresh(d, staleMs));
  }

  function setPanelLevel(id, level) {
    const el = document.getElementById(id);
    if (el) el.setAttribute("data-level", level);
  }

  function updateSystemBanner(levels) {
    const box = document.getElementById("system-status");
    const text = document.getElementById("system-text");
    const level = worstLevel(levels);
    if (box) box.setAttribute("data-level", level);
    if (!text) return;
    if (level === "ok") text.textContent = "NORMAL";
    else if (level === "warn") text.textContent = "CAUTION";
    else if (level === "bad") text.textContent = "ALERT";
    else text.textContent = "WAITING";
  }

  function tickClock() {
    const el = document.getElementById("andon-clock");
    if (!el) return;
    el.textContent = new Date().toLocaleTimeString([], {
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit"
    });
  }

  function fillStationWeather(prefix, data, statusId, updatedId) {
    const temp = document.getElementById(prefix + "-temp");
    const status = document.getElementById(statusId);
    const updated = document.getElementById(updatedId);
    if (!data) {
      if (temp) temp.textContent = "—";
      if (updated) updated.textContent = "Updated —";
      setBadge(status, "error", "OFFLINE");
      return;
    }
    if (temp) temp.textContent = fmt(data.temperature, 1);
    if (updated) updated.textContent = formatUpdated(data.updatedAt);
    const st = freshness(data, WEATHER_STALE_MS);
    setBadge(status, st, st === "live" ? "LIVE" : "STALE");
  }

  function fillStationAir(prefix, data, statusId, updatedId) {
    const aqi = document.getElementById(prefix + "-aqi");
    const status = document.getElementById(statusId);
    const updated = document.getElementById(updatedId);
    if (!data) {
      if (aqi) aqi.textContent = "—";
      if (updated) updated.textContent = "Updated —";
      setBadge(status, "error", "OFFLINE");
      return;
    }
    if (aqi) aqi.textContent = fmt(data.aqi, 0);
    if (updated) updated.textContent = formatUpdated(data.updatedAt);
    const st = freshness(data, AIR_STALE_MS);
    setBadge(status, st, st === "live" ? "LIVE" : "STALE");
  }

  function renderAll() {
    fillStationWeather("wx-a", state["weather-a"], "wx-a-status", "wx-a-updated");
    fillStationWeather("wx-b", state["weather-b"], "wx-b-status", "wx-b-updated");
    fillStationAir("aq-a", state["air-a"], "aq-a-status", "aq-a-updated");
    fillStationAir("aq-b", state["air-b"], "aq-b-status", "aq-b-updated");

    const levels = [];

    [["weather-a", "wx-a-panel", WEATHER_STALE_MS],
     ["weather-b", "wx-b-panel", WEATHER_STALE_MS]].forEach(([id, panel, stale]) => {
      const d = state[id];
      if (!isFresh(d, stale)) {
        setPanelLevel(panel, "bad");
        levels.push("bad");
      } else {
        setPanelLevel(panel, "ok");
        levels.push("ok");
      }
    });

    [["air-a", "aq-a-panel", AIR_STALE_MS],
     ["air-b", "aq-b-panel", AIR_STALE_MS]].forEach(([id, panel, stale]) => {
      const d = state[id];
      if (!isFresh(d, stale)) {
        setPanelLevel(panel, "bad");
        levels.push("bad");
      } else {
        const lvl = aqiLevel(d.aqi);
        setPanelLevel(panel, lvl === "wait" ? "ok" : lvl);
        levels.push(lvl === "wait" ? "ok" : lvl);
      }
    });

    const wxLive = liveRows(["weather-a", "weather-b"], WEATHER_STALE_MS);
    if (!wxLive.length) {
      fillWeather("compound-wx", null, "compound-wx-status", "compound-wx-updated");
      setBadge(document.getElementById("compound-wx-status"), "error", "NO DATA");
      setPanelLevel("compound-weather-panel", "bad");
      levels.push("bad");
    } else {
      const avg = meanFields(wxLive, ["temperature", "humidity", "pressure", "rainfall", "windSpeed"]);
      const badge = { state: "live", text: wxLive.length === 2 ? "2/2 AVG" : "1/2 AVG" };
      fillWeather("compound-wx", avg, "compound-wx-status", "compound-wx-updated", badge);
      const wxLevels = WEATHER_METRICS.map((m) => metricLevel(m, avg[m.key]));
      const wxWorst = worstLevel(wxLevels);
      setPanelLevel("compound-weather-panel", wxWorst === "wait" ? "ok" : wxWorst);
      levels.push(wxWorst);
    }

    const aqLive = liveRows(["air-a", "air-b"], AIR_STALE_MS);
    if (!aqLive.length) {
      fillAir("compound-aq", null, "compound-aq-status", "compound-aq-updated");
      setBadge(document.getElementById("compound-aq-status"), "error", "NO DATA");
      setPanelLevel("compound-air-panel", "bad");
      levels.push("bad");
    } else {
      const avg = meanFields(aqLive, ["aqi", "dust", "co2", "nh3", "benzene", "alcohol", "toluene", "acetone"]);
      const badge = { state: "live", text: aqLive.length === 2 ? "2/2 AVG" : "1/2 AVG" };
      fillAir("compound-aq", avg, "compound-aq-status", "compound-aq-updated", badge);
      const aqLvl = aqiLevel(avg.aqi);
      setPanelLevel("compound-air-panel", aqLvl);
      levels.push(aqLvl);
      AIR_METRICS.forEach((m) => levels.push(metricLevel(m, avg[m.key])));
    }

    updateSystemBanner(levels);
  }

  function setStation(id, data) {
    if (data && typeof data === "object") {
      data.receivedAtMs = Date.now();
      state[id] = data;
    } else {
      state[id] = null;
    }
    renderAll();
  }

  buildMetricCards();
  tickClock();
  setInterval(tickClock, 1000);
  setInterval(renderAll, 5000);

  if (!window.AIRIS_FIREBASE_CONFIG ||
      String(window.AIRIS_FIREBASE_CONFIG.apiKey || "").includes("YOUR_")) {
    console.warn("Firebase config missing — andon demo preview.");
    const now = Math.floor(Date.now() / 1000);
    const receivedAtMs = Date.now();
    state["weather-a"] = {
      temperature: 28.4, humidity: 72.0, pressure: 1008.2,
      rainfall: 1.4, windSpeed: 1.75, updatedAt: now, receivedAtMs: receivedAtMs
    };
    state["weather-b"] = {
      temperature: 29.1, humidity: 68.5, pressure: 1007.6,
      rainfall: 1.1, windSpeed: 2.10, updatedAt: now, receivedAtMs: receivedAtMs
    };
    state["air-a"] = {
      aqi: 42, dust: 11.5, co2: 612.0, nh3: 0.42, benzene: 0.018,
      alcohol: 0.31, toluene: 0.021, acetone: 0.015, updatedAt: now, receivedAtMs: receivedAtMs
    };
    state["air-b"] = {
      aqi: 48, dust: 14.2, co2: 640.0, nh3: 0.55, benzene: 0.022,
      alcohol: 0.28, toluene: 0.019, acetone: 0.017, updatedAt: now, receivedAtMs: receivedAtMs
    };
    renderAll();
    return;
  }

  firebase.initializeApp(window.AIRIS_FIREBASE_CONFIG);
  const db = firebase.database();

  function bind(path, id) {
    db.ref(path).on("value", (snap) => setStation(id, snap.val()), (err) => {
      console.error(path, err);
      setStation(id, null);
    });
  }

  bind("airis/weather/weather-a", "weather-a");
  bind("airis/weather/weather-b", "weather-b");
  bind("airis/air/air-a", "air-a");
  bind("airis/air/air-b", "air-b");

  if (navigator.wakeLock && navigator.wakeLock.request) {
    navigator.wakeLock.request("screen").catch(function () {});
  }
})();
