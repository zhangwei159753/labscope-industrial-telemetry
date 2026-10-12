const $ = selector => document.querySelector(selector);
const deviceInput = $('#device-id');
const statusText = $('#connection-status');
const statusLight = $('#status-light');
const log = $('#event-log');
const canvas = $('#trend');
const state = { socket: null, device: '', samples: [], previous: null, retry: null };

function addLog(message) {
  const item = document.createElement('li');
  const stamp = document.createElement('time');
  stamp.textContent = new Date().toLocaleTimeString('zh-CN', { hour12: false });
  item.append(stamp, document.createTextNode(message));
  log.prepend(item);
  while (log.children.length > 7) log.lastElementChild.remove();
}

function setStatus(label, mode = '') {
  statusText.textContent = label;
  statusLight.className = `status-light ${mode}`;
}

function connect(device) {
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(device)) {
    setStatus('设备编号无效', 'warn');
    addLog('设备编号仅允许英数字、- 和 _。');
    return;
  }
  clearTimeout(state.retry);
  if (state.socket) state.socket.close();
  state.device = device;
  state.samples = [];
  state.previous = null;
  drawChart();
  const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const socket = new WebSocket(`${scheme}//${location.host}/ws/monitor?device_id=${encodeURIComponent(device)}`);
  state.socket = socket;
  setStatus('正在连接', 'warn');
  addLog(`建立 ${device} 监控连接`);
  socket.onopen = () => setStatus('等待订阅确认', 'warn');
  socket.onmessage = message => {
    let event;
    try { event = JSON.parse(message.data); } catch { addLog('收到无法解析的消息'); return; }
    if (event.type === 'subscribed') {
      setStatus('监控中', 'live');
      addLog(`已订阅 ${device}`);
    } else if (event.type === 'telemetry') {
      renderSample(event);
    }
  };
  socket.onclose = () => {
    if (state.socket !== socket) return;
    setStatus('连接中断', 'warn');
    addLog('连接断开，1 秒后重连');
    state.retry = setTimeout(() => connect(device), 1000);
  };
  socket.onerror = () => setStatus('链路异常', 'warn');
}

function renderSample(event) {
  const receivedAt = Date.now();
  $('#sequence').textContent = Number(event.seq).toLocaleString('zh-CN');
  $('#ingest-lag').textContent = `${event.received_ms - event.timestamp_ms} ms`;
  $('#monitor-lag').textContent = `${receivedAt - event.timestamp_ms} ms`;
  $('#last-seen').textContent = `最近更新 ${new Date(receivedAt).toLocaleTimeString('zh-CN', { hour12: false })}`;
  if (state.previous) {
    const elapsed = receivedAt - state.previous.receivedAt;
    if (elapsed > 0) $('#sample-rate').textContent = `${Math.round((event.seq - state.previous.seq) * 1000 / elapsed)}/s`;
  }
  state.previous = { seq: event.seq, receivedAt };

  const container = $('#metrics');
  container.replaceChildren();
  for (const [key, value] of Object.entries(event.metrics)) {
    const card = document.createElement('div'); card.className = 'metric';
    const label = document.createElement('small'); label.textContent = key;
    const number = document.createElement('strong'); number.textContent = String(value);
    card.append(label, number); container.append(card);
  }
  const temp = Number(event.metrics.temperature_c);
  if (Number.isFinite(temp)) {
    state.samples.push(temp);
    if (state.samples.length > 120) state.samples.shift();
    drawChart();
  }
}

function drawChart() {
  const box = canvas.getBoundingClientRect();
  if (!box.width || !box.height) return;
  const ratio = window.devicePixelRatio || 1;
  canvas.width = Math.round(box.width * ratio);
  canvas.height = Math.round(box.height * ratio);
  const ctx = canvas.getContext('2d');
  ctx.scale(ratio, ratio);
  const w = box.width, h = box.height, pad = 18;
  ctx.strokeStyle = '#29414a'; ctx.lineWidth = 1;
  for (let line = 1; line <= 4; line++) {
    const y = line * h / 5;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
  }
  const points = state.samples;
  if (points.length < 2) { $('#chart-range').textContent = '等待采样'; return; }
  let min = Math.min(...points), max = Math.max(...points);
  if (max - min < 0.3) { min -= 0.15; max += 0.15; }
  $('#chart-range').textContent = `${min.toFixed(2)} – ${max.toFixed(2)} °C`;
  ctx.strokeStyle = '#83d6cb'; ctx.lineWidth = 2.5; ctx.lineJoin = 'round';
  ctx.beginPath();
  points.forEach((value, index) => {
    const x = index / 119 * w;
    const y = h - pad - (value - min) / (max - min) * (h - pad * 2);
    if (index) ctx.lineTo(x, y); else ctx.moveTo(x, y);
  });
  ctx.stroke();
  const x = (points.length - 1) / 119 * w;
  const y = h - pad - (points.at(-1) - min) / (max - min) * (h - pad * 2);
  ctx.fillStyle = '#efb463'; ctx.beginPath(); ctx.arc(x, y, 4, 0, Math.PI * 2); ctx.fill();
}

async function refreshStats() {
  try {
    const response = await fetch('/api/v1/stats', { cache: 'no-store' });
    const stats = await response.json();
    for (const field of ['accepted', 'rejected', 'devices', 'monitors']) {
      $(`#${field}`).textContent = Number(stats[field]).toLocaleString('zh-CN');
    }
  } catch { addLog('统计接口暂不可用'); }
}

$('#connect').addEventListener('click', () => connect(deviceInput.value.trim()));
deviceInput.addEventListener('keydown', event => { if (event.key === 'Enter') connect(deviceInput.value.trim()); });
document.querySelectorAll('[data-device]').forEach(button => button.addEventListener('click', () => {
  deviceInput.value = button.dataset.device;
  connect(button.dataset.device);
}));
window.addEventListener('resize', drawChart);
setInterval(() => { $('#clock').textContent = new Date().toLocaleTimeString('zh-CN', { hour12: false }); }, 1000);
setInterval(refreshStats, 1000);
refreshStats();
drawChart();
connect(deviceInput.value);
