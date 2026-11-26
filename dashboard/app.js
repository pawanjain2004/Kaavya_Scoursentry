import { firebaseConfig, DEVICE_ID, HISTORY_POINTS, OFFLINE_AFTER_MS } from "./firebase-config.js";

const FB_VERSION = "10.12.2";
const DEMO = firebaseConfig.apiKey === "YOUR_API_KEY" || new URLSearchParams(location.search).has("demo");

const $ = (id) => document.getElementById(id);
const LEVELS = ["SAFE", "WARNING", "DANGER"];
const COLORS = { green: "#22c55e", yellow: "#facc15", red: "#ef4444", blue: "#38bdf8", violet: "#a78bfa", muted: "#8a99b8" };

const state = {
  live: null,
  liveReceivedAt: 0,
  serverOffset: 0,
  packetCount: 0,
  history: [],
  events: [],
  thresholds: {
    angleSafeLo: 80, angleSafeHi: 100, angleWarnLo: 70, angleWarnHi: 110,
    freqWarn: 20, freqDanger: 50,
    vibWarn: 200, vibDanger: 500,
  },
};

function angleAlarmLevel(angle, t) {
  if (!isNum(angle)) return -1;
  const mag = Math.abs(angle); // +82 and -82 treated the same
  const lo = t.angleWarnLo ?? 70, hi = t.angleWarnHi ?? 110;
  const sLo = t.angleSafeLo ?? 80, sHi = t.angleSafeHi ?? 100;
  if (mag <= lo || mag >= hi) return 2;
  if (mag < sLo || mag > sHi) return 1;
  return 0;
}

function freqAlarmLevel(freq, t) {
  if (!isNum(freq)) return -1;
  if (freq > (t.freqDanger ?? 50)) return 2;
  if (freq > (t.freqWarn ?? 20)) return 1;
  return 0;
}

function setConnCard(cardId, stateId, detailId, mode, stateText, detail) {
  const card = $(cardId);
  card.className = "conn-card " + mode;
  $(stateId).textContent = stateText;
  $(detailId).textContent = detail;
}

function pushFeed(lineHtml) {
  const log = $("feedLog");
  if (log.querySelector(".empty")) log.replaceChildren();
  const li = document.createElement("li");
  li.innerHTML = lineHtml;
  log.prepend(li);
  while (log.children.length > 40) log.removeChild(log.lastChild);
}

// ---------------------------------------------------------------- helpers
const isNum = (v) => typeof v === "number" && Number.isFinite(v);
const fmt = (v, d = 2) => (isNum(v) ? v.toFixed(d) : "--");
const fmtTime = (ms) => new Date(ms).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
const fmtDateTime = (ms) =>
  new Date(ms).toLocaleString([], { day: "2-digit", month: "short", hour: "2-digit", minute: "2-digit", second: "2-digit" });

function fmtUptime(s) {
  if (!isNum(s)) return "--";
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  return d ? `${d}d ${h}h ${m}m` : h ? `${h}h ${m}m` : `${m}m ${Math.floor(s % 60)}s`;
}

function levelOf(value, warn, danger) {
  if (!isNum(value)) return -1;
  return value >= danger ? 2 : value >= warn ? 1 : 0;
}

function setTag(el, level) {
  el.className = "tag" + (level >= 0 ? " " + LEVELS[level].toLowerCase() : "");
  el.textContent = level >= 0 ? LEVELS[level] : "N/A";
}

function setGauge(id, value, warn, danger, lblWarnId, lblDangerId, unit) {
  const g = $(id);
  const max = danger * 1.5;
  const [zg, zy, zr] = g.querySelectorAll(".gz");
  zg.style.width = `${(warn / max) * 100}%`;
  zy.style.width = `${((danger - warn) / max) * 100}%`;
  zr.style.width = `${((max - danger) / max) * 100}%`;
  const pct = isNum(value) ? Math.min(Math.max(value / max, 0), 1) * 100 : 0;
  g.querySelector(".marker").style.left = `${pct}%`;
  g.querySelector(".marker").style.opacity = isNum(value) ? 1 : 0.2;
  const lw = $(lblWarnId), ld = $(lblDangerId);
  lw.style.left = `${(warn / max) * 100}%`;
  ld.style.left = `${(danger / max) * 100}%`;
  lw.textContent = `${warn}${unit}`;
  ld.textContent = `${danger}${unit}`;
}

