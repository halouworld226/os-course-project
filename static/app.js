const $ = (selector) => document.querySelector(selector);
let currentUser = null;
let currentTicket = null;
let currentJob = null;
let jobs = [];
let messageTimer;
const tileImages = new Map();

const eventNames = {tile_dispatched:'派发', tile_committed:'已提交', worker_exit:'进程退出', tile_requeued:'重新入队'};
const stateNames = {processing:'提交中', queued:'等待调度', running:'处理中', complete:'已完成', failed:'失败'};
const operationNames = {grayscale:'灰度化', invert:'反色'};

function message(text) {
  $('#message').textContent = text;
  $('#message').hidden = false;
  clearTimeout(messageTimer);
  messageTimer = setTimeout(() => { $('#message').hidden = true; }, 6000);
}

async function api(path, options = {}) {
  const response = await fetch(path, options);
  const value = await response.json();
  if (!response.ok) throw new Error(value.error || '请求失败');
  return value;
}

function node(tag, text, className) {
  const element = document.createElement(tag);
  if (text !== undefined) element.textContent = text;
  if (className) element.className = className;
  return element;
}

function showUser(user) {
  currentTicket = null;
  currentJob = null;
  jobs = [];
  tileImages.clear();
  $('#preview').removeAttribute('src');
  $('#grid').width = 0;
  $('#preview-wrap').hidden = true;
  $('#download').hidden = true;
  $('#download').removeAttribute('href');
  $('#processing').hidden = true;
  $('#empty').hidden = false;
  $('#result-title').textContent = '选择一个作业';
  $('#job-details').textContent = '完成后可以查看图像与图块记录。';
  $('#history').replaceChildren();
  $('#timeline').replaceChildren(node('p', '暂无记录', 'muted small'));
  $('#worker-controls').replaceChildren();
  $('#server-jobs-panel').hidden = true;
  $('#queues-panel').hidden = true;
  currentUser = user;
  $('#login').hidden = Boolean(user);
  $('#workspace').hidden = !user;
  $('#identity').replaceChildren();
  if (user) {
    $('#identity').append(node('span', `${user.username} · ${user.role === 'admin' ? '管理员' : '普通用户'}`));
    const logout = node('button', '退出');
    logout.onclick = async () => { await api('/api/logout', {method:'POST'}); currentTicket = null; currentJob = null; showUser(null); };
    $('#identity').append(logout);
    refresh().catch(error => message(error.message));
  }
}

async function refresh() {
  if (!currentUser) return;
  const user = currentUser;
  const result = await api('/api/jobs');
  if (currentUser !== user) return;
  jobs = result.jobs.reverse();
  const list = $('#jobs');
  list.replaceChildren();
  if (!jobs.length) list.append(node('p', '暂无作业', 'muted small'));
  jobs.forEach(job => {
    const button = node('button', undefined, `job${job.ticket === currentTicket ? ' active' : ''}`);
    button.append(node('strong', `${operationNames[job.operation]}${job.fault_requested ? ' · 故障演示' : ''}`));
    button.append(node('span', stateNames[job.state] || job.state, `state ${job.state}`));
    button.append(node('small', `${job.owner} · ${job.priority === 'high' ? '高优先级' : '普通优先级'}`));
    button.append(node('small', job.job_id || job.ticket.slice(0, 8)));
    button.onclick = () => selectJob(job.ticket);
    list.append(button);
  });
  if (currentTicket) {
    const job = jobs.find(item => item.ticket === currentTicket);
    if (job) renderJob(job);
  } else if (jobs.length) selectJob(jobs[0].ticket);
  $('#server-jobs-panel').hidden = currentUser.role !== 'admin';
  $('#queues-panel').hidden = currentUser.role !== 'admin';
  if (currentUser.role === 'admin') {
    const active = await api('/api/active');
    if (currentUser !== user) return;
    const body = $('#server-jobs');
    body.replaceChildren();
    (active.jobs || []).forEach(job => {
      const row = node('tr');
      [job.job_id, job.priority === 'high' ? '高' : '普通', stateNames[job.state] || job.state,
        `${job.tile_complete}/${job.tile_total}`].forEach(value => row.append(node('td', value)));
      body.append(row);
    });
    if (!active.jobs?.length) {
      const row = node('tr');
      const cell = node('td', active.persistent_service === false ? '连接常驻 TCP 服务后可查看全局作业。' : '暂无作业');
      cell.colSpan = 4;
      row.append(cell);
      body.append(row);
    }
    const queues = await api('/api/queues');
    if (currentUser !== user) return;
    ['high', 'normal'].forEach(priority => {
      const entries = queues[`${priority}_queue`] || [];
      $(`#${priority}-count`).textContent = `(${entries.length})`;
      const list = $(`#${priority}-queue`);
      list.replaceChildren();
      entries.slice(0, 50).forEach(tile => list.append(node('li', `${tile.job_id} / 图块 ${tile.tile_id}`)));
      if (!entries.length) list.append(node('li', queues.persistent_service === false ? 'TCP 模式可查看实时队列' : '队列为空', 'muted'));
      if (entries.length > 50) list.append(node('li', `另有 ${entries.length - 50} 块`, 'muted'));
    });
    const workers = $('#global-workers');
    workers.replaceChildren();
    (queues.workers || []).forEach(worker => {
      const row = node('div', undefined, 'worker-row');
      const busy = Boolean(worker.job_id);
      row.append(node('span', `PID ${worker.pid} · ${busy ? `${worker.job_id} / 图块 ${worker.tile_id} / 尝试 ${worker.attempt_id}` : '空闲'}`));
      if (busy) row.append(killButton(worker.pid));
      workers.append(row);
    });
  }
}

