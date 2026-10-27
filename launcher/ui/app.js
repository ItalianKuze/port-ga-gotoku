// SPDX-License-Identifier: GPL-2.0-or-later
// PortGaGotoku launcher window. Every text is written in English and looked up in the table
// pgg_lang.py sends (t(<English text>) here, data-t / data-t-label in index.html); tests/test_launcher.py
// checks that each one has an Italian entry.
'use strict';

const $ = (id) => document.getElementById(id);
let table = {};
let state = null;
let logPosition = 0;
let logText = '';
let drawerWanted = false;   // the user opened the log
let errorShown = null;      // the exit code whose error opened the drawer
let welcomeForced = false;  // "Change..." opened the welcome page over a playable game
let browsedOnce = false;
let bannerFor = null;

const t = (english) => table[english] || english;
const fmt = (text, ...values) => values.reduce((out, value) => out.replace('{}', value), text);

const CHECK = '<svg viewBox="0 0 24 24" aria-hidden="true"><polyline points="5 12.5 10 17 19 7"></polyline></svg>';
const DOT = '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="3"></circle></svg>';
const ALERT = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 4 2.8 19.5h18.4z"></path><line x1="12" y1="10" x2="12" y2="14"></line></svg>';
const CROSS = '<svg viewBox="0 0 24 24" aria-hidden="true"><line x1="6" y1="6" x2="18" y2="18"></line><line x1="18" y1="6" x2="6" y2="18"></line></svg>';
const ICONS = { ok: CHECK, wait: DOT, warn: ALERT, bad: CROSS };

function translateStatic() {
  document.querySelectorAll('[data-t]').forEach((el) => { el.textContent = t(el.dataset.t); });
  document.querySelectorAll('[data-t-label]').forEach((el) => { el.setAttribute('aria-label', t(el.dataset.tLabel)); el.title = t(el.dataset.tLabel); });
}

async function loadTexts() {
  const texts = await pywebview.api.texts();
  table = texts.table || {};
  document.documentElement.lang = texts.lang;
  translateStatic();
}

// ---- game folder messages ------------------------------------------------------------------
function gameMessage(game) {
  switch (game.state) {
    case 'ok': return ['ok', fmt(t('Found: {} ({}), version {}'), game.title, game.title_id, game.version)];
    case 'untested_version': return ['warn', fmt(t('Found {} version {}: only version {} has been tried'), game.title_id, game.version, game.tested_version)];
    case 'empty': return ['wait', t('No folder chosen yet')];
    case 'missing': return ['bad', t('The game folder does not exist any more: choose it again')];
    case 'no_eboot': return ['bad', t('No eboot.bin in this folder: it is not the game folder')];
    case 'no_sfo': return ['bad', t('No readable sce_sys\\param.sfo in this folder: it is not a complete PS4 dump')];
    case 'other_title': return ['bad', fmt(t('This folder holds {} ({}), not CUSA12781. If it is another edition of the game, you can try it as an experiment.'), game.title, game.title_id)];
    case 'experimental': return ['warn', fmt(t('{} ({}) version {}: not supported, started as an experiment'), game.title, game.title_id, game.version)];
    default: return ['bad', game.state];
  }
}

// ---- rendering -----------------------------------------------------------------------------
function statusItem(kind, text) {
  const li = document.createElement('li');
  li.className = kind;
  li.innerHTML = ICONS[kind];
  li.append(document.createTextNode(text));
  return li;
}