function setHealth(id, text, cls) {
  const el = $(id);
  el.textContent = text;
  el.className = cls || "";
}

// ---------------------------------------------------------------- charts
Chart.defaults.color = COLORS.muted;
Chart.defaults.borderColor = "rgba(34,48,77,0.6)";
Chart.defaults.font.family = "Inter, system-ui, sans-serif";
Chart.defaults.font.size = 11;

function lineChart(canvasId, datasets, yTitle) {
  return new Chart($(canvasId), {
    type: "line",
    data: { labels: [], datasets },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      interaction: { mode: "index", intersect: false },
      elements: { point: { radius: 0, hitRadius: 6 }, line: { tension: 0.25, borderWidth: 2 } },
      scales: {
        x: { ticks: { maxTicksLimit: 6, maxRotation: 0 } },
        y: { title: { display: !!yTitle, text: yTitle } },
      },
      plugins: { legend: { labels: { boxWidth: 10, boxHeight: 10 } } },
    },
  });
}

const thrLine = (label, color) => ({ label, data: [], borderColor: color, borderDash: [6, 5], borderWidth: 1.5, pointRadius: 0, fill: false });

const tiltChart = lineChart("tiltChart", [
  { label: "Angle", data: [], borderColor: COLORS.blue, backgroundColor: "rgba(56,189,248,0.12)", fill: true },
  thrLine("Warning", COLORS.yellow),
  thrLine("Danger", COLORS.red),
], "deg");

const freqChart = lineChart("freqChart", [
  { label: "Natural freq.", data: [], borderColor: COLORS.green, backgroundColor: "rgba(34,197,94,0.1)", fill: true, spanGaps: true },
  thrLine("Baseline", COLORS.muted),
  thrLine("Warning", COLORS.yellow),
  thrLine("Danger", COLORS.red),
], "Hz");

const vibChart = lineChart("vibChart", [
  { label: "Vibration RMS", data: [], borderColor: COLORS.violet, backgroundColor: "rgba(167,139,250,0.12)", fill: true },
  thrLine("Warning", COLORS.yellow),
  thrLine("Danger", COLORS.red),
], "mg");

const specChart = new Chart($("specChart"), {
  type: "line",
  data: { labels: [], datasets: [{ label: "Amplitude (mg)", data: [], borderColor: COLORS.blue, backgroundColor: "rgba(56,189,248,0.18)", fill: true, tension: 0.2, pointRadius: [], pointBackgroundColor: COLORS.yellow }] },
  options: {
    responsive: true,
    maintainAspectRatio: false,
    animation: { duration: 300 },
    plugins: { legend: { display: false } },
    scales: {
      x: { title: { display: true, text: "Frequency (Hz)" }, ticks: { maxTicksLimit: 12 } },
      y: { title: { display: true, text: "mg" }, beginAtZero: true },
    },
  },
});

