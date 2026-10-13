# Vertical+ for OBS Studio

A second, vertical (9:16) canvas that streams and records alongside your normal 16:9 output, built on the native canvas API in OBS Studio 32. It does what Aitum Vertical does and fixes its four most-reported problems:

| Aitum Vertical | Vertical+ |
| --- | --- |
| Vertical-only audio sources don't reach the vertical stream | **Isolated vertical audio bus**: vertical hears exactly what is visible on vertical, on its own reserved track; nothing vertical-only leaks into the main stream |
| Toggling a source on one canvas leaves the other out of sync | **Linked visibility** across canvases, from clicks, hotkeys or automation |
| Two full scene renders every frame | **Main Canvas Reframe** source (crops the finished main frame), **idle parking**, encoder **fps divisor** |
| Stream Deck / Streamer.bot / websocket can't see vertical scenes | **obs-websocket vendor `vertical-plus`** + hotkeys for every vertical scene and output |

Requires **OBS Studio 32.0+** on Windows, macOS or Linux (X11 preview; Wayland preview is a TODO).

## How the audio isolation works

The vertical canvas is created with `ACTIVATE | MIX_AUDIO | SCENE_REF`, but channel 0 holds a private **program source**, not the scene. That source:

- renders the vertical transition (and so the current vertical scene) as its video,
- is the only root of the vertical canvas in the global audio mix, and writes **only the reserved track** (default 6),
- sums every visible audio source on the vertical canvas directly from that source's own buffer.

Vertical scenes are therefore never mixed into tracks 1-5. A source shared with the main canvas (your mic) is root-mixed once by libobs for the main tracks and read once by the bus for track 6, so it is never doubled. Every other source is kept off the reserved track automatically.

This was verified headlessly against libobs built from obs-studio master (Oct 2026), see `tests/test-audio-bus.cpp`:

```
1. mic shared, game main-only, music vertical-only   track 1 = 0.500  track 6 = 0.300  PASS
2. music hidden on vertical                          track 1 = 0.500  track 6 = 0.100  PASS
3. mic hidden on vertical only                       track 1 = 0.500  track 6 = 0.200  PASS
4. mic fader at 50% (applies to both)                track 1 = 0.450  track 6 = 0.250  PASS
5. music muted                                       track 1 = 0.500  track 6 = 0.100  PASS
6. vertical parked                                   track 1 = 0.500  track 6 = 0.000  PASS
```

The same test with the naive setup (scene directly on an audio-mixing canvas) leaks the vertical-only music into the main stream (track 1 = 0.700) and leaves track 6 silent, which is the behaviour Aitum users report.

## Using it

1. Install, start OBS, open **Docks > Vertical+**.
2. Add vertical scenes with **+**; add sources from the **+** menu (existing inputs, main scenes, or a **Main Canvas Reframe**).
3. Arrange sources right in the dock preview, like OBS's main preview:
   - **Click** a source to select it (red outline with handles). **Drag** to move; it snaps to the canvas edges and centre (hold **Ctrl** to turn snapping off).
   - **Drag a handle** to resize. Corners keep the shape; hold **Shift** to stretch freely. Edge handles stretch one side.
   - **Arrow keys** nudge 1 px (**Shift** = 10 px). **Esc** deselects.
   - **Double-click** a source, or right-click > **Edit Transform...**, for exact position, size, rotation and crop.
   - Right-click also has **Center on canvas**, **Reset Transform**, quick layouts (Fit / Fill / Top half / Bottom half), the link toggle, properties and filters.
4. **Tools > Vertical+ Settings** (or **Settings** in the dock): pick your **Platform** and paste your **stream key**.
   YouTube, Twitch and Facebook fill in the server for you. TikTok, Kick, Instagram and Custom also need the
   **Server URL** the platform gives you next to the key (TikTok usually issues a new key every time you go live).
   The same window has canvas size, reserved audio track, encoder, recording folder and start-with-main options.
   If the vertical stream can't start or drops, the dock tells you why and offers to open Settings.
5. Make sure your main outputs do not use the reserved track; the dock warns if they do.

### Automation

`CallVendorRequest` with `vendorName: "vertical-plus"`:

| requestType | requestData | response |
| --- | --- | --- |
| `GetStatus` | - | width, height, currentScene, streaming, recording, parked, audioTrack |
| `GetSceneList` | - | scenes[] (sceneName, sceneUuid), currentScene |
| `SetCurrentScene` | sceneName | success |
| `SetItemVisible` | sceneName, sourceName, visible, link (default true), canvas ("vertical" or "main") | success |
| `StartStream` / `StopStream` | - | success |
| `StartRecord` / `StopRecord` | - | success, outputPath on stop |
| `SetReframePreset` | sourceName, preset (center, left, right, full, custom), durationMs | success |