function renderHome(s) {
  const game = s.game;
  const list = $('status');
  list.replaceChildren();
  const gameKind = game.state === 'ok' ? 'ok' : 'warn';
  list.append(statusItem(gameKind, game.state === 'ok'
    ? fmt(t('Game ready · {} v{}'), game.title_id, game.version)
    : game.state === 'experimental'
      ? fmt(t('{} version {} (experimental, not supported)'), game.title_id, game.version)
      : fmt(t('{} version {} (not tried)'), game.title_id, game.version)));
  if (!s.devices) {
    list.append(statusItem('wait', t('Controller: checking…')));
    list.append(statusItem('wait', t('Graphics card: checking…')));
  } else {
    const pads = s.devices.gamepads;
    list.append(pads.length ? statusItem('ok', pads.join(', ')) : statusItem('wait', t('No controller (the keyboard works)')));
    const gpus = s.devices.gpus;
    const chosen = s.settings.gpu;
    if (!gpus.length) list.append(statusItem('bad', t('No Vulkan graphics card')));
    else if (chosen && !gpus.some((g) => g.name === chosen)) list.append(statusItem('warn', fmt(t('{} not found'), chosen)));
    else {
      const auto = (gpus.find((g) => g.kind === 'discrete') || gpus[0]).name;
      list.append(statusItem('ok', shortGpu(chosen || auto)));
    }
  }
  list.append(statusItem(s.saves.length ? 'ok' : 'wait', s.saves.length === 1 ? t('1 save') :
    s.saves.length ? fmt(t('{} saves'), s.saves.length) : t('No saves yet')));

  $('summary').textContent = [s.settings.fullscreen ? t('Fullscreen') : t('Window'), s.settings.present_mode].join(' · ');
  const playing = s.phase !== 'idle';
  const failed = !playing && s.last_exit !== null && s.last_exit !== 0 && !s.stopped;
  $('play').disabled = playing;
  $('play-label').textContent = s.phase === 'preparing' ? t('Preparing…') : s.phase === 'running' ? t('Running')
    : s.phase === 'stopping' ? t('Closing…') : failed ? t('Play again') : t('Play');
  $('stop').hidden = s.phase !== 'running';

  const note = $('note');
  note.className = 'note';
  if (s.phase === 'preparing') note.textContent = t('Preparing the boot image (a few seconds)…');
  else if (s.phase === 'running') note.textContent = t('The game is running in its own window. In the game, Insert or L3+R3 opens the port menu.');
  else if (s.phase === 'stopping') note.textContent = t('Closing the game: a save in progress is finished first…');
  else if (s.last_exit === 0) note.textContent = t('Last game: closed normally.');
  else if (s.last_exit !== null && s.stopped) note.textContent = fmt(t('Last game: stopped from the launcher (code {}).'), s.last_exit);
  else if (failed && s.last_exit === s.exit_no_memory) { note.textContent = t('Not enough memory to start the game: it reserves about 6 GB and Windows could not give them. Close other heavy applications, or enlarge the Windows page file, and try again.'); note.className = 'note bad'; }
  else if (failed) { note.textContent = fmt(t('Last game: ended with an error (code {}).'), s.last_exit); note.className = 'note bad'; }
  else if (!game.prepared) note.textContent = t('At the first start the boot image is prepared (a few seconds).');
  else note.textContent = '';

  // An abnormal end opens the log by itself, once per exit.
  if (failed && errorShown !== s.last_exit) { errorShown = s.last_exit; drawerWanted = true; }
  if (playing) errorShown = null;
}

function shortGpu(name) { return name.replace(/^NVIDIA GeForce /, '').replace(/^AMD /, ''); }

function renderWelcome(s) {
  const game = s.game;
  $('welcome-lead').innerHTML = '';
  $('welcome-lead').append(document.createTextNode(t('To play Fist of the North Star: Lost Paradise you need your own dump of the game: the folder ')));
  const strong = document.createElement('strong');
  strong.textContent = 'CUSA12781';
  $('welcome-lead').append(strong, document.createTextNode(t(' with eboot.bin and sce_sys.')));
  const path = $('welcome-path');
  const shown = state.chosen || s.settings.game_dir;
  path.textContent = shown || t('No folder chosen');
  const shownGame = state.chosenResult || game;
  const [kind, text] = gameMessage(shownGame);
  path.className = 'path ' + (kind === 'ok' || kind === 'warn' ? 'ok' : kind === 'bad' ? 'bad' : '');
  const message = $('welcome-message');
  message.className = 'message ' + kind;
  message.textContent = kind === 'wait' ? '' : text;
  $('welcome-continue').disabled = !game.playable;
  $('welcome-experimental').hidden = shownGame.state !== 'other_title';
}

function renderSettings(s) {
  const modes = $('present-mode');
  if (!modes.childElementCount) {
    s.present_modes.forEach((mode) => {
      const b = document.createElement('button');
      b.type = 'button'; b.setAttribute('role', 'radio'); b.textContent = mode; b.dataset.mode = mode;
      b.addEventListener('click', () => setSetting('present_mode', mode));
      modes.append(b);
    });
  }
  modes.querySelectorAll('button').forEach((b) => b.setAttribute('aria-checked', String(b.dataset.mode === s.settings.present_mode)));
  $('fullscreen').setAttribute('aria-checked', String(!!s.settings.fullscreen));
  $('frame-stats').setAttribute('aria-checked', String(!!s.settings.frame_stats));
  const gpu = $('gpu');
  const options = [['', s.devices && s.devices.gpus.length
    ? fmt(t('Automatic ({})'), shortGpu((s.devices.gpus.find((g) => g.kind === 'discrete') || s.devices.gpus[0]).name))
    : t('Automatic')]].concat((s.devices ? s.devices.gpus : []).map((g) => [g.name, g.name]));
  if (gpu.dataset.key !== JSON.stringify(options)) {
    gpu.replaceChildren(...options.map(([value, text]) => new Option(text, value)));
    gpu.dataset.key = JSON.stringify(options);
  }
  if (document.activeElement !== gpu) gpu.value = s.settings.gpu;
  $('game-path').textContent = s.settings.game_dir || t('No folder chosen');
  if (document.activeElement !== $('language')) $('language').value = s.settings.ui_language;
  $('panel-build').textContent = s.build ? 'Build ' + s.build : '';
}