// ---------------------------------------------------------------- render: live
function renderLive(live) {
  if (!live) return;
  state.live = live;
  state.liveReceivedAt = Date.now();
  state.packetCount += 1;
  if (live.thresholds) state.thresholds = { ...state.thresholds, ...live.thresholds };
  const t = state.thresholds;
  const mpuOk = live.mpuConnected === true || live.sensorOk === true;
  const sdOk = live.sdConnected === true || live.sdOk === true;
  const angle = readingAngle(live);
  const angleCal = live.angleCal === true || live.tiltCal === true;
  const hasReadings = live.hasReadings === true || (mpuOk && isNum(angle));
  const fWarn = t.freqWarn ?? t.freqDropWarn ?? 20;
  const fDanger = t.freqDanger ?? t.freqDropDanger ?? 50;
  const vWarn = t.vibWarn ?? 200;
  const vDanger = t.vibDanger ?? 500;
  const aWarnLo = t.angleWarnLo ?? 80;
  const aWarnHi = t.angleWarnHi ?? 100;

  // connection cards
  setConnCard("cardEsp", "espState", "espDetail", "ok", "ONLINE",
    isNum(live.ts) ? `Packet at ${fmtDateTime(live.ts)}` : "Receiving live packets");
  setConnCard("cardMpu", "mpuState", "mpuDetail",
    mpuOk ? "ok" : "bad",
    mpuOk ? "CONNECTED" : "NOT CONNECTED",
    mpuOk ? "MPU6050 responding on I2C" : "Wire MPU6050: SDA 21, SCL 22, VCC, GND");
  setConnCard("cardSd", "sdState", "sdDetail",
    sdOk ? "ok" : "bad",
    sdOk ? "CONNECTED" : "NOT CONNECTED",
    sdOk ? "Logging to SD card" : "Insert SD card (CS=GPIO5) or ignore if unused");
  const rssi = live.rssi;
  setConnCard("cardWifi", "wifiState", "wifiDetail",
    isNum(rssi) ? (rssi > -80 ? "ok" : "bad") : "wait",
    isNum(rssi) ? "CONNECTED" : "UNKNOWN",
    isNum(rssi) ? `RSSI ${rssi} dBm · uptime ${fmtUptime(live.uptime)}` : "No RSSI in packet");

  // live feed
  $("feedCount").textContent = String(state.packetCount);
  $("feedLastTime").textContent = isNum(live.ts) ? fmtDateTime(live.ts) : fmtDateTime(Date.now());
  $("feedNote").textContent = live.dataNote || (mpuOk ? "Live data" : "MPU6050 not connected");
  const feedCls = mpuOk ? "ok" : "bad";
  pushFeed(
    `<span class="t">${fmtTime(isNum(live.ts) ? live.ts : Date.now())}</span>` +
    `<span class="${feedCls}">MPU ${mpuOk ? "CONNECTED" : "NOT CONNECTED"}</span>` +
    ` · SD ${sdOk ? "OK" : "NO"}` +
    ` · status ${live.status || LEVELS[isNum(live.level) ? live.level : 0]}` +
    (hasReadings ? ` · angle ${fmt(angle, 2)}°` : " · no sensor readings") +
    (isNum(live.freq) ? ` · f ${fmt(live.freq, 2)} Hz` : "") +
    (isNum(live.vibRms) ? ` · vib ${fmt(live.vibRms, 1)} mg` : "")
  );

  // status banner
  const level = isNum(live.level) ? live.level : 0;
  const alerts = (live.alerts || "").split(",").filter(Boolean);
  renderStatus(level, alerts, live);

  // LEDs + buzzer (mirror of the firmware output logic)
  const ledR = $("ledRed"), ledY = $("ledYellow"), ledG = $("ledGreen");
  [ledR, ledY, ledG].forEach((l) => l.classList.remove("on", "blink"));
  if (!mpuOk) ledY.classList.add("on", "blink");
  else if (level >= 2) ledR.classList.add("on");
  else if (level === 1) ledY.classList.add("on");
  else ledG.classList.add("on");
  if (live.calibrating) ledG.classList.add("on", "blink");

  const bz = $("buzzerBox");
  bz.className = "buzzer" + (live.buzzer ? " on" : live.muted && level > 0 ? " muted" : "");
  $("buzzerText").textContent = live.buzzer ? (level >= 2 ? "Buzzer: alarm" : "Buzzer: chirping") : live.muted && level > 0 ? "Buzzer muted" : "Buzzer off";

  // angle / freq / vib — show NO SENSOR when MPU is missing
  if (!mpuOk) {
    $("tiltValue").textContent = "N/A";
    setTag($("tiltTag"), -1);
    $("tiltTag").textContent = "NO SENSOR";
    setGauge("tiltGauge", null, 5, 10, "tiltWarnLbl", "tiltDangerLbl", "°");

    $("freqValue").textContent = "N/A";
    setTag($("freqTag"), -1);
    $("freqTag").textContent = "NO SENSOR";
    setGauge("freqGauge", null, fWarn, fDanger, "freqWarnLbl", "freqDangerLbl", "");

    $("vibValue").textContent = "N/A";
    if ($("vibPeakValue")) $("vibPeakValue").textContent = "N/A";
    setTag($("vibTag"), -1);
    $("vibTag").textContent = "NO SENSOR";
    setGauge("vibGauge", null, vWarn, vDanger, "vibWarnLbl", "vibDangerLbl", "");
  } else {
    $("tiltValue").textContent = fmt(angle, 2);
    const aLvl = angleCal ? angleAlarmLevel(angle, t) : -1;
    setTag($("tiltTag"), aLvl);
    // Gauge uses ||angle|-90|: safe ≤10, warn ≤20, danger >20 (+/- tilt same)
    const from90 = isNum(angle) ? Math.abs(Math.abs(angle) - 90) : null;
    setGauge("tiltGauge", from90, 10, 20, "tiltWarnLbl", "tiltDangerLbl", "°");
    $("tiltWarnLbl").textContent = "80–100";
    $("tiltDangerLbl").textContent = "≤70/≥110";

    $("freqValue").textContent = isNum(live.freq) ? `${fmt(live.freq, 2)}${live.freqValid ? "" : "?"}` : "--";
    const fLvl = isNum(live.freq) ? freqAlarmLevel(live.freq, t) : -1;
    setTag($("freqTag"), fLvl);
    setGauge("freqGauge", live.freq, fWarn, fDanger, "freqWarnLbl", "freqDangerLbl", "");
    $("freqWarnLbl").textContent = `${fWarn} Hz`;
    $("freqDangerLbl").textContent = `${fDanger} Hz`;

    $("vibValue").textContent = fmt(live.vibRms, 1);
    if ($("vibPeakValue")) $("vibPeakValue").textContent = fmt(live.vibPeak, 1);
    setTag($("vibTag"), isNum(live.vibRms) ? levelOf(live.vibRms, vWarn, vDanger) : -1);
    setGauge("vibGauge", live.vibRms, vWarn, vDanger, "vibWarnLbl", "vibDangerLbl", "");
  }

  // health
  setHealth("hSensor", mpuOk ? "CONNECTED" : "NOT CONNECTED", mpuOk ? "ok" : "bad");
  setHealth("hSd", sdOk ? "CONNECTED" : "NOT CONNECTED", sdOk ? "ok" : "bad");
  setHealth("hRssi", isNum(rssi) ? `${rssi} dBm` : "--", !isNum(rssi) ? "" : rssi > -67 ? "ok" : rssi > -80 ? "warn" : "bad");
  setHealth("hTiltCal", !mpuOk ? "N/A" : live.calibrating && !angleCal ? "Do Z then N" : angleCal ? "Saved" : "Not set", !mpuOk ? "" : angleCal ? "ok" : "warn");
  setHealth("hFreqCal", !mpuOk ? "N/A" : live.freqCal ? `${fmt(live.baselineFreq, 2)} Hz` : "Send F on device", !mpuOk ? "" : live.freqCal ? "ok" : "warn");
  setHealth("hFs", isNum(live.sampleRate) ? `${live.sampleRate.toFixed(1)} Hz` : "N/A");
  setHealth("hUptime", fmtUptime(live.uptime));

  // pier visual
  $("pTilt").textContent = mpuOk ? `${fmt(angle, 2)}°` : "N/A";
  $("pRoll").textContent = mpuOk && isNum(angle) ? `${fmt(Math.abs(angle - 90), 2)}° from 90` : "N/A";
  $("pPitch").textContent = mpuOk && isNum(live.freq)
    ? `${fmt(live.freq, 2)} Hz${live.freqValid ? "" : "?"}`
    : mpuOk ? "waiting..." : "N/A";
  $("pRollRaw").textContent = mpuOk ? `${fmt(live.vibRms, 1)} mg` : "N/A";
  $("pPitchRaw").textContent = mpuOk ? `${fmt(live.vibPeak, 1)} mg` : "N/A";
  const vis = mpuOk && isNum(angle) ? Math.max(-15, Math.min(15, (angle - 90) * 2)) : 0;
  $("pierGroup").style.transform = `rotate(${vis}deg)`;
  const pierColor = !mpuOk ? COLORS.muted : level >= 2 ? COLORS.red : level === 1 ? COLORS.yellow : "#94a3b8";
  document.querySelector("#pierGroup .pier").style.stroke = pierColor;

  // thresholds table
  $("thTiltW").textContent = `${aWarnLo}–${t.angleSafeLo ?? 80}° / ${t.angleSafeHi ?? 100}–${aWarnHi}°`;
  $("thTiltD").textContent = `≤${aWarnLo}° or ≥${aWarnHi}°`;
  $("thFreqW").textContent = `>${fWarn}–${fDanger} Hz`;
  $("thFreqD").textContent = `>${fDanger} Hz`;
  $("thVibW").textContent = `${vWarn}–${vDanger} mg`;
  $("thVibD").textContent = `≥${vDanger} mg`;

  updateHistoryCharts();
  updateConnection();
}

