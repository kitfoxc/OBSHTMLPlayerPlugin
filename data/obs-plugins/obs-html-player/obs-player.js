/* OBS HTML Player SDK v0.2 */
(function () {
  'use strict';
  const origin = window.location.origin;
  const path = window.location.pathname;
  const match = path.match(/^\/p\/([^/]+)\//);
  const playerId = match ? match[1] : '';
  const endpoint = origin + '/state';
  const versionEndpoint = playerId ? origin + '/p/' + encodeURIComponent(playerId) + '/__version' : '';
  const listeners = new Map();
  let state = {
    hasTrack: false, playing: false, paused: false, stopped: true,
    title: '', artist: '', album: '', source: '', duration: 0, position: 0,
    timestamp: 0, albumArt: '', version: 0
  };
  let lastVersion = null;

  function emit(name, value) {
    const set = listeners.get(name);
    if (!set) return;
    for (const fn of set) { try { fn(value); } catch (_) {} }
  }
  function normalize(s) {
    s = s || {};
    return Object.assign({
      hasTrack:false, playing:false, paused:false, stopped:true,
      title:'', artist:'', album:'', source:'', duration:0, position:0,
      timestamp:0, albumArt:'', version:0
    }, s);
  }
  function update(next) {
    const old = state;
    state = normalize(next);
    if (old.title !== state.title || old.artist !== state.artist || old.album !== state.album || old.source !== state.source)
      emit('trackchange', state);
    if (old.playing !== state.playing) emit(state.playing ? 'play' : 'pause', state);
    if (old.hasTrack !== state.hasTrack) emit(state.hasTrack ? 'trackchange' : 'stop', state);
    if (old.albumArt !== state.albumArt) emit('albumart', state.albumArt);
    emit('state', state);
  }
  async function refresh() {
    try {
      const r = await fetch(endpoint + '?t=' + Date.now(), {cache:'no-store'});
      if (r.ok) update(await r.json());
    } catch (_) {}
  }
  async function checkVersion() {
    if (!versionEndpoint) return;
    try {
      const r = await fetch(versionEndpoint + '?t=' + Date.now(), {cache:'no-store'});
      if (!r.ok) return;
      const v = Number(await r.text());
      if (lastVersion !== null && v !== lastVersion) location.reload();
      lastVersion = v;
    } catch (_) {}
  }
  function loop() { refresh().finally(() => setTimeout(loop, 250)); }
  function versionLoop() { checkVersion().finally(() => setTimeout(versionLoop, 1000)); }

  window.obsPlayer = {
    get state() { return state; },
    get playerId() { return playerId; },
    get endpoint() { return endpoint; },
    on(name, fn) {
      if (!listeners.has(name)) listeners.set(name, new Set());
      listeners.get(name).add(fn);
      return () => listeners.get(name)?.delete(fn);
    },
    once(name, fn) {
      const off = this.on(name, value => { off(); fn(value); });
      return off;
    },
    refresh,
    getProgress() {
      if (!state.duration) return 0;
      return Math.max(0, Math.min(1, state.position / state.duration));
    },
    getPosition() {
      if (!state.playing || !state.timestamp) return state.position;
      return Math.max(0, Math.min(state.duration || Infinity,
        state.position + (performance.now() / 1000 + performance.timeOrigin / 1000 - state.timestamp)));
    }
  };

  window.OBSPlayer = window.obsPlayer;
  refresh();
  loop();
  checkVersion();
  versionLoop();
  emit('ready', state);
})();
