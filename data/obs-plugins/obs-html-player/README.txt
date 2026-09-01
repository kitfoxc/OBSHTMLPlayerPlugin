OBS HTML Player v0.2

Place an HTML source in OBS and load index.html. The page can use the OBS Player SDK:

<script src="../../data/obs-plugins/obs-html-player/obs-player.js"></script>

API:
  obsPlayer.state
  obsPlayer.title
  obsPlayer.artist
  obsPlayer.album
  obsPlayer.source
  obsPlayer.duration
  obsPlayer.position
  obsPlayer.playing
  obsPlayer.paused
  obsPlayer.albumArt
  obsPlayer.on(name, callback)
  obsPlayer.once(name, callback)
  obsPlayer.refresh()

Events:
  ready is reserved for a future embedded-injection bridge.
  state, trackchange, play, pause, stop, albumart, error