const STATUS_MSG = {
  0: "Angle 80–100°, frequency ≤20 Hz, vibration <200 mg. All clear.",
  1: "Warning band reached (yellow + two beeps). Take care and prepare if it worsens.",
  2: "Danger threshold exceeded (red + continuous beep). Evacuate immediately.",
};
const ALERT_NAMES = {
  ANGLE: "Angle exceeded",
  TILT: "Angle exceeded",
  FREQ: "Natural frequency drop",
  VIB: "High vibration",
  SENSOR: "MPU6050 not connected",
};

function readingAngle(live) {
  if (isNum(live.angle)) return live.angle;
  if (isNum(live.tilt)) return live.tilt;
  return null;
}

function renderStatus(level, alerts, live) {
  const offline = isOffline();
  const mpuOk = live && (live.mpuConnected === true || live.sensorOk === true);
  const banner = $("statusBanner");

  if (offline) {
    banner.className = "status-banner level-offline";
    $("statusText").textContent = "DEVICE OFFLINE";
    $("statusMsg").textContent = "No Firebase live update in the last 15 seconds. ESP32 may be powered off or WiFi lost.";
  } else if (live && !mpuOk) {
    banner.className = "status-banner level-sensor";
    $("statusText").textContent = "SENSOR NOT CONNECTED";
    $("statusMsg").textContent = live.dataNote || "ESP32 is online and sending to Firebase, but MPU6050 is not connected. Tilt/frequency values are unavailable.";
  } else {
    banner.className = "status-banner " + `level-${LEVELS[level].toLowerCase()}`;
    $("statusText").textContent = LEVELS[level];
    let msg = STATUS_MSG[level];
    if (live && live.calibrating) msg += " (Calibration in progress.)";
    $("statusMsg").textContent = msg;
  }

  const chips = $("alertChips");
  chips.replaceChildren(
    ...alerts.map((a) => {
      const c = document.createElement("span");
      c.className = "chip";
      c.textContent = ALERT_NAMES[a] || a;
      return c;
    })
  );
}

