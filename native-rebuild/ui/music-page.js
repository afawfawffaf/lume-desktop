import {mountMusicVisualizer, MUSIC_VISUALIZERS} from './music-visualizer.js';
import {mountMusicEffects} from './music-effects.js';
import {createMusicFontAssets, createMusicFontGate} from './music-font-assets.js';
import {musicPreferenceSnapshot, DEFAULT_MUSIC_PREFERENCES} from './music-preference-values.js';
import {mountMusicCardMenu} from './music-page-menu.js';
import {mountMusicBackground} from './music-background-view.js';

// The desktop card is a second, independent view of the same native playback.
// It never owns state: every frame it shows is a message from the shell. The
// card is a lyric surface only -- the same stage the immersive player draws, on
// a transparent glass pane, with no chrome but the close button.
const webview = globalThis.chrome?.webview;
const root = document.getElementById('music-page');

const state = {
  snapshot: null,
  lyrics: [],
  preferences: musicPreferenceSnapshot(undefined, DEFAULT_MUSIC_PREFERENCES),
  // The revision the preferences were last confirmed at; a save quotes it so a
  // concurrent edit from the main window is rejected instead of overwritten.
  preferenceRevision: 0,
  backgroundRevision: 0,
  // The card draws the same background media the main window selected.
  background: null,
  receivedAt: performance.now(),
  material: 'fallback',
};
let preferenceRequest = 0;
let backgroundRequest = 0;
let saveTimer = null;
let backgroundRenderer = null;
let backgroundCanvas = null;

const send = message => {
  try {
    webview?.postMessage(message);
  } catch {}
};

const windowAction = (action, edge) => {
  if (!action) return;
  send(edge ? {version: 1, type: 'music-page-window', action, edge} : {version: 1, type: 'music-page-window', action});
};

// No chrome at all: the card is a lyric picture, and everything it can do is in
// the right-click menu. That keeps the artwork clean and removes every control
// the stage could steal a gesture from.
root.innerHTML = `
  <div class="MusicPageEffects" data-music-page-effects aria-hidden="true"></div>
  <div class="MusicPageStage" data-music-page-stage aria-label="歌词"></div>`;

const stage = root.querySelector('[data-music-page-stage]');
const effectHost = root.querySelector('[data-music-page-effects]');

const fontAssets = createMusicFontAssets();
const ensureFont = (document, family, options) => fontAssets.ensureFont(document, family, options);
const gate = createMusicFontGate(document, {ensureFont, onReady: () => draw(true)});

const visualizer = mountMusicVisualizer(stage, {ensureFont, onError: () => {}});
const effects = mountMusicEffects(effectHost, {onError: () => {}});

// The card runs its own clock between messages so the stage advances at the
// same cadence as the audio, exactly like the main window's stage does.
function projectedTime() {
  const snapshot = state.snapshot;
  if (!snapshot) return 0;
  const base = snapshot.timeline?.position || 0;
  const playing = snapshot.playing === true;
  const elapsedSeconds = playing ? Math.max(0, (performance.now() - state.receivedAt) / 1000) : 0;
  const total = Math.min(snapshot.timeline?.duration || 86400, base + Math.min(elapsedSeconds, 1));
  return Number.isFinite(total) ? total : 0;
}

// No page-driven frame loop: mountMusicVisualizer already runs its own clock
// and re-projects between native updates. Adding a second requestAnimationFrame
// loop made every frame arrive with syncClock set, which reset the stage's clock
// and made each lyric word flash instead of scrolling.
function draw(syncClock = false) {
  const snapshot = state.snapshot;
  if (!snapshot) return;
  // musicPreferenceSnapshot() already returns the bare validated record, so
  // effectMode and animationIntensity are read straight off it.
  const preferences = state.preferences;
  const visible = document.visibilityState !== 'hidden';
  const current = snapshot.current;
  const next = {
    current,
    lines: state.lyrics,
    currentTime: projectedTime(),
    duration: snapshot.timeline?.duration || 0,
    playing: snapshot.playing === true,
    visible,
    analysis: snapshot.analysis || null,
    // The font gate resolves the family and returns the record the stage draws
    // with; every other field is carried through unchanged.
    preferences: gate.preferences(preferences, {visible: true}),
  };
  // A new snapshot resets the clock; lyric and preference updates must not.
  visualizer.update(next, preferences.mode, {syncClock});
  effects.update({
    mode: preferences.effectMode || 'none',
    visualizer: preferences.mode,
    visible,
    // The rain-window effect paints through the background scene, exactly as
    // the main window's stage does; without it that effect has nothing to draw.
    scene: backgroundRenderer?.getScene?.() || null,
    current,
  });
}

