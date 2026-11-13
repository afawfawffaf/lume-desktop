import {MUSIC_VISUALIZERS} from './music-visualizer.js';

// The card has no close button and no visible chrome: everything it can do lives
// in one right-click menu. The menu only reports the user's choice; the owner
// decides how to persist it.
//
// Every section expands inline rather than flying out beside its row: the panel
// is a single scroll container, so a long list scrolls instead of being clipped
// by the card edge or by the menu's own overflow.
const STYLES = MUSIC_VISUALIZERS;
const EFFECTS = [['none', '关闭'], ['rain-window', '雨窗'], ['snow', '雪天'], ['star-river', '星河']];
const FONTS = [
  ['', '默认'],
  ['KaiTi', '楷体'],
  ['SimSun', '宋体'],
  ['Microsoft YaHei', '雅黑'],
];
const INTENSITY = [['calm', '平静'], ['normal', '标准'], ['chaotic', '强烈']];
// The lyric colours the visualizers actually read. Both are plain hex, so a
// native colour input can drive them without a picker surface of our own.
const COLORS = [
  ['lyricsFontColor', '歌词颜色'],
  ['lyricsKaraokeFillColor', '填充颜色'],
  ['sonnetColors.lyricsFontColor', '字样颜色'],
  ['sonnetColors.karaokeFillColor', '字样填充'],
  ['sonnetColors.glowColor', '光晕颜色'],
];