// ---------------------------------------------------------------- render: history
function addHistory(point) {
  if (!point || !isNum(point.ts)) return;
  state.history.push(point);
  if (state.history.length > HISTORY_POINTS) state.history.splice(0, state.history.length - HISTORY_POINTS);
  scheduleHistoryUpdate();
}

let historyTimer = null;
function scheduleHistoryUpdate() {
  if (historyTimer) return;
  historyTimer = setTimeout(() => {
    historyTimer = null;
    updateHistoryCharts();
  }, 100);
}

function updateHistoryCharts() {
  const h = state.history;
  const t = state.thresholds;
  const labels = h.map((p) => fmtTime(p.ts));
  const val = (v) => (isNum(v) ? v : null);
  const flat = (v) => h.map(() => v);

  tiltChart.data.labels = labels;
  tiltChart.data.datasets[0].data = h.map((p) => val(isNum(p.angle) ? p.angle : p.tilt));
  tiltChart.data.datasets[1].data = flat(t.angleSafeLo ?? 80);
  tiltChart.data.datasets[2].data = flat(t.angleWarnLo ?? 70);
  tiltChart.update();

  const fW = t.freqWarn ?? t.freqDropWarn ?? 20;
  const fD = t.freqDanger ?? t.freqDropDanger ?? 50;
  freqChart.data.labels = labels;
  freqChart.data.datasets[0].data = h.map((p) => val(p.freq));
  freqChart.data.datasets[1].data = flat(fW);
  freqChart.data.datasets[2].data = flat(fW);
  freqChart.data.datasets[3].data = flat(fD);
  if (freqChart.data.datasets[1]) freqChart.data.datasets[1].label = "Warn 20 Hz";
  if (freqChart.data.datasets[3]) freqChart.data.datasets[3].label = "Danger 50 Hz";
  freqChart.update();

  vibChart.data.labels = labels;
  vibChart.data.datasets[0].data = h.map((p) => val(p.vibRms));
  vibChart.data.datasets[1].data = flat(t.vibWarn ?? 200);
  vibChart.data.datasets[2].data = flat(t.vibDanger ?? 500);
  vibChart.update();
}