// No transport controls and no close button: the card is a lyric surface, and
// everything it can do lives in the right-click menu below.

// The card body is a drag surface and the outer band is a resize surface. The
// stage above it is pointer-transparent (its iframe would otherwise swallow
// every press), so these pointers actually reach the page. A double-click on the
// body closes the card, exactly like the widget's own double-click opens it.
const DOUBLE_CLICK_MS = 260;
const DRAG_ARM_MS = 220;
const EDGE_BAND = 8;
let dragTimer = null;
let pressTimer = null;
let pressAt = null;
let pressOrigin = null;

// Returns the native resize edge under the pointer, or '' for the card body.
function edgeAt(x, y) {
  const box = root.getBoundingClientRect();
  const left = x - box.left <= EDGE_BAND;
  const right = box.right - x <= EDGE_BAND;
  const top = y - box.top <= EDGE_BAND;
  const bottom = box.bottom - y <= EDGE_BAND;
  if (top && left) return 'nw';
  if (top && right) return 'ne';
  if (bottom && left) return 'sw';
  if (bottom && right) return 'se';
  if (left) return 'w';
  if (right) return 'e';
  if (top) return 'n';
  if (bottom) return 's';
  return '';
}

// The cursor is the only affordance the card has left, so the band has to say
// what it does before the user commits to the drag.
const CURSORS = {
  nw: 'nwse-resize', se: 'nwse-resize', ne: 'nesw-resize', sw: 'nesw-resize',
  n: 'ns-resize', s: 'ns-resize', e: 'ew-resize', w: 'ew-resize', '': 'grab',
};
root.addEventListener('pointermove', event => {
  const edge = edgeAt(event.clientX, event.clientY);
  const cursor = CURSORS[edge] || 'grab';
  // grab while idle, grabbing while a native move is in flight.
  if (root.style.cursor !== cursor) root.style.cursor = cursor;
});

const cancelPress = () => {
  if (pressTimer !== null) clearTimeout(pressTimer);
  pressTimer = null;
  pressAt = null;
  pressOrigin = null;
};

root.addEventListener('pointerdown', event => {
  if (event.button !== 0 || event.target.closest('button, input')) return;
  // An edge press is a resize, never a move and never a double-click.
  const edge = edgeAt(event.clientX, event.clientY);
  if (edge) {
    cancelPress();
    if (dragTimer !== null) clearTimeout(dragTimer);
    dragTimer = null;
    windowAction('resize', edge);
    return;
  }
  const now = performance.now();
  const point = {x: event.clientX, y: event.clientY};
  const near = pressOrigin && Math.abs(point.x - pressOrigin.x) <= 8 && Math.abs(point.y - pressOrigin.y) <= 8;
  if (pressAt !== null && now - pressAt <= DOUBLE_CLICK_MS && near) {
    // Second press of a double-click: put the card away instead of dragging.
    cancelPress();
    if (dragTimer !== null) clearTimeout(dragTimer);
    dragTimer = null;
    windowAction('close');
    return;
  }
  cancelPress();
  pressAt = now;
  pressOrigin = point;
  pressTimer = setTimeout(() => {
    pressTimer = null;
  }, DOUBLE_CLICK_MS);
  if (dragTimer !== null) clearTimeout(dragTimer);
  dragTimer = setTimeout(() => {
    dragTimer = null;
    // Still the same press: hand it to the native move loop.
    if (pressAt !== null) windowAction('drag');
  }, DRAG_ARM_MS);
});

for (const name of ['pointerup', 'pointercancel']) {
  root.addEventListener(name, () => {
    if (dragTimer !== null) clearTimeout(dragTimer);
    dragTimer = null;
  });
}

// Everything the card can change lives here. The menu only reports the choice;
// the shell owns persistence, because the same preferences are shared with the
// main window and must keep one revision.
// The card draws the same background media the main window selected. It is a
// plain draw behind the stage: the compositor owns decoding, scaling and video,
// so the card never re-implements any of it and a clear background is simply a
// removed canvas.
//
// The canvas is created whenever the rain-window effect could need it: that
// effect paints through the background scene, so it has to exist even when the
// user has not chosen a background of their own. Without media the compositor
// draws the bundled default, which is exactly what the main window shows.
const RAIN_SCENE = 'rain-window';

