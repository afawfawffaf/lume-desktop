export function createNoticeController(element, {
  timers = globalThis,
  reducedMotion = () => globalThis.matchMedia?.("(prefers-reduced-motion: reduce)")?.matches === true,
} = {}) {
  let timer = null, generation = 0, disposed = false, anchored = false;
  function dismiss() {
    ++generation;
    if (timer !== null) timers.clearTimeout(timer);
    timer = null;
    element.hidden = true;
    element.classList.remove("is-hiding");
    element.classList.remove("is-top");
    anchored = false;
    element.textContent = "";
  }
  return {
    show(text, { autoDismiss = false, top = anchored } = {}) {
      if (disposed) return;
      dismiss();
      if (!text) return;
      element.textContent = text;
      element.hidden = false;
      // Long-running work pins the notice to the top so it never covers the
      // controls the user still needs; every other notice keeps the base spot.
      if (top) element.classList.add("is-top");
      anchored = top === true;
      if (!autoDismiss) return;
      const current = generation;
      timer = timers.setTimeout(() => {
        if (disposed || current !== generation) return;
        timer = null;
        if (reducedMotion()) { dismiss(); return; }
        element.classList.add("is-hiding");
        timer = timers.setTimeout(() => {
          if (!disposed && current === generation) dismiss();
        }, 180);
      }, 2500);
    },
    dismiss,
    dispose() { if (!disposed) { dismiss(); disposed = true; } },
  };
}