Vendor events: `SceneChanged`, `StreamStateChanged`, `RecordStateChanged`.

Hotkeys (Settings > Hotkeys): switch to each vertical scene, start/stop vertical stream, start/stop vertical recording; each Reframe source adds next/previous framing.

## Installing (Windows)

1. Open this repo's [**Releases**](https://github.com/manreyes420/vertical-plus/releases/latest) page and download
   `vertical-plus-<version>-windows-x64-Installer.exe`.
2. Close OBS, then run the installer. It is not code-signed yet, so Windows may say
   *Windows protected your PC*: click **More info → Run anyway**.
3. Start OBS Studio 32 or newer. **Docks → Vertical+** and **Tools → Vertical+ Settings** should appear.
   If not, check **Help → Log Files → View Current Log** for `vertical-plus`.

The installer puts the plugin in `C:\ProgramData\obs-studio\plugins\vertical-plus\` (this folder survives
OBS updates), warns if OBS is missing or older than 32, waits for OBS to be closed, and adds
**Vertical+ for OBS** to Windows' *Installed apps* so it can be uninstalled. Your Vertical+ settings
(in your OBS profile) are kept when uninstalling.

Manual install: download the `windows-x64.zip` from the same page and unzip it into
`C:\ProgramData\obs-studio\plugins\`.

## Building

GitHub Actions builds Windows and Ubuntu packages on every push to `main`
(this repo is based on the official [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)).
Every push to `main` also publishes (or refreshes) the GitHub Release for the version in
`buildspec.json`, with the Windows installer, zip and Linux packages attached. Bump `version`
in `buildspec.json` to start a new release. The installer is built with Inno Setup from
`installer/vertical-plus.iss`.

It builds against OBS Studio 32.0.0 headers, so it loads in any OBS 32.x.
The obs-deps and Qt downloads are checked against OBS's own published hashes, and the
Windows OBS source archive hash is pinned. The macOS build is switched off for now: the
template cannot yet configure OBS 32's Swift-based Metal renderer.

Local build: `cmake --preset windows-x64` then `cmake --build --preset windows-x64`
(see the template's documentation for prerequisites).

## Layout

| File | Job |
| --- | --- |
| `src/plugin-main.cpp` | Module load, frontend events, Tools menu |
| `src/canvas-manager.cpp` | Find or create the canvas, scenes, idle parking, hotkeys |
| `src/audio-bus.cpp` | Program source / isolated audio bus, reserved-track handling |
| `src/reframe-source.cpp` | Main Canvas Reframe source with eased framing presets |
| `src/visibility-link.cpp` | Linked visibility across canvases |
| `src/outputs.cpp` | Vertical stream + recording, encoders |
| `src/websocket-api.cpp` | obs-websocket vendor requests and events |
| `src/settings.cpp` | Per-profile `vertical-plus.json`, settings dialog |
| `src/dock.cpp` | Qt dock: preview, scene and source lists, output buttons |
| `src/transform-math.hpp`, `src/item-edit.hpp` | Preview move/resize geometry and scene item helpers |
| `tests/test-audio-bus.cpp` | Headless libobs test of the audio isolation |
| `tests/test-transform-math.cpp`, `tests/test-preview-edit.cpp` | Unit and libobs tests for preview move/resize |
| `src/platforms.hpp` | Streaming platforms and their ingest servers |
| `tests/test-stream.cpp` | libobs test: streams the vertical canvas to a local RTMP server, plus error cases |

## Status and known gaps

What has been checked: every source file compiles cleanly with GCC 13 and Clang against current libobs, frontend API and Qt 6 headers; the linked plugin's only unresolved symbols are frontend-API functions; the audio bus passes the headless test above. It has **not yet been run inside the OBS app**, so the dock, outputs and settings dialog need a first real-world pass.

Known gaps (milestone 4 in the design spec):

- Rotated sources can be moved in the preview but are resized through Edit Transform.
- Audio crossfade during vertical fades (audio cuts at the start of the transition).
- Per-canvas volume for shared sources (the fader applies to both canvases).
- Wayland preview; HDR colour handling in the Reframe source.
- Changing the reserved track while vertical outputs are live applies on the next start.

## License

GPL-2.0-or-later. `src/external/obs-websocket-api.h` is from obs-websocket (GPL-2.0-or-later).
