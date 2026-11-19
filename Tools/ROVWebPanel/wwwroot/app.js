// Painel do ROVsim: recebe o estado da ponte por WebSocket e manda comandos e pilotagem de volta.
"use strict";

const $ = (id) => document.getElementById(id);
const fmt = (v, digits = 1, sign = false) =>
  v === undefined || v === null || Number.isNaN(v) ? "--" : (sign && v >= 0 ? "+" : "") + Number(v).toFixed(digits);
const CARDINALS = ["N", "NNE", "NE", "ENE", "L", "ESE", "SE", "SSE", "S", "SSO", "SO", "OSO", "O", "ONO", "NO", "NNO"];
const cardinal = (deg) => CARDINALS[Math.round(((deg % 360) + 360) % 360 / 22.5) % 16];
const PHASES = { ADJUSTING: "ajustando", BRAKING: "freando", HOLDING: "segurando", NO_BOTTOM: "sem fundo", OFF: "desligado" };

// =============================================================================
// WebSocket com reconexão automática
// =============================================================================
let socket = null;

function connect() {
  socket = new WebSocket(`ws://${location.host}/ws`);
  socket.onopen = () => setLink("link-ws", true);
  socket.onclose = () => { setLink("link-ws", false); setTimeout(connect, 1500); };
  socket.onmessage = (event) => {
    const msg = JSON.parse(event.data);
    if (msg.type === "state") onState(msg);
    else if (msg.type === "console") logConsole(msg.dir, msg.text);
  };
}

function send(obj) {
  if (socket && socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify(obj));
}

function sendCommand(text) {
  if (text.trim()) send({ type: "cmd", text: text.trim() });
}

function setLink(id, on) { $(id).classList.toggle("on", !!on); }

// =============================================================================
// Estado vindo da ponte (10 Hz)
// =============================================================================
const history = { depth: [], targetDepth: [], heading: [], targetHeading: [] };
const HISTORY_SIZE = 600; // 60 s a 10 Hz