const escape = value => String(value).replace(/[&<>"']/g, ch => (
  {'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[ch]
));

const read = (value, path) => path.split('.').reduce((current, key) => current?.[key], value);
const HEX = /^#[0-9a-f]{6}$/i;

// One collapsible section: a header button and the list it reveals.
function section(id, label, options, value) {
  const current = options.find(([key]) => key === value)?.[1] || options[0][1];
  return `<button class="MusicCardMenuRow" type="button" data-music-card-menu-toggle="${id}"
      aria-expanded="false" aria-controls="music-card-menu-${id}">
      <span class="MusicCardMenuLabel">${escape(label)}</span>
      <span class="MusicCardMenuValue">${escape(current)}</span>
    </button>
    <div class="MusicCardMenuList" id="music-card-menu-${id}" role="menu" hidden>
      ${options.map(([key, text]) => `<button type="button" role="menuitemradio"
        aria-checked="${key === value}" data-music-card-menu-pick="${id}" data-music-card-menu-value="${escape(key)}"
        >${escape(text)}</button>`).join('')}
    </div>`;
}

function colors(preferences) {
  return COLORS.map(([path, label]) => {
    const value = read(preferences, path);
    return `<label class="MusicCardMenuColor">
      <span>${escape(label)}</span>
      <input type="color" value="${HEX.test(value || '') ? value : '#ffffff'}"
        data-music-card-menu-color="${path}" aria-label="${escape(label)}">
    </label>`;
  }).join('');
}

export function mountMusicCardMenu(host, {onPick, onClose, onCommand, onBackground} = {}) {
  const document = host.ownerDocument;
  const window = document.defaultView;
  let menu = null, panel = null;
  let disposed = false;
  let open = '';
  let preferences = null;
  let background = null;

  // Inner markup only. The panel element itself is created once by show() and
  // re-filled in place: assigning the whole panel markup into panel.innerHTML
  // would nest a second fixed-position panel and collapse the outer one to a
  // zero-height sliver after every change.
  const content = () => {
    const value = preferences || {};
    return `
      ${section('mode', '歌词样式', STYLES.map(item => [item.id, item.label]), value.mode)}
      ${section('fontFamily', '字体设置', FONTS, value.fontFamily || '')}
      <button class="MusicCardMenuRow" type="button" data-music-card-menu-toggle="color"
          aria-expanded="false" aria-controls="music-card-menu-color">
        <span class="MusicCardMenuLabel">歌词颜色</span>
        <span class="MusicCardMenuValue">调整</span>
      </button>
      <div class="MusicCardMenuList MusicCardMenuColors" id="music-card-menu-color" hidden>
        ${colors(value)}
      </div>
      ${section('effectMode', '特效', EFFECTS, value.effectMode || 'none')}
      ${section('animationIntensity', '动画强度', INTENSITY, value.animationIntensity || 'normal')}
      <button class="MusicCardMenuRow" type="button" data-music-card-menu-toggle="background"
          aria-expanded="false" aria-controls="music-card-menu-background">
        <span class="MusicCardMenuLabel">背景设置</span>
        <span class="MusicCardMenuValue">${escape(background?.name || '无')}</span>
      </button>
      <div class="MusicCardMenuList MusicCardMenuBackground" id="music-card-menu-background" hidden>
        <button type="button" role="menuitem" data-music-card-menu-background="choose">自选背景</button>
        <button type="button" role="menuitem" data-music-card-menu-background="clear"
          ${background?.id ? '' : 'disabled'}>清除背景</button>
      </div>
      <div class="MusicCardMenuDivider" role="separator"></div>
      <button class="MusicCardMenuAction" type="button" role="menuitem" data-music-card-menu-action="settings">打开音乐设置</button>
      <button class="MusicCardMenuAction" type="button" role="menuitem" data-music-card-menu-action="close">关闭卡片</button>`;
  };

  // Replaces the panel's contents and reopens the section that was open, so a
  // change never closes the menu the user is working in.
  const refresh = () => {
    if (!panel) return;
    const previous = open;
    const scroll = panel.scrollTop;
    panel.innerHTML = content();
    open = '';
    if (previous) {
      const header = panel.querySelector(`[data-music-card-menu-toggle="${previous}"]`);
      const list = panel.querySelector(`#music-card-menu-${previous}`);
      if (header && list) {
        list.hidden = false;
        header.setAttribute('aria-expanded', 'true');
        open = previous;
      }
    }
    panel.scrollTop = scroll;
  };

  const hide = () => {
    if (!menu) return;
    menu.remove();
    menu = null;
    panel = null;
    open = '';
    window.removeEventListener('pointerdown', onOutside, true);
    window.removeEventListener('contextmenu', onOutside, true);
    window.removeEventListener('keydown', onKey, true);
    window.removeEventListener('resize', hide);
  };

  function onOutside(event) {
    if (!menu) return;
    if (menu.contains(event.target)) return;
    hide();
  }

  function onKey(event) {
    if (event.key === 'Escape') {
      event.stopPropagation();
      hide();
      onClose?.();
    }
  }

  const toggle = id => {
    const header = panel?.querySelector(`[data-music-card-menu-toggle="${id}"]`);
    const list = panel?.querySelector(`#music-card-menu-${id}`);
    if (!header || !list || !panel) return;
    const wasOpen = !list.hidden;
    // One section at a time keeps the panel short on a small card.
    for (const other of panel.querySelectorAll('.MusicCardMenuList')) other.hidden = true;
    for (const other of panel.querySelectorAll('[data-music-card-menu-toggle]'))
      other.setAttribute('aria-expanded', 'false');
    if (!wasOpen) {
      list.hidden = false;
      header.setAttribute('aria-expanded', 'true');
      open = id;
      // Scroll the panel, not the document: scrollIntoView would also move the
      // card's own scroll position and could shove the whole surface around.
      const top = list.offsetTop;
      const want = top + list.offsetHeight;
      const capacity = panel.scrollTop + panel.clientHeight;
      if (want > capacity) panel.scrollTop = Math.min(top, want - panel.clientHeight);
      else if (top < panel.scrollTop) panel.scrollTop = top;
    } else {
      open = '';
    }
  };

  // Position only; size comes from CSS. A measured height was fragile here --
  // the card rect can be zero on the frame the menu is attached, and a
  // too-small clamp collapsed the whole panel into a sliver with a scrollbar.
  function show(event) {
    const card = host.getBoundingClientRect();
    const left0 = card.left || 0, top0 = card.top || 0;
    const width0 = card.width || window.innerWidth;
    const height0 = card.height || window.innerHeight;
    hide();
    menu = document.createElement('div');
    menu.className = 'MusicCardMenuHost';
    host.append(menu);
    panel = document.createElement('div');
    panel.className = 'MusicCardMenu';
    panel.setAttribute('role', 'dialog');
    panel.setAttribute('aria-label', '音乐卡片设置');
    panel.innerHTML = content();
    menu.append(panel);
    const gap = 8;
    // Horizontal: prefer the pointer, then pull inside the card.
    const left = Math.max(left0 + gap, Math.min(event.clientX, left0 + width0 - panel.offsetWidth - gap));
    panel.style.left = `${Math.round(left)}px`;
    // Vertical: open below the pointer; flip above only when the panel cannot
    // fit below and there is more room above.
    const natural = panel.offsetHeight;
    const below = top0 + height0 - event.clientY;
    const above = event.clientY - top0;
    const useAbove = natural + gap > below && above > below;
    const top = useAbove
      ? Math.max(top0 + gap, event.clientY - natural - gap)
      : Math.min(event.clientY + gap, top0 + height0 - natural - gap);
    panel.style.top = `${Math.round(top)}px`;
    window.addEventListener('pointerdown', onOutside, true);
    window.addEventListener('contextmenu', onOutside, true);
    window.addEventListener('keydown', onKey, true);
    window.addEventListener('resize', hide);
  }

  const pick = (id, value) => {
    if (!preferences) return;
    preferences = {...preferences, [id]: value};
    refresh();
    onPick?.({[id]: value});
  };

  const pickColor = (path, value) => {
    if (!preferences || !HEX.test(value)) return;
    const patch = path.startsWith('sonnetColors.')
      ? {sonnetColors: {...preferences.sonnetColors, [path.slice('sonnetColors.'.length)]: value}}
      : {[path]: value};
    preferences = {...preferences, ...patch};
    onPick?.(patch);
  };

  host.addEventListener('contextmenu', event => {
    if (disposed) return;
    event.preventDefault();
    show(event);
  });

  // One delegated listener: the panel is rebuilt on every change, so per-button
  // registrations would leak with each render.
  host.addEventListener('click', event => {
    const target = event.target;
    const toggleButton = target?.closest?.('[data-music-card-menu-toggle]');
    if (toggleButton) {
      event.preventDefault();
      toggle(toggleButton.dataset.musicCardMenuToggle);
      return;
    }
    const picker = target?.closest?.('[data-music-card-menu-pick]');
    if (picker) {
      event.preventDefault();
      pick(picker.dataset.musicCardMenuPick, picker.dataset.musicCardMenuValue);
      return;
    }
    const background = target?.closest?.('[data-music-card-menu-background]');
    if (background) {
      event.preventDefault();
      if (background.disabled) return;
      onBackground?.(background.dataset.musicCardMenuBackground);
      return;
    }
    const action = target?.closest?.('[data-music-card-menu-action]');
    if (action) {
      event.preventDefault();
      const name = action.dataset.musicCardMenuAction;
      if (name === 'close') hide();
      onCommand?.(name);
    }
  });

  // A colour input fires continuously while the picker is open; preview each
  // value so the lyric stage follows the drag, and let the owner debounce saves.
  host.addEventListener('input', event => {
    const input = event.target?.closest?.('[data-music-card-menu-color]');
    if (!input) return;
    pickColor(input.dataset.musicCardMenuColor, input.value);
  });

  return {
    // The card's current preferences and background, so the menu opens on the
    // values actually in effect.
    setPreferences(next) {
      preferences = next || null;
      refresh();
    },
    setBackground(next) {
      background = next || null;
      if (!panel) return;
      const header = panel.querySelector('[data-music-card-menu-toggle="background"] .MusicCardMenuValue');
      if (header) header.textContent = background?.name || '无';
      const clear = panel.querySelector('[data-music-card-menu-background="clear"]');
      if (clear) clear.disabled = !background?.id;
    },
    get open() { return !!menu; },
    hide,
    dispose() {
      if (disposed) return;
      disposed = true;
      hide();
    },
  };
}