function killButton(pid) {
  const button = node('button', `终止 PID ${pid}`, 'secondary');
  button.onclick = async () => {
    button.disabled = true;
    try { await api(`/api/workers/${pid}/terminate`, {method:'POST'}); message(`已请求终止 PID ${pid}`); await refresh(); }
    catch (error) { message(error.message); }
    finally { button.disabled = false; }
  };
  return button;
}

function selectJob(ticket) {
  currentTicket = ticket;
  currentJob = null;
  $('#tile-select').value = '';
  refresh().catch(error => message(error.message));
}

function renderJob(job) {
  const changed = !currentJob || currentJob.state !== job.state || currentJob.ticket !== job.ticket || currentJob.status?.events?.length !== job.status?.events?.length;
  currentJob = job;
  $('#result-title').textContent = `${operationNames[job.operation]} · ${stateNames[job.state] || job.state}`;
  const status = job.status;
  $('#job-details').textContent = job.error || (status ? `${job.owner} · ${job.job_id} · ${status.tile_complete}/${status.tile_total} 块已提交` : `${job.owner} · 提交处理中`);
  $('#processing').hidden = !['processing','queued','running'].includes(job.state);
  $('#empty').hidden = ['processing','queued','running','complete'].includes(job.state);
  $('#preview-wrap').hidden = !status?.events?.length;
  $('#download').hidden = job.state !== 'complete';
  if (!changed) return;
  const select = $('#tile-select');
  const previousTile = select.value;
  select.replaceChildren(node('option', '全部图块'));
  select.firstChild.value = '';
  if (status) {
    const ids = [...new Set(status.events.map(event => event.tile_id))].sort((a,b) => a-b);
    ids.forEach(id => { const option = node('option', `图块 ${id}`); option.value = id; select.append(option); });
    if (ids.map(String).includes(previousTile)) select.value = previousTile;
  }
  if (job.state === 'complete') {
    const base = `/api/jobs/${job.ticket}`;
    $('#preview').src = `${base}/preview`;
    $('#preview').onload = drawGrid;
    $('#download').href = `${base}/download`;
  }
  renderHistory();
  renderTimeline();
}

function renderHistory() {
  const body = $('#history');
  body.replaceChildren();
  const tile = $('#tile-select').value;
  const events = currentJob?.status?.events || [];
  events.filter(event => tile === '' || String(event.tile_id) === tile).forEach(event => {
    const row = node('tr');
    [event.tile_id, event.attempt_id, event.worker_pid, eventNames[event.event] || event.event,
      new Date(event.timestamp_ms).toLocaleTimeString('zh-CN', {hour12:false}) + '.' + String(event.timestamp_ms % 1000).padStart(3,'0')]
      .forEach(value => row.append(node('td', value)));
    body.append(row);
  });
  drawGrid();
}

function drawGrid() {
  const image = $('#preview');
  const canvas = $('#grid');
  const status = currentJob?.status;
  if (!status?.events?.length) return;
  const allTiles = status.events.filter(event => event.width && event.height);
  canvas.width = status.width || Math.max(...allTiles.map(tile => tile.x + tile.width));
  canvas.height = status.height || Math.max(...allTiles.map(tile => tile.y + tile.height));
  if (!canvas.width || !canvas.height) return;
  const context = canvas.getContext('2d');
  context.fillStyle = '#e7eff2';
  context.fillRect(0, 0, canvas.width, canvas.height);
  if (currentJob.state === 'complete' && image.complete && image.naturalWidth && image.src.includes(currentJob.ticket)) {
    context.drawImage(image, 0, 0);
  } else {
    const ticket = currentJob.ticket;
    status.events.filter(event => event.event === 'tile_committed').forEach(tile => {
      const key = `${ticket}/${tile.tile_id}`;
      let bitmap = tileImages.get(key);
      if (!bitmap) {
        bitmap = new Image();
        tileImages.set(key, bitmap);
        bitmap.onload = () => { if (currentJob?.ticket === ticket) drawGrid(); };
        bitmap.src = `/api/jobs/${ticket}/tiles/${tile.tile_id}`;
      }
      if (bitmap.complete && bitmap.naturalWidth) context.drawImage(bitmap, tile.x, tile.y);
    });
  }
  const selected = $('#tile-select').value;
  const tiles = new Map();
  currentJob.status.events.forEach(event => { if (event.width && event.height) tiles.set(event.tile_id, event); });
  tiles.forEach(tile => {
    context.strokeStyle = 'rgba(25,116,101,.55)';
    context.lineWidth = Math.max(1, canvas.width / 800);
    context.strokeRect(tile.x + .5, tile.y + .5, tile.width - 1, tile.height - 1);
    if (String(tile.tile_id) === selected) {
      context.fillStyle = 'rgba(246,181,67,.35)';
      context.fillRect(tile.x, tile.y, tile.width, tile.height);
    }
  });
}