// ---------------------------------------------------------------- render: spectrum
function renderSpectrum(s) {
  if (!s || !Array.isArray(s.amp)) return;
  const g = s.group || 1;
  const df = s.df || 0.2;
  const freqs = s.amp.map((_, i) => ((i * g + (g - 1) / 2) * df).toFixed(2));
  let pk = 0;
  s.amp.forEach((a, i) => { if (a > s.amp[pk]) pk = i; });

  const ds = specChart.data.datasets[0];
  specChart.data.labels = freqs;
  ds.data = s.amp;
  ds.pointRadius = s.amp.map((_, i) => (i === pk && s.valid ? 5 : 0));
  specChart.update();

  $("specInfo").textContent = s.valid
    ? `peak ${fmt(s.peakFreq, 3)} Hz · resolution ${fmt(df, 3)} Hz`
    : `no clear peak · resolution ${fmt(df, 3)} Hz`;
}

// ---------------------------------------------------------------- render: events
function addEvent(ev) {
  if (!ev) return;
  state.events.unshift(ev);
  state.events.sort((a, b) => (b.ts || 0) - (a.ts || 0));
  state.events = state.events.slice(0, 50);
  const body = $("eventsBody");
  body.replaceChildren(
    ...state.events.map((e) => {
      const tr = document.createElement("tr");
      const lvl = isNum(e.level) ? e.level : 0;
      const cells = [
        isNum(e.ts) ? fmtDateTime(e.ts) : "--",
        LEVELS[lvl],
        (e.alerts || (lvl === 0 ? "Recovered" : "-")).split(",").map((a) => ALERT_NAMES[a] || a).join(", "),
        fmt(isNum(e.angle) ? e.angle : e.tilt, 2),
        fmt(e.freq, 2),
        fmt(e.vibRms, 1),
      ];
      cells.forEach((c, i) => {
        const td = document.createElement("td");
        td.textContent = c;
        if (i === 1) td.className = lvl >= 2 ? "c-danger" : lvl === 1 ? "c-warn" : "ok";
        tr.appendChild(td);
      });
      return tr;
    })
  );
}

// ---------------------------------------------------------------- connection status
function isOffline() {
  if (DEMO) return false;
  if (!state.live || !isNum(state.live.ts)) return true;
  return Date.now() + state.serverOffset - state.live.ts > OFFLINE_AFTER_MS;
}

function updateConnection() {
  const pill = $("connPill");
  const text = $("connText");
  if (DEMO) {
    pill.className = "pill pill-demo";
    text.textContent = "Demo data";
  } else if (!state.live) {
    pill.className = "pill pill-wait";
    text.textContent = "Waiting for Firebase";
    setConnCard("cardEsp", "espState", "espDetail", "wait", "WAITING", "Listening to scourSentry/pier-01/live");
  } else if (isOffline()) {
    pill.className = "pill pill-offline";
    text.textContent = "Device offline";
    setConnCard("cardEsp", "espState", "espDetail", "bad", "OFFLINE", "No packet for >15 s");
  } else {
    pill.className = "pill pill-online";
    text.textContent = "Live from Firebase";
  }

  if (state.live && isNum(state.live.ts)) {
    const age = Math.max(0, Math.round((Date.now() + state.serverOffset - state.live.ts) / 1000));
    $("lastUpdate").textContent = fmtDateTime(state.live.ts);
    $("lastAge").textContent = age < 60 ? `${age}s ago` : age < 3600 ? `${Math.floor(age / 60)}m ago` : `${Math.floor(age / 3600)}h ago`;
  } else if (state.liveReceivedAt) {
    $("lastUpdate").textContent = fmtDateTime(state.liveReceivedAt);
    $("lastAge").textContent = "browser receive time";
  }

  if (state.live) {
    const lvl = isNum(state.live.level) ? state.live.level : 0;
    renderStatus(lvl, (state.live.alerts || "").split(",").filter(Boolean), state.live);
  }
}
setInterval(updateConnection, 1000);

function showError(msg) {
  $("connPill").className = "pill pill-offline";
  $("connText").textContent = msg;
}