function renderDrawer(s) {
  const failed = s.phase === 'idle' && s.last_exit !== null && s.last_exit !== 0 && !s.stopped;
  $('drawer').hidden = !drawerWanted;
  $('drawer-alert').hidden = !failed;
  const title = $('drawer-title');
  title.className = 'drawer-title' + (failed ? ' bad' : '');
  title.textContent = failed ? fmt(t('The game closed with an error (code {})'), s.last_exit) : t('Log');
  $('drawer-sub').textContent = s.log_name || t('No game started from this window yet');
}

function render() {
  if (!state) return;
  const s = state.s;
  const showWelcome = !s.game.playable || welcomeForced;
  $('home').hidden = showWelcome;
  $('welcome').hidden = !showWelcome;
  $('open-log').hidden = showWelcome && !s.log_name;
  if (showWelcome) renderWelcome(s); else renderHome(s);
  renderSettings(s);
  renderDrawer(s);
  if (!$('saves').hidden) renderSavesBusy();
  $('build').textContent = s.build ? 'Build ' + s.build : '';
  if (bannerFor !== s.settings.game_dir && s.game.playable) loadBanner(s.settings.game_dir);
  // First start: the folder picker opens by itself, once.
  if (s.game.state === 'empty' && !browsedOnce) { browsedOnce = true; setTimeout(browse, 350); }
}

async function loadBanner(folder) {
  bannerFor = folder;
  const url = await pywebview.api.banner();
  $('art').style.backgroundImage = url ? `url("${url}")` : '';
}

// ---- log -----------------------------------------------------------------------------------
function appendLog(text, reset) {
  if (reset) { logText = ''; }
  logText += text;
  if (logText.length > 400000) logText = logText.slice(-400000);
  const log = $('log');
  const atEnd = log.scrollTop + log.clientHeight >= log.scrollHeight - 8;
  if (reset) log.replaceChildren();
  for (const line of text.split(/(?<=\n)/)) {
    const span = document.createElement('span');
    if (/STOP|ERROR|Fault|WARNING|FAILED|exit code/.test(line)) span.className = 'err';
    span.textContent = line;
    log.append(span);
  }
  while (log.childNodes.length > 6000) log.firstChild.remove();
  if (atEnd || reset) log.scrollTop = log.scrollHeight;
}

// ---- polling -------------------------------------------------------------------------------
async function poll() {
  try {
    const s = await pywebview.api.state();
    const chunk = await pywebview.api.log_since(logPosition);
    if (chunk.text || chunk.reset) appendLog(chunk.text, chunk.reset);
    logPosition = chunk.position;
    state = Object.assign(state || {}, { s });
    render();
  } catch (error) {
    console.error(error);
  }
  setTimeout(poll, 500);
}

// ---- actions -------------------------------------------------------------------------------
async function setSetting(key, value) {
  await pywebview.api.set_setting(key, value);
  if (key === 'ui_language') await loadTexts();
  await refresh();
}

async function refresh() {
  state = Object.assign(state || {}, { s: await pywebview.api.state() });
  render();
}

async function browse() {
  const result = await pywebview.api.browse_game();
  if (!result.chosen) return;
  state.chosen = result.chosen;
  state.chosenResult = { state: result.state, title_id: result.title_id, title: result.title, version: result.version, tested_version: '' };
  if (['ok', 'untested_version', 'experimental'].includes(result.state)) { state.chosen = null; state.chosenResult = null; bannerFor = null; }
  await refresh();
}

// Another edition of the game (another TITLE_ID): only after an explicit confirmation.
async function tryExperimental() {
  const folder = state.chosen || state.s.settings.game_dir;
  const titleId = (state.chosenResult || state.s.game).title_id;
  if (!folder || !confirm(fmt(t('Only CUSA12781 is supported. {} has never been tried: it may not start, or it may have errors. If you send a log, it tells which version this is.\n\nTry it anyway?'), titleId))) return;
  if (!await pywebview.api.try_experimental(folder)) return;
  state.chosen = null; state.chosenResult = null; bannerFor = null;
  await refresh();
}