function onState(state) {
  setLink("link-telemetry", state.links.telemetry);
  setLink("link-commands", state.links.commands);
  setLink("link-dvl", state.links.dvl);
  renderSensors(state.sensors);

  const t = state.links.telemetry ? state.telemetry : null;
  lastTelemetry = t;
  updateSwitches(t);
  if (!t) {
    $("badge-mode").textContent = "sem telemetria";
    return;
  }

  // Cabeçalho
  $("badge-mode").textContent = t.mode === "SIM" ? "SIMULADOR" : "DEMO";
  $("badge-mode").classList.toggle("demo", t.mode !== "SIM");
  $("badge-control").textContent = t.remote ? "CONTROLE REMOTO" : "controle local";
  $("badge-control").classList.toggle("remote", !!t.remote);

  // Navegação
  $("depth").textContent = fmt(t.depth, 1);
  $("altitude").textContent = t.altitude >= 0 ? fmt(t.altitude, 1) : "--";
  $("heading").textContent = `${fmt(t.heading, 1)}°`;
  $("heading-card").textContent = `${cardinal(t.heading)} · para onde a proa aponta`;
  $("pitch").textContent = `${fmt(t.pitch, 1, true)}°`;
  $("roll").textContent = `${fmt(t.roll, 1, true)}°`;

  // Velocidade (no referencial do ROV)
  const [vx, vy, vz] = t.vel;
  const horiz = Math.hypot(vx, vy);
  $("v-surge").textContent = `${fmt(vx, 2, true)} m/s`;
  $("v-sway").textContent = `${fmt(vy, 2, true)} m/s`;
  $("v-heave").textContent = `${fmt(vz, 2, true)} m/s`;
  $("v-horiz").textContent = `${fmt(horiz, 2)} m/s (${fmt(horiz / 0.514444, 2)} nós)`;
  $("v-yaw").textContent = `${fmt(t.yawRate, 1, true)} °/s`;
  renderBars("thrust-bars", ["Avanço", "Lateral", "Vertical", "Giro"], t.thrust || [0, 0, 0, 0]);

  // Autos
  const ah = t.autos.heading, av = t.autos.vertical;
  $("auto-heading").innerHTML = ah.on
    ? `<span class="phase ${ah.phase}">${PHASES[ah.phase]}</span> alvo ${fmt(ah.target, 1)}°` : `<span class="phase OFF">desligado</span>`;
  $("auto-heading-dev").textContent = ah.on ? `desvio ${fmt(angleDiff(t.heading, ah.target), 1, true)}°` : "";
  if (av.mode === "OFF") {
    $("auto-vertical").innerHTML = `<span class="phase OFF">desligado</span>`;
    $("auto-vertical-dev").textContent = "";
  } else {
    const isDepth = av.mode === "DEPTH";
    $("auto-vertical").innerHTML = `<span class="phase ${av.phase}">${PHASES[av.phase]}</span> ${isDepth ? "prof." : "alt."} ${fmt(av.target, 1)} m`;
    const dev = isDepth ? t.depth - av.target : t.altitude - av.target;
    $("auto-vertical-dev").textContent = `desvio ${fmt(dev, 2, true)} m ${isDepth ? "(+ = mais fundo)" : "(+ = mais alto)"}`;
  }

  const ap = t.autos.position;
  if (ap && ap.on) {
    $("auto-position").innerHTML = `<span class="phase ${ap.phase}">${PHASES[ap.phase]}</span> N ${fmt(ap.target[0], 1)} · L ${fmt(ap.target[1], 1)} m`;
    $("auto-position-dev").textContent = `desvio ${fmt(ap.offset, 2)} m`;
  } else {
    $("auto-position").innerHTML = `<span class="phase OFF">desligado</span>`;
    $("auto-position-dev").textContent = "";
  }

  // Ambiente
  $("current").textContent = t.current.knots > 0.005
    ? `${fmt(t.current.knots, 2)} nós para ${fmt(t.current.toward, 0)}° ${cardinal(t.current.toward)}` : "nenhuma";
  const sw = t.swell;
  if (sw && sw.height > 0) {
    $("swell").textContent = `${fmt(sw.height, 1)} m · ${fmt(sw.period, 0)} s · de ${fmt(sw.from, 0)}° ${cardinal(sw.from)}`;
    $("swell-hint").textContent = `aqui sobra ${fmt(sw.attenuation * 100, 0)}% do movimento (de onde vem)`;
  } else {
    $("swell").textContent = "nenhum";
    $("swell-hint").textContent = "de onde as ondas vêm";
  }
  $("lights-camera").textContent = `${t.lights ? "ligadas" : "desligadas"} · ${t.camera === "EXT" ? "externa" : "cockpit"}`;
  $("submerged").textContent = `${fmt(t.submerged * 100, 0)}%`;

  // Instrumentos e gráficos
  drawCompass(t.heading, ah.on ? ah.target : null);
  drawAttitude(t.pitch, t.roll);
  push("depth", t.depth); push("targetDepth", av.mode === "DEPTH" ? av.target : null);
  push("heading", t.heading); push("targetHeading", ah.on ? ah.target : null);
  drawChart("chart-depth", "Profundidade (m)", history.depth, history.targetDepth, true);
  drawChart("chart-heading", "Rumo (°)", history.heading, history.targetHeading, false);
}

const angleDiff = (from, to) => ((from - to + 540) % 360) - 180; // desvio pelo caminho mais curto
function push(key, value) { history[key].push(value); if (history[key].length > HISTORY_SIZE) history[key].shift(); }

// =============================================================================
// Desenho
// =============================================================================
function renderBars(id, labels, values) {
  const el = $(id);
  if (el.children.length !== labels.length) {
    el.innerHTML = labels.map((l) => `<div class="bar"><span>${l}</span><div class="track"><div class="fill"></div></div><span>0%</span></div>`).join("");
  }
  [...el.children].forEach((row, i) => {
    const v = Math.max(-100, Math.min(100, values[i] || 0));
    const fill = row.querySelector(".fill");
    fill.className = `fill ${v >= 0 ? "pos" : "neg"}`;
    fill.style.width = `${Math.abs(v) / 2}%`;
    row.lastElementChild.textContent = `${v >= 0 ? "+" : ""}${v.toFixed(0)}%`;
  });
}