function drawBackground() {
  const media = state.background;
  const sceneWanted = (state.preferences.effectMode || 'none') === RAIN_SCENE;
  if (!media?.url && !sceneWanted) {
    backgroundRenderer?.dispose?.();
    backgroundRenderer = null;
    backgroundCanvas?.remove();
    backgroundCanvas = null;
    return;
  }
  if (!backgroundCanvas) {
    backgroundCanvas = document.createElement('canvas');
    backgroundCanvas.className = 'MusicPageBackdrop';
    backgroundCanvas.setAttribute('aria-hidden', 'true');
    root.prepend(backgroundCanvas);
  }
  const visible = document.visibilityState !== 'hidden';
  if (!backgroundRenderer) {
    backgroundRenderer = mountMusicBackground(backgroundCanvas, {
      media: media || null, visible, onError: () => {},
    });
  } else {
    backgroundRenderer.update({media: media || null, visible});
  }
}

const menu = mountMusicCardMenu(root, {
  onPick(patch) {
    // The card applies the change immediately so the stage reacts while the
    // shell writes it; the next preferences frame confirms or corrects it.
    state.preferences = musicPreferenceSnapshot(state.preferences, {...state.preferences, ...patch});
    // The background canvas exists only when media or the rain effect needs it,
    // so a change to the effect has to re-evaluate it before the next draw.
    drawBackground();
    draw(false);
    // A colour drag fires continuously; only the last value of a burst is saved.
    schedulePreferenceSave();
  },
  onBackground(action) {
    state.backgroundRevision = state.backgroundRevision || 0;
    send({
      version: 1, scope: 'music-background', action,
      request: ++backgroundRequest, revision: state.backgroundRevision,
    });
  },
  onCommand(name) {
    if (name === 'close') windowAction('close');
    else if (name === 'settings') send({version: 1, type: 'music-page-window', action: 'settings'});
  },
});

function savePreferences() {
  if (!Number.isSafeInteger(state.preferenceRevision) || state.preferenceRevision < 0) return;
  send({
    version: 1, scope: 'music-preferences', action: 'apply',
    request: ++preferenceRequest, revision: state.preferenceRevision, preferences: state.preferences,
  });
}

function schedulePreferenceSave() {
  if (saveTimer !== null) clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    saveTimer = null;
    savePreferences();
  }, 160);
}

function applyState(message) {
  if (!message || message.version !== 1) return;
  if (message.type === 'music-state') {
    state.snapshot = message;
    state.receivedAt = performance.now();
    draw(true);
  } else if (message.type === 'music-lyrics') {
    // Lyrics are per track; a message for another track than the one on screen
    // is stale and must not replace the stage.
    if (!state.snapshot || message.trackId !== state.snapshot.current?.id) return;
    state.lyrics = Array.isArray(message.lines) ? message.lines : [];
    draw(false);
  } else if (message.type === 'music-preferences-state') {
    if (!Number.isSafeInteger(message.revision)) return;
    state.preferenceRevision = message.revision;
    // A rejected save leaves the previous preferences in place; the next frame
    // carries the authority, so the stage never keeps an unsaved change.
    state.preferences = musicPreferenceSnapshot(state.preferences, message.preferences);
    menu.setPreferences(state.preferences);
    drawBackground();
    draw(false);
  } else if (message.type === 'music-background-state') {
    if (!Number.isSafeInteger(message.revision)) return;
    state.backgroundRevision = message.revision;
    state.background = message.media || null;
    menu.setBackground(state.background);
    drawBackground();
  } else if (message.type === 'music-page-material') {
    state.material = message.status || 'fallback';
    root.dataset.musicPageMaterial = state.material;
  }
}

webview?.addEventListener?.('message', event => {
  try {
    applyState(typeof event.data === 'string' ? JSON.parse(event.data) : event.data);
  } catch {}
});

document.addEventListener('visibilitychange', () => draw(false));

// Ask once for the current frame; the shell answers with a snapshot, lyrics and
// the saved preferences at the same revision as the main window.
send({version: 1, scope: 'music', action: 'snapshot'});
send({version: 1, scope: 'music-preferences', action: 'snapshot'});
send({version: 1, scope: 'music-background', action: 'snapshot'});
send({version: 1, type: 'music-page-window', action: 'ready'});