// ---------------------------------------------------------------- data source: Firebase
async function startFirebase() {
  $("connText").textContent = "Connecting to Firebase...";
  pushFeed(`<span class="t">${fmtTime(Date.now())}</span>Connecting to Firebase Realtime Database...`);

  const { initializeApp } = await import(`https://www.gstatic.com/firebasejs/${FB_VERSION}/firebase-app.js`);
  const { getDatabase, ref, onValue } = await import(
    `https://www.gstatic.com/firebasejs/${FB_VERSION}/firebase-database.js`
  );

  const app = initializeApp(firebaseConfig);
  const db = getDatabase(app);
  const base = `scourSentry/${DEVICE_ID}`;
  const onErr = (e) => {
    console.error(e);
    const msg = e.code === "PERMISSION_DENIED" ? "Permission denied (check DB rules)" : `Firebase error: ${e.message || e.code || e}`;
    showError(msg);
    pushFeed(`<span class="t">${fmtTime(Date.now())}</span><span class="bad">${msg}</span>`);
  };

  onValue(ref(db, ".info/connected"), (s) => {
    if (s.val() === true) {
      pushFeed(`<span class="t">${fmtTime(Date.now())}</span><span class="ok">Firebase socket connected</span> · listening to ${base}/live`);
      $("connText").textContent = "Listening for live data";
    }
  });
  onValue(ref(db, ".info/serverTimeOffset"), (s) => { state.serverOffset = s.val() || 0; });

  onValue(ref(db, `${base}/live`), (s) => {
    const data = s.val();
    if (!data) {
      pushFeed(`<span class="t">${fmtTime(Date.now())}</span><span class="bad">Path ${base}/live is empty</span>`);
      setConnCard("cardEsp", "espState", "espDetail", "wait", "WAITING", "Live node exists but has no data yet");
      return;
    }
    renderLive(data);
  }, onErr);

  onValue(ref(db, `${base}/spectrum`), (s) => renderSpectrum(s.val()), onErr);

  // Full replace (device keeps only last 5 minutes on Firebase)
  onValue(ref(db, `${base}/history`), (s) => {
    const v = s.val();
    state.history = [];
    if (v && typeof v === "object") {
      Object.values(v)
        .filter((p) => p && isNum(p.ts))
        .sort((a, b) => a.ts - b.ts)
        .slice(-HISTORY_POINTS)
        .forEach((p) => state.history.push(p));
    }
    updateHistoryCharts();
  }, onErr);

  onValue(ref(db, `${base}/events`), (s) => {
    const v = s.val();
    state.events = [];
    if (v && typeof v === "object") {
      Object.values(v)
        .filter((e) => e)
        .sort((a, b) => (b.ts || 0) - (a.ts || 0))
        .slice(0, 50)
        .forEach((e) => state.events.push(e));
    }
    // Re-render table from state.events
    const body = $("eventsBody");
    if (!state.events.length) {
      body.innerHTML = `<tr><td colspan="6" class="empty">No events in the last 5 minutes</td></tr>`;
      return;
    }
    body.replaceChildren(
      ...state.events.map((e) => {
        const tr = document.createElement("tr");
        const lvl = isNum(e.level) ? e.level : 0;
        const cells = [
          isNum(e.ts) ? fmtDateTime(e.ts) : "--",
          LEVELS[lvl],
          (e.alerts || (lvl === 0 ? "Recovered" : "-")).split(",").map((a) => ALERT_NAMES[a] || a).join(", "),
          fmt(isNum(e.angle) ? e.angle : e.tilt, 2),
          fmt(e.freq, 2),
          fmt(e.vibRms, 1),
        ];
        cells.forEach((c, i) => {
          const td = document.createElement("td");
          td.textContent = c;
          if (i === 1) td.className = lvl >= 2 ? "c-danger" : lvl === 1 ? "c-warn" : "ok";
          tr.appendChild(td);
        });
        return tr;
      })
    );
  }, onErr);
}