async function play() {
  const answer = await pywebview.api.play();
  if (!answer.ok) return;
  if (answer.running.length && !confirm(fmt(t('bb-probe.exe is already running (PID {}). Two games on the same saves can damage them.\n\nStart anyway?'), answer.running.join(', ')))) return;
  logPosition = 0;
  appendLog('', true);
  await pywebview.api.start();
  await refresh();
}

// ---- Saves page ------------------------------------------------------------------------------
let exportWanted = null;  // {slot, dest, shadps4} while an export is previewed

function when(seconds) {
  const d = new Date(seconds * 1000);
  const two = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${two(d.getMonth() + 1)}-${two(d.getDate())} ${two(d.getHours())}:${two(d.getMinutes())}`;
}

function saveItem(slot, label, sub, action, onClick) {
  const item = document.createElement('div');
  item.className = 'item';
  const number = document.createElement('span');
  number.className = 'slot';
  number.textContent = slot;
  const text = document.createElement('div');
  text.className = 'text';
  const main = document.createElement('span');
  main.className = 'label';
  main.textContent = label;
  const hint = document.createElement('span');
  hint.className = 'hint';
  hint.textContent = sub;
  text.append(main, hint);
  const button = document.createElement('button');
  button.type = 'button'; button.className = 'pill save-action'; button.textContent = action;
  button.addEventListener('click', onClick);
  item.append(number, text, button);
  return item;
}

const backupKind = (kind) => ({ game: t('Before the game saved'), import: t('Before an import'), restore: t('Before a restore') }[kind]);

async function loadSaves() {
  const data = await pywebview.api.saves();
  const slots = $('slot-list');
  slots.replaceChildren(...data.slots.map((s) => saveItem(s.slot, s.subtitle || s.title || s.slot,
    [s.detail, when(s.mtime), fmt(t('{} files'), s.files)].filter(Boolean).join(' · '), t('Export…'), () => exportSlot(s.slot))));
  if (!data.slots.length) slots.replaceChildren(Object.assign(document.createElement('div'), { className: 'empty', textContent: t('No saves yet') }));
  const backups = $('backup-list');
  backups.replaceChildren(...data.backups.map((b) => saveItem(b.slot, `${when(b.when)} · ${backupKind(b.kind)}`,
    [b.subtitle, b.detail].filter(Boolean).join(' · '), t('Restore…'), () => previewSave(pywebview.api.save_preview_restore(b.name)))));
  if (!data.backups.length) backups.replaceChildren(Object.assign(document.createElement('div'), { className: 'empty', textContent: t('No backups yet') }));
  renderSavesBusy();
}

function renderSavesBusy() {
  const busy = !!state && state.s.phase !== 'idle';
  document.querySelectorAll('.save-action, #save-import, #preview-confirm').forEach((b) => { b.disabled = busy; });
  const closeFirst = t('Close the game first: saves change only with the game closed.');
  if (busy && !$('saves').hidden) saveMessage('warn', closeFirst);
  else if (!busy && $('save-message').textContent === closeFirst) saveMessage('', '');
}

function saveMessage(kind, text) {
  const message = $('save-message');
  message.className = 'message ' + kind;
  message.textContent = text;
}

const previewTitle = (kind) => ({ import: t('Import into slot {}'), restore: t('Restore slot {}'), export: t('Export slot {}') }[kind]);

async function previewSave(request) {
  saveMessage('', '');
  const p = await request;
  if (!p.ok) { $('save-preview').hidden = true; saveMessage('bad', p.error); return; }
  $('preview-title').textContent = fmt(previewTitle(p.kind), p.slot);
  const facts = [[p.subtitle, ''], [p.detail, ''], [fmt(t('{} files'), p.files), ''], [fmt(t('From: {}'), p.source), ''],
    [fmt(t('To: {}'), p.target), '']];
  if (p.kind === 'export') facts.push([t('Nothing is overwritten: the destination folder must not exist yet.'), '']);
  else if (p.backup) facts.push([fmt(t('The current slot is kept first in: {}'), p.backup), 'warn']);
  else facts.push([fmt(t('Slot {} is empty: nothing to keep.'), p.slot), '']);
  p.warnings.forEach((w) => facts.push([w, 'warn']));
  $('preview-facts').replaceChildren(...facts.filter(([text]) => text).map(([text, kind]) => {
    const li = document.createElement('li'); li.textContent = text; li.className = kind; return li;
  }));
  const layout = $('export-layout');
  layout.hidden = p.kind !== 'export';
  layout.querySelectorAll('button').forEach((b) => b.setAttribute('aria-checked', String(!!exportWanted && (b.dataset.layout === 'shadps4') === exportWanted.shadps4)));
  $('save-preview').hidden = false;
  $('save-preview').dataset.kind = p.kind;
  $('save-preview').dataset.slot = p.slot;
  renderSavesBusy();
}

async function importSave() {
  exportWanted = null;
  const folder = await pywebview.api.save_choose_folder();
  if (folder) await previewSave(pywebview.api.save_preview_import(folder));
}

async function exportSlot(slot) {
  const dest = await pywebview.api.save_choose_folder();
  if (!dest) return;
  exportWanted = { slot, dest, shadps4: false };
  await previewSave(pywebview.api.save_preview_export(slot, dest, false));
}

const doneText = (kind) => ({ import: t('Slot {} imported and verified.'), restore: t('Slot {} restored and verified.'), export: t('Slot {} exported and verified.') }[kind]);

async function confirmSave() {
  const box = $('save-preview');
  const result = await pywebview.api.save_apply();
  box.hidden = true;
  exportWanted = null;
  if (result.ok) saveMessage('ok', fmt(doneText(box.dataset.kind), box.dataset.slot));
  else if (result.error === 'running') saveMessage('bad', t('Close the game first: saves change only with the game closed.'));
  else saveMessage('bad', result.error);
  await loadSaves();
  await refresh();
}

function openSaves(open) {
  $('saves').hidden = !open;
  $('backdrop').hidden = !open;
  if (open) { $('save-preview').hidden = true; saveMessage('', ''); loadSaves(); $('close-saves').focus(); }
}

function openSettings(open) {
  $('settings').hidden = !open;
  $('backdrop').hidden = !open;
  if (open) $('close-settings').focus();
}

function toggleFolders(open) {
  const menu = $('folders');
  const show = open === undefined ? menu.hidden : open;
  menu.hidden = !show;
  $('open-folders').setAttribute('aria-expanded', String(show));
}

function wire() {
  $('play').addEventListener('click', play);
  $('stop').addEventListener('click', () => pywebview.api.stop().then(refresh));
  $('welcome-browse').addEventListener('click', browse);
  $('welcome-experimental').addEventListener('click', tryExperimental);
  $('welcome-continue').addEventListener('click', () => { welcomeForced = false; state.chosen = null; state.chosenResult = null; render(); });
  $('change-game').addEventListener('click', () => { openSettings(false); welcomeForced = true; render(); browse(); });
  $('open-settings').addEventListener('click', () => openSettings(true));
  $('close-settings').addEventListener('click', () => openSettings(false));
  $('backdrop').addEventListener('click', () => { openSettings(false); openSaves(false); });
  $('open-saves').addEventListener('click', () => openSaves(true));
  $('close-saves').addEventListener('click', () => openSaves(false));
  $('save-import').addEventListener('click', importSave);
  $('preview-confirm').addEventListener('click', confirmSave);
  $('preview-cancel').addEventListener('click', () => { $('save-preview').hidden = true; exportWanted = null; });
  $('export-layout').querySelectorAll('button').forEach((b) => b.addEventListener('click', () => {
    if (!exportWanted) return;
    exportWanted.shadps4 = b.dataset.layout === 'shadps4';
    previewSave(pywebview.api.save_preview_export(exportWanted.slot, exportWanted.dest, exportWanted.shadps4));
  }));
  $('open-folders').addEventListener('click', (e) => { e.stopPropagation(); toggleFolders(); });
  document.addEventListener('click', () => toggleFolders(false));
  document.querySelectorAll('[data-folder]').forEach((b) => b.addEventListener('click', () => { pywebview.api.open_folder(b.dataset.folder); toggleFolders(false); }));
  $('open-log').addEventListener('click', () => { drawerWanted = !drawerWanted; render(); });
  $('report-bug').addEventListener('click', () => pywebview.api.report_bug());
  $('close-log').addEventListener('click', () => { drawerWanted = false; render(); });
  $('copy-log').addEventListener('click', () => navigator.clipboard.writeText(logText));
  $('fullscreen').addEventListener('click', () => setSetting('fullscreen', !state.s.settings.fullscreen));
  $('frame-stats').addEventListener('click', () => setSetting('frame_stats', !state.s.settings.frame_stats));
  $('gpu').addEventListener('change', (e) => setSetting('gpu', e.target.value));
  $('language').addEventListener('change', (e) => setSetting('ui_language', e.target.value));
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') { openSettings(false); openSaves(false); toggleFolders(false); }
  });
}

window.addEventListener('pywebviewready', async () => {
  wire();
  await loadTexts();
  await refresh();
  poll();
});