function drawCompass(heading, target) {
  const c = $("compass"), g = c.getContext("2d"), r = c.width / 2 - 14, cx = c.width / 2, cy = c.height / 2;
  g.clearRect(0, 0, c.width, c.height);
  g.save(); g.translate(cx, cy);
  g.strokeStyle = "#1e4352"; g.lineWidth = 2; g.beginPath(); g.arc(0, 0, r, 0, Math.PI * 2); g.stroke();
  // A rosa gira: o rumo atual fica sempre no topo (como numa girobússola de console)
  g.rotate((-heading * Math.PI) / 180);
  for (let d = 0; d < 360; d += 10) {
    const a = (d * Math.PI) / 180, major = d % 30 === 0;
    g.strokeStyle = major ? "#9bb" : "#4a6670"; g.lineWidth = major ? 2 : 1;
    g.beginPath(); g.moveTo(Math.sin(a) * (r - (major ? 12 : 6)), -Math.cos(a) * (r - (major ? 12 : 6)));
    g.lineTo(Math.sin(a) * r, -Math.cos(a) * r); g.stroke();
  }
  g.fillStyle = "#d5e3ea"; g.font = "bold 13px Segoe UI"; g.textAlign = "center"; g.textBaseline = "middle";
  [["N", 0], ["L", 90], ["S", 180], ["O", 270]].forEach(([t, d]) => {
    // Posição gira com a rosa, mas a letra fica sempre em pé (desfaz a rotação só para escrever)
    const a = (d * Math.PI) / 180;
    g.save(); g.translate(Math.sin(a) * (r - 24), -Math.cos(a) * (r - 24)); g.rotate((heading * Math.PI) / 180);
    g.fillText(t, 0, 0); g.restore();
  });
  if (target !== null) { // marca do alvo do auto heading
    const a = (target * Math.PI) / 180;
    g.fillStyle = "#ffb44d"; g.beginPath();
    g.arc(Math.sin(a) * (r + 1), -Math.cos(a) * (r + 1), 6, 0, Math.PI * 2); g.fill();
  }
  g.restore();
  // Proa (fixa no topo)
  g.fillStyle = "#5fe3ff"; g.beginPath(); g.moveTo(cx, cy - r + 16); g.lineTo(cx - 9, cy - r + 34); g.lineTo(cx + 9, cy - r + 34); g.fill();
  g.fillStyle = "#d5e3ea"; g.font = "bold 20px Segoe UI"; g.textAlign = "center"; g.fillText(`${heading.toFixed(0).padStart(3, "0")}°`, cx, cy + 6);
}

function drawAttitude(pitch, roll) {
  const c = $("attitude"), g = c.getContext("2d"), w = c.width, h = c.height, r = w / 2 - 14;
  g.clearRect(0, 0, w, h);
  g.save(); g.translate(w / 2, h / 2);
  g.beginPath(); g.arc(0, 0, r, 0, Math.PI * 2); g.clip();
  // Céu e "fundo" giram com o roll e sobem/descem com o pitch (4 px por grau)
  g.rotate((-roll * Math.PI) / 180);
  const offset = pitch * 4;
  g.fillStyle = "#1d5d7a"; g.fillRect(-w, -h * 2 + offset, w * 2, h * 2);
  g.fillStyle = "#3a2e1f"; g.fillRect(-w, offset, w * 2, h * 2);
  g.strokeStyle = "#e8f2f6"; g.lineWidth = 2; g.beginPath(); g.moveTo(-w, offset); g.lineTo(w, offset); g.stroke();
  g.lineWidth = 1; g.fillStyle = "#e8f2f6"; g.font = "10px Segoe UI";
  for (let p = -20; p <= 20; p += 10) {
    if (p === 0) continue;
    const y = offset - p * 4;
    g.beginPath(); g.moveTo(-20, y); g.lineTo(20, y); g.stroke(); g.fillText(`${p}`, 24, y + 3);
  }
  g.restore();
  // Símbolo fixo do ROV
  g.strokeStyle = "#ffb44d"; g.lineWidth = 3;
  g.beginPath(); g.moveTo(w / 2 - 40, h / 2); g.lineTo(w / 2 - 12, h / 2); g.lineTo(w / 2, h / 2 + 8); g.lineTo(w / 2 + 12, h / 2); g.lineTo(w / 2 + 40, h / 2); g.stroke();
  g.strokeStyle = "#1e4352"; g.lineWidth = 2; g.beginPath(); g.arc(w / 2, h / 2, r, 0, Math.PI * 2); g.stroke();
}