// ---------------------------------------------------------------- data source: demo simulator
function startDemo() {
  $("demoBar").classList.remove("hidden");
  let scenario = "auto";
  document.querySelectorAll(".demo-controls button").forEach((b) =>
    b.addEventListener("click", () => {
      scenario = b.dataset.scenario;
      document.querySelectorAll(".demo-controls button").forEach((x) => x.classList.toggle("active", x === b));
    })
  );

  const t = state.thresholds;
  const BASE_F = 4.95;
  const targets = {
    safe: { tilt: 0.12, freq: BASE_F, vib: 9 },
    warning: { tilt: 0.7, freq: BASE_F * 0.87, vib: 70 },
    danger: { tilt: 1.35, freq: BASE_F * 0.76, vib: 190 },
  };
  const sim = { tilt: 0.12, freq: BASE_F, vib: 9, dir: 0.6 };
  const start = Date.now();
  let level = 0;
  const rnd = (s) => (Math.random() - 0.5) * 2 * s;

  function autoScenario() {
    const c = ((Date.now() - start) / 1000) % 120;
    return c < 55 ? "safe" : c < 85 ? "warning" : c < 105 ? "danger" : "safe";
  }

  function step(k = 0.25) {
    const tg = targets[scenario === "auto" ? autoScenario() : scenario];
    sim.tilt += (tg.tilt - sim.tilt) * k + rnd(0.01);
    sim.freq += (tg.freq - sim.freq) * k + rnd(0.02);
    sim.vib += (tg.vib - sim.vib) * k + rnd(2);
    sim.tilt = Math.max(0, sim.tilt);
    sim.vib = Math.max(1, sim.vib);
  }

  function snapshot(ts) {
    const roll = sim.tilt * Math.cos(sim.dir);
    const pitch = sim.tilt * Math.sin(sim.dir);
    const change = ((sim.freq - BASE_F) / BASE_F) * 100;
    return { ts, tilt: sim.tilt, roll, pitch, freq: sim.freq, vibRms: sim.vib, change };
  }

  // prefill one hour-ish of history
  for (let i = 120; i > 0; i--) {
    step(0.3);
    const s = snapshot(Date.now() - i * 10000);
    state.history.push({ ts: s.ts, tilt: s.tilt, roll: s.roll, pitch: s.pitch, freq: s.freq, vibRms: s.vibRms, level: 0 });
  }
  addEvent({ ts: start - 3600e3, level: 0, alerts: "", tilt: 0.1, freq: BASE_F, vibRms: 8 });

  function tick() {
    step();
    const now = Date.now();
    const s = snapshot(now);
    const lv = Math.max(
      levelOf(s.tilt, t.tiltWarn, t.tiltDanger),
      levelOf(-s.change, t.freqDropWarn, t.freqDropDanger),
      levelOf(s.vibRms, t.vibWarn, t.vibDanger)
    );
    const alerts = [
      levelOf(s.tilt, t.tiltWarn, t.tiltDanger) > 0 && "TILT",
      levelOf(-s.change, t.freqDropWarn, t.freqDropDanger) > 0 && "FREQ",
      levelOf(s.vibRms, t.vibWarn, t.vibDanger) > 0 && "VIB",
    ].filter(Boolean).join(",");

    if (lv !== level) {
      level = lv;
      addEvent({ ts: now, level: lv, alerts, tilt: s.tilt, freq: s.freq, vibRms: s.vibRms });
    }

    state.serverOffset = 0;
    renderLive({
      ts: now,
      uptime: Math.round((now - start) / 1000) + 86400 * 2 + 3723,
      roll: s.roll, pitch: s.pitch, tilt: s.tilt,
      rollRaw: 1.84 + s.roll, pitchRaw: -0.62 + s.pitch,
      freq: s.freq, freqValid: true, freqPeakMg: 3 + sim.vib * 0.08,
      baselineFreq: BASE_F, freqChangePct: s.change,
      vibRms: s.vibRms, vibPeak: s.vibRms * 3.1, sampleRate: 199.8,
      level: lv, status: LEVELS[lv], alerts,
      buzzer: lv > 0, muted: false, sensorOk: true, sdOk: true,
      tiltCal: true, freqCal: true, calibrating: false, rssi: -58 + Math.round(rnd(4)),
      thresholds: t,
    });
    addHistory({ ts: now, tilt: s.tilt, roll: s.roll, pitch: s.pitch, freq: s.freq, vibRms: s.vibRms, level: lv });
  }

  function spectrumTick() {
    const df = 200 / 1024, group = 2, n = 64;
    const amp = [];
    for (let i = 0; i < n; i++) {
      const f = (i * group + 0.5) * df;
      const peak = (sim.vib * 0.08 + 2) * Math.exp(-((f - sim.freq) ** 2) / 0.05);
      const harmonic = 0.35 * peak * Math.exp(-((f - sim.freq * 2.7) ** 2) / 0.1);
      amp.push(+(peak + harmonic + 0.15 + Math.random() * 0.25 + (f < 1 ? 0.6 / (f + 0.3) : 0)).toFixed(3));
    }
    renderSpectrum({ ts: Date.now(), df, group, peakFreq: sim.freq, valid: true, amp });
  }

  tick();
  spectrumTick();
  setInterval(tick, 2000);
  setInterval(spectrumTick, 5000);
}

// ---------------------------------------------------------------- boot
$("deviceId").textContent = DEVICE_ID;
if (DEMO) {
  startDemo();
} else {
  startFirebase().catch((e) => {
    console.error(e);
    showError("Could not load Firebase");
  });
}
