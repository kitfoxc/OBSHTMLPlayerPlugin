# OBS HTML Now Playing

A Windows OBS source based on the SMTC reader from [OBSSpotifyPlugin](https://github.com/lingeriegoat/OBSSpotifyPlugin), redesigned so the visual layer is a local HTML/CSS/JavaScript document rendered by OBS Browser Source.

## What it does

- Adds an OBS source named **HTML Now Playing**.
- Select any local `.html` / `.htm` file as the visual template.
- Uses OBS Browser Source / CEF for rendering, so CSS, SVG, Canvas, Web Animations, local images and fonts can be used.
- Reads Windows System Media Transport Controls (SMTC) for title, artist, album, source, playback state, position, duration and album artwork.
- Exposes the media state through a loopback-only HTTP API on `127.0.0.1:38765`.
- Serves `obs-player.js` from the same loopback API, so a template only needs one script tag.

## HTML template

Add this to your local HTML:

```html
<script src="http://127.0.0.1:38765/obs-player.js"></script>
```

Then use:

```js
obsPlayer.state.title
obsPlayer.state.artist
obsPlayer.state.album
obsPlayer.state.duration
obsPlayer.state.position
obsPlayer.state.playing
obsPlayer.state.source
obsPlayer.state.albumArt
```

Events:

```js
obsPlayer.on('state', state => {});
obsPlayer.on('trackchange', state => {});
obsPlayer.on('play', state => {});
obsPlayer.on('pause', state => {});
obsPlayer.on('stop', state => {});
obsPlayer.on('albumart', dataUrl => {});
```

The example template is in `examples/glass/index.html`.

## OBS setup

1. Build and install the plugin into the OBS installation directory.
2. Make sure OBS Browser Source is available/enabled; the plugin creates a private `browser_source` child internally.
3. Add **HTML Now Playing** as a source.
4. Select your local HTML file.
5. Set the viewport width, height and FPS.

OBS Browser Source already supports a local-file mode; this plugin uses that same mechanism internally rather than embedding another Chromium runtime. The OBS API also provides private source creation, video rendering and active-child management, which are used by this implementation.

## Current architecture

```text
Windows SMTC
    |
    v
HTML Now Playing plugin
    |
    +-- SMTC polling thread
    |
    +-- localhost JSON API :38765
    |
    +-- obsPlayer JavaScript SDK
    |
    v
OBS browser_source / CEF
    |
    v
local HTML + CSS + JS
```

The API intentionally binds to `127.0.0.1`, not `0.0.0.0`, so it is not exposed to the LAN.

## Build

The project keeps the OBS plugin-template CMake structure and targets Windows x64. The branch `html-player-v0.1` contains the first implementation.

The original `spotify-source.cpp/.h` files are retained in the fork as reference material, but they are no longer part of the build target.

## License

This project is GPL-2.0. It is derived from the GPL-2.0 licensed `OBSSpotifyPlugin` codebase and retains its attribution where applicable.