function drawChart(id, title, values, targets, invert) {
  const c = $(id), g = c.getContext("2d"), w = c.width, h = c.height, pad = 26;
  g.clearRect(0, 0, w, h);
  const all = values.concat(targets.filter((v) => v !== null));
  if (values.length < 2) return;
  let lo = Math.min(...all), hi = Math.max(...all);
  if (hi - lo < 1) { const mid = (hi + lo) / 2; lo = mid - 0.5; hi = mid + 0.5; }
  const x = (i) => pad + (i / (HISTORY_SIZE - 1)) * (w - pad - 6);
  // Profundidade cresce para baixo: o gráfico fica "de cabeça para baixo", como o ROV vê
  const y = (v) => invert ? 14 + ((v - lo) / (hi - lo)) * (h - 28) : h - 14 - ((v - lo) / (hi - lo)) * (h - 28);
  g.fillStyle = "#7b929d"; g.font = "11px Segoe UI";
  g.fillText(title, pad, 11); g.fillText(hi.toFixed(1), 0, invert ? h - 10 : 22); g.fillText(lo.toFixed(1), 0, invert ? 22 : h - 10);
  const offset = HISTORY_SIZE - values.length;
  const line = (arr, color, dash) => {
    g.strokeStyle = color; g.lineWidth = 1.6; g.setLineDash(dash); g.beginPath(); let started = false;
    arr.forEach((v, i) => { if (v === null) { started = false; return; } const px = x(i + offset), py = y(v); started ? g.lineTo(px, py) : g.moveTo(px, py); started = true; });
    g.stroke(); g.setLineDash([]);
  };
  line(targets, "#ffb44d", [5, 4]);
  line(values, "#5fe3ff", []);
}

function renderSensors(sensors) {
  if (!sensors || sensors.length === 0) return;
  $("sensor-rows").innerHTML = sensors.map((s) => `
    <tr>
      <td>${s.name}</td><td>${s.source}</td><td>$${s.type}</td>
      <td>${s.hz.toFixed(1)}</td><td>${s.total}</td>
      <td class="${s.invalid > 0 ? "bad" : "good"}">${s.invalid}</td>
      <td>${s.fields}</td><td class="sentence">${escapeHtml(s.last)}</td>
    </tr>`).join("");
}

const escapeHtml = (s) => s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

// =============================================================================
// Console
// =============================================================================
const consoleHistory = [];
let historyIndex = 0;

function logConsole(dir, text) {
  const log = $("console-log");
  const line = document.createElement("div");
  line.className = dir + (dir === "in" ? (text.startsWith("OK") ? " ok" : text.startsWith("ERR") ? " err" : "") : "");
  line.textContent = `${new Date().toLocaleTimeString()}  ${text}`;
  log.appendChild(line);
  while (log.children.length > 300) log.removeChild(log.firstChild);
  log.scrollTop = log.scrollHeight;
}

$("console-form").addEventListener("submit", (e) => {
  e.preventDefault();
  const input = $("console-input");
  sendCommand(input.value);
  if (input.value.trim()) consoleHistory.push(input.value.trim());
  historyIndex = consoleHistory.length;
  input.value = "";
});
$("console-input").addEventListener("keydown", (e) => {
  if (e.key === "ArrowUp" && historyIndex > 0) { historyIndex--; e.target.value = consoleHistory[historyIndex]; e.preventDefault(); }
  if (e.key === "ArrowDown") { historyIndex = Math.min(consoleHistory.length, historyIndex + 1); e.target.value = consoleHistory[historyIndex] || ""; e.preventDefault(); }
});

// Botões com comando fixo e formulários com valor
document.querySelectorAll("button[data-cmd]").forEach((b) => b.addEventListener("click", (e) => { e.preventDefault(); sendCommand(b.dataset.cmd); }));
document.querySelectorAll("form[data-template]").forEach((f) => f.addEventListener("submit", (e) => {
  e.preventDefault();
  sendCommand(f.dataset.template.replace("{v}", f.querySelector("input").value));
}));
$("current-form").addEventListener("submit", (e) => {
  e.preventDefault();
  sendCommand(`CURRENT ${$("current-knots").value} ${$("current-toward").value || 0}`);
});
const swellCommand = () => `SWELL ${$("swell-height").value || 2} ${$("swell-period").value || 10} ${$("swell-from").value || 0}`;
$("pos-form").addEventListener("submit", (e) => {
  e.preventDefault();
  sendCommand(`AUTO POS ${$("pos-north").value} ${$("pos-east").value}`);
});
$("swell-form").addEventListener("submit", (e) => { e.preventDefault(); sendCommand(swellCommand()); });

