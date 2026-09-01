// OBS HTML Now Playing SDK
// The native plugin exposes a read-only SMTC state endpoint on loopback.
(function () {
  const endpoint = 'http://127.0.0.1:38765/state';
  const listeners = new Map();
  let state = {
    hasTrack: false, playing: false, title: '', artist: '', album: '', source: '',
    duration: 0, position: 0, timestamp: 0, albumArt: ''
  };
  let previous = JSON.stringify(state);

  function emit(name, value) {
    (listeners.get(name) || []).forEach(fn => { try { fn(value); } catch (_) {} });
    (listeners.get('*') || []).forEach(fn => { try { fn(value, name); } catch (_) {} });
  }

  async function poll() {
    try {
      const r = await fetch(endpoint + '?t=' + Date.now(), { cache: 'no-store' });
      if (!r.ok) throw new Error(r.status);
      const next = await r.json();
      const old = state;
      state = next;
      if (!old.hasTrack && next.hasTrack) emit('trackchange', next);
      else if (old.title !== next.title || old.artist !== next.artist || old.album !== next.album) emit('trackchange', next);
      if (old.playing !== next.playing) emit(next.playing ? 'play' : 'pause', next);
      if (old.hasTrack !== next.hasTrack) emit(next.hasTrack ? 'trackchange' : 'stop', next);
      if (old.albumArt !== next.albumArt) emit('albumart', next.albumArt);
      emit('state', next);
      previous = JSON.stringify(next);
    } catch (_) {
      emit('error', _);
    }
    setTimeout(poll, 250);
  }

  window.obsPlayer = {
    get state() { return state; },
    on(name, callback) {
      if (!listeners.has(name)) listeners.set(name, new Set());
      listeners.get(name).add(callback);
      return () => listeners.get(name)?.delete(callback);
    },
    once(name, callback) {
      const off = this.on(name, value => { off(); callback(value); });
      return off;
    },
    refresh() { return fetch(endpoint + '?t=' + Date.now(), { cache: 'no-store' }).then(r => r.json()).then(v => { state = v; return v; }); },
    endpoint
  };

  poll();
})();
