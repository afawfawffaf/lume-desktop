// The desktop music card's settings, mirroring music-desktop.js: the page only
// validates and shapes messages, the native session owns the file.
const exact = (value, keys) =>
  value !== null && typeof value === 'object' && !Array.isArray(value) &&
  Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));

export const MUSIC_PAGE_DEFAULTS = Object.freeze({width: 900, height: 620, minWidth: 480, minHeight: 360, maxWidth: 1600, maxHeight: 1200});

export function validMusicPageCommand(action, fields) {
  if (action === 'snapshot' || action === 'reset') return exact(fields, []);
  return action === 'setSize' && exact(fields, ['width', 'height']) &&
    Number.isInteger(fields.width) && Number.isInteger(fields.height) &&
    fields.width > 0 && fields.height > 0 && fields.width <= 10000 && fields.height <= 10000;
}

export function validMusicPageMessage(message) {
  return exact(message, ['version', 'type', 'open', 'stopped', 'width', 'height', 'minWidth', 'minHeight', 'maxWidth', 'maxHeight']) &&
    message.version === 1 && message.type === 'music-page-state' &&
    typeof message.open === 'boolean' && typeof message.stopped === 'boolean' &&
    ['width', 'height', 'minWidth', 'minHeight', 'maxWidth', 'maxHeight'].every(key =>
      Number.isInteger(message[key]) && message[key] > 0) &&
    message.minWidth <= message.width && message.width <= message.maxWidth &&
    message.minHeight <= message.height && message.height <= message.maxHeight;
}

const clamp = (value, low, high) => Math.max(low, Math.min(high, Math.round(value)));

export function createMusicPage({send, onChange, onError} = {}) {
  let state = {
    ...MUSIC_PAGE_DEFAULTS,
    open: false, stopped: false, width: MUSIC_PAGE_DEFAULTS.width, height: MUSIC_PAGE_DEFAULTS.height,
    pending: false, error: '',
  };
  let disposed = false;
  const emit = () => onChange?.({...state});

  function post(action, fields = {}) {
    if (disposed || !validMusicPageCommand(action, fields)) return false;
    if (state.pending) return false;
    state = {...state, pending: true, error: ''};
    emit();
    try {
      if (send?.(action, fields) === true) return true;
    } catch {}
    state = {...state, pending: false, error: 'unavailable'};
    emit();
    onError?.(state.error);
    return false;
  }

  return {
    get state() { return {...state}; },
    load: () => post('snapshot'),
    reset: () => post('reset'),
    setSize: (width, height) => post('setSize', {
      width: clamp(width, state.minWidth, state.maxWidth),
      height: clamp(height, state.minHeight, state.maxHeight),
    }),
    receive(message) {
      if (disposed || !validMusicPageMessage(message)) return false;
      state = {...state, ...message, pending: false, error: ''};
      emit();
      return true;
    },
    dispose() { disposed = true; },
  };
}