$('#grid').onclick = event => {
  const canvas = $('#grid');
  const rect = canvas.getBoundingClientRect();
  const x = (event.clientX - rect.left) * canvas.width / rect.width;
  const y = (event.clientY - rect.top) * canvas.height / rect.height;
  const tile = currentJob?.status?.events.find(item => x >= item.x && x < item.x + item.width && y >= item.y && y < item.y + item.height);
  if (tile) { $('#tile-select').value = tile.tile_id; renderHistory(); }
};

function renderTimeline() {
  const timeline = $('#timeline');
  timeline.replaceChildren();
  const events = currentJob?.status?.events || [];
  const controls = $('#worker-controls');
  controls.replaceChildren();
  if (currentUser?.role === 'admin') {
    const busy = events.filter(event => event.event === 'tile_dispatched' && !events.some(end =>
      end.worker_pid === event.worker_pid && end.tile_id === event.tile_id && end.attempt_id === event.attempt_id && ['tile_committed','worker_exit'].includes(end.event)));
    [...new Set(busy.map(event => event.worker_pid))].forEach(pid => {
      controls.append(killButton(pid));
    });
  }
  if (!events.length) { timeline.append(node('p', '暂无记录', 'muted small')); return; }
  const start = Math.min(...events.map(event => event.timestamp_ms));
  const duration = Math.max(1, Math.max(...events.map(event => event.timestamp_ms)) - start);
  const pids = [...new Set(events.map(event => event.worker_pid))];
  pids.forEach(pid => {
    const row = node('div', undefined, 'time-row');
    row.append(node('span', `PID ${pid}`, 'pid'));
    const track = node('div', undefined, 'track');
    events.filter(event => event.worker_pid === pid && event.event === 'tile_dispatched').forEach(dispatched => {
      const end = events.find(event => event.worker_pid === pid && event.tile_id === dispatched.tile_id && event.attempt_id === dispatched.attempt_id && ['tile_committed','worker_exit'].includes(event.event));
      const bar = node('div', String(dispatched.tile_id), `bar${dispatched.attempt_id > 1 ? ' retry' : ''}`);
      bar.style.left = `${(dispatched.timestamp_ms - start) / duration * 98}%`;
      bar.style.width = `${Math.max(.5, ((end?.timestamp_ms || dispatched.timestamp_ms) - dispatched.timestamp_ms) / duration * 98)}%`;
      bar.title = `图块 ${dispatched.tile_id}，尝试 ${dispatched.attempt_id}，${end ? end.timestamp_ms - dispatched.timestamp_ms : '?'} ms`;
      track.append(bar);
    });
    row.append(track);
    timeline.append(row);
  });
}

$('#login-form').onsubmit = async event => {
  event.preventDefault();
  try { showUser(await api('/api/login', {method:'POST', body:JSON.stringify(Object.fromEntries(new FormData(event.target)))})); }
  catch (error) { message(error.message); }
};
$('#submit-form').onsubmit = async event => {
  event.preventDefault();
  const form = event.target;
  const file = form.elements.namedItem('image').files[0];
  if (!file || file.size > 16 * 1024 * 1024) { message('请选择 16 MiB 以下的 PPM 图片'); return; }
  const button = form.querySelector('button');
  button.disabled = true;
  try {
    const job = await api(`/api/jobs?operation=${form.elements.namedItem('operation').value}&priority=${form.elements.namedItem('priority').value}&fault=${form.elements.namedItem('fault').checked ? 1 : 0}`, {method:'POST', body:await file.arrayBuffer()});
    currentTicket = job.ticket;
    currentJob = null;
    await refresh();
  } catch (error) { message(error.message); }
  finally { button.disabled = false; }
};
$('#refresh').onclick = () => refresh().catch(error => message(error.message));
$('#tile-select').onchange = renderHistory;
setInterval(() => { if (currentUser) refresh().catch(error => message(error.message)); }, 2000);
api('/api/me').then(showUser).catch(() => showUser(null));
