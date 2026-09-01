(function () {
  'use strict';
  const endpoint = 'http://127.0.0.1:38765/state';
  const listeners = new Map();
  let state = Object.freeze({hasTrack:false,playing:false,title:'',artist:'',album:'',source:'',duration:0,position:0,timestamp:0,albumArt:''});
  let timer = 0;
  let stopped = false;
  function emit(name, value) { const set=listeners.get(name); if(!set)return; for(const fn of [...set]){try{fn(value)}catch(e){console.error('[obsPlayer]',e)}} }
  async function refresh(){
    try {
      const r=await fetch(endpoint+'?t='+Date.now(),{cache:'no-store'});
      if(!r.ok) throw new Error('HTTP '+r.status);
      const next=await r.json();
      const prev=state;
      state=Object.freeze(next);
      if(prev.title!==next.title||prev.artist!==next.artist||prev.album!==next.album) emit('trackchange',next);
      if(prev.playing!==next.playing) emit(next.playing?'play':'pause',next);
      if(prev.hasTrack!==next.hasTrack) emit(next.hasTrack?'trackchange':'stop',next);
      if(prev.albumArt!==next.albumArt) emit('albumart',next.albumArt);
      emit('state',next);
    }catch(e){ emit('error',e); }
    if(!stopped) timer=setTimeout(refresh,250);
    return state;
  }
  const api={
    get state(){return state;},
    get title(){return state.title;}, get artist(){return state.artist;}, get album(){return state.album;},
    get source(){return state.source;}, get duration(){return state.duration;}, get position(){return state.position;},
    get playing(){return state.playing;}, get paused(){return !state.playing&&state.hasTrack;}, get albumArt(){return state.albumArt;},
    on(name,fn){if(typeof fn!=='function')throw new TypeError('listener must be a function');if(!listeners.has(name))listeners.set(name,new Set());listeners.get(name).add(fn);return()=>listeners.get(name)?.delete(fn);},
    once(name,fn){let off;off=this.on(name,v=>{off();fn(v);});return off;},
    refresh,
    stop(){stopped=true;clearTimeout(timer);},
    endpoint,
    version:'0.2.0'
  };
  window.obsPlayer=api;
  window.OBSPlayer=api;
  refresh();
})();