// =============================================================================
// Botões liga/desliga: cada um mostra o estado ATUAL (da telemetria) e, ao clicar,
// manda o comando que leva ao estado oposto. Assim o botão nunca "mente", mesmo que o
// estado tenha mudado pelo teclado do simulador ou pelo console.
// =============================================================================
let lastTelemetry = null;

const SWITCHES = {
  "sw-mode": {
    label: (t) => `Modo: ${t.mode === "SIM" ? "simulador" : "demo"}`,
    on: (t) => t.mode === "SIM",
    command: (t) => (t.mode === "SIM" ? "MODE DEMO" : "MODE SIM"),
  },
  "sw-lights": {
    label: (t) => `Luzes: ${t.lights ? "ON" : "OFF"}`,
    on: (t) => t.lights,
    command: (t) => (t.lights ? "LIGHTS OFF" : "LIGHTS ON"),
  },
  "sw-camera": {
    label: (t) => `Câmera: ${t.camera === "EXT" ? "externa" : "cockpit"}`,
    on: () => true,
    command: (t) => (t.camera === "EXT" ? "CAMERA COCKPIT" : "CAMERA EXT"),
  },
  "sw-auto-heading": {
    label: (t) => `Auto rumo: ${t.autos.heading.on ? "ON" : "OFF"}`,
    on: (t) => t.autos.heading.on,
    command: (t) => (t.autos.heading.on ? "AUTO HEADING OFF" : "AUTO HEADING ON"),
  },
  "sw-auto-depth": {
    label: (t) => `Auto prof.: ${t.autos.vertical.mode === "DEPTH" ? "ON" : "OFF"}`,
    on: (t) => t.autos.vertical.mode === "DEPTH",
    command: (t) => (t.autos.vertical.mode === "DEPTH" ? "AUTO DEPTH OFF" : "AUTO DEPTH ON"),
  },
  "sw-auto-alt": {
    label: (t) => `Auto alt.: ${t.autos.vertical.mode === "ALT" ? "ON" : "OFF"}`,
    on: (t) => t.autos.vertical.mode === "ALT",
    command: (t) => (t.autos.vertical.mode === "ALT" ? "AUTO ALT OFF" : "AUTO ALT ON"),
  },
  "sw-auto-pos": {
    label: (t) => `Auto posição: ${t.autos.position && t.autos.position.on ? "ON" : "OFF"}`,
    on: (t) => t.autos.position && t.autos.position.on,
    command: (t) => (t.autos.position && t.autos.position.on ? "AUTO POS OFF" : "AUTO POS ON"),
  },
  "sw-current": {
    label: (t) => `Corrente: ${t.current.knots > 0.005 ? `${fmt(t.current.knots, 1)} nó` : "OFF"}`,
    on: (t) => t.current.knots > 0.005,
    // Ligada: desliga. Desligada: liga com os valores dos campos (1 nó para o Norte se estiverem vazios)
    command: (t) => (t.current.knots > 0.005 ? "CURRENT 0" : `CURRENT ${$("current-knots").value || 1} ${$("current-toward").value || 0}`),
  },
  "sw-swell": {
    label: (t) => `Swell: ${t.swell && t.swell.height > 0 ? `${fmt(t.swell.height, 1)} m` : "OFF"}`,
    on: (t) => t.swell && t.swell.height > 0,
    // Ligado: desliga. Desligado: liga com os campos (2 m, 10 s, do Norte se estiverem vazios)
    command: (t) => (t.swell && t.swell.height > 0 ? "SWELL 0" : swellCommand()),
  },
};

function updateSwitches(t) {
  for (const [id, sw] of Object.entries(SWITCHES)) {
    const button = $(id);
    button.disabled = !t;
    if (!t) continue;
    button.textContent = sw.label(t);
    button.classList.toggle("on", !!sw.on(t));
  }
}

for (const [id, sw] of Object.entries(SWITCHES)) {
  $(id).addEventListener("click", (e) => {
    e.preventDefault();
    if (lastTelemetry) sendCommand(sw.command(lastTelemetry));
  });
}

connect();
