# Parties UI architecture

## Stylesheet architecture

RCSS is split by responsibility. `theme.rcss` owns the single token namespace,
`typography.rcss` owns the semantic type scale, `primitives.rcss` owns native
RmlUI form pseudo-elements, and `components.rcss` owns reusable `ui-*`
components. `shell.rcss` and the feature files (`launcher`, `room`, `chat`,
`settings`, `streaming`, `dialogs`) own composition only. `desktop.rcss` and
`mobile.rcss` are the final platform-specific density and interaction layers.

Documents load the cascade in a fixed direction: theme, typography, native
primitives, reusable components, shell and feature composition, then the final
desktop or mobile platform layer. A lower-level module must never depend on a
selector declared by a higher-level module.

Tokens use RmlUI's native CSS-compatible custom properties and `var()` support.
They cascade and inherit through the element tree, so a screen or component can
override a token locally. Undefined names, cycles, and malformed expressions
are reported through RmlUI's logging system; fallbacks use `var(--name, value)`.

## Windows renderer

Windows is pinned to upstream `mikke89/RmlUi` revision
`0ae381e00d7426762bb5ed897973366358b16642` and builds RmlUI's bundled
`Win32_DX12` backend. RmlUI owns the device, swap chain, render targets,
command recording, descriptors, clipping, layers, filters, shaders, and
presentation. There is no runtime backend selector and no Parties-owned D3D11,
Vulkan, or general-purpose D3D12 RmlUI renderer.

`PartiesRenderInterface_DX12` is a narrow compatibility adapter. It delegates
the complete RmlUI render interface to the upstream backend and adds only the
application-specific video texture contract and deterministic designer
screenshot hook. New general rendering behavior belongs upstream instead of in
the adapter.

Video conversion is isolated in `Dx12VideoConverter`, not mixed into RmlUI's
pipelines. CPU I420/NV12 frames use persistent per-back-buffer upload resources
and a compute pass into one RGBA texture. AMF supplies native NV12 plane views,
its producer fence, and its current resource state to the same compute pass.
NVDEC supplies an already converted CUDA/D3D12 shared RGBA texture. Every RGBA
result enters RmlUI through the small upstream external-texture hook and is
retired only after the matching swap-chain fence completes. See
`docs/video-pipeline.md` for the ownership and fallback contracts.

```rcss
body {
  --color-surface: #10141c;
  --space-card: 16dp;
}

.card {
  padding: var(--space-card);
  background-color: var(--color-surface);
}
```

## Layers

1. **Application shell** — title bar, navigation rail, content viewport, modal
   host, and responsive platform behavior.
2. **Screens** — launcher, voice room, chat, streams, settings, and onboarding.
   Screens compose primitives and bind to view models; they do not own network,
   audio, persistence, or platform logic.
3. **Primitives** — button, icon button, card, field, toggle, slider, avatar,
   badge, empty state, section header, dialog, and menu.
4. **View models** — observable presentation state and user intents. Domain and
   platform callbacks are injected from the application layer.
5. **Services** — settings, identity, audio, capture, networking, and updates.
   These remain independent of RmlUI.

## State ownership rules

- Persistent preferences belong to a typed settings service, not a screen model.
- Navigation and overlay state belong to a dedicated UI session model.
- Domain models expose domain facts; derived labels and visibility state belong
  to view models.
- RML sends semantic intents (`leave_channel`, `toggle_mute`), never platform
  commands or database operations.
- A model mutation must dirty its own binding. Direct raw binding is reserved for
  tooling and dynamic previews.
- Only one top-level overlay is active. Dialogs are pushed through a modal host
  instead of adding another `show_*` boolean to the lobby model.

## Document routing

`DocumentRouter` is the single owner of the active top-level RML route. Room,
chat, settings, share picker, and streams are enum values in one state machine,
not independent visibility flags. RML renders those pages from the bound
`route` string, which makes mutually exclusive pages impossible by construction.

Primary navigation uses `go()` and clears transient history. Settings and the
share picker use `push()` so their close action returns to the exact invoking
route. Stream termination rewrites stale stream entries in the history before
navigation, preventing Back from resurrecting an ended session. Settings
sections are child state owned by the router and do not become top-level routes.

## Transient context windows

Desktop context surfaces are real frameless `WS_POPUP` HWNDs, not overlays in
the main document and not blocking Win32 menus. `ContextWindowManager` owns one
active transient window, its renderer, RmlUI context, IME handler, document, and
data model. Opening another context surface replaces the current one; Escape,
the close action, and focus loss request asynchronous teardown without a nested
message loop.

User and channel menus use the same platform-neutral `ContextWindowModel` and
`context_window.rml`. The user variant composes personal volume and voice
normalization controls with permission-aware actions. Channel, server, and
message variants supply semantic action lists. The shared model is also loaded
by RmlUI Designer, so screenshots exercise the production document rather than
a separate mock.

## Picture-in-picture

A watched stream can be popped out into picture-in-picture (PiP). The state is
platform-neutral: `StreamPipController` (`client/include/client/stream_pip.h`),
owned by `AppCore`, is either closed or open on exactly one watched stream, and
mirrors it into `LobbyModel::pip_stream_id`. The RML intent is
`toggle_stream_pip(id)` from the button in each stream cell's footer; picking it
on another stream switches PiP to that stream. `AppCore` reconciles the
controller against the authoritative watched set after every watch mutation
(`rebuild_watched_model`, `clear_all_sharers`), so a stream that ends, stop
watching, leaving the channel and disconnecting all close PiP through one path.
Opening PiP also makes its stream the primary one, so its audio is played, and
leaves fullscreen.

While a stream is in PiP its grid cell shows "Playing in picture-in-picture"
instead of the `<video_frame>`; frame routing is described in
`docs/video-pipeline.md`. Platforms implement only `PlatformBridge::show_pip`,
`hide_pip` and `show_main_window`; leaving `show_pip` unset hides the button
(`pip_supported`).

The desktop overlay is shared: `ui/pip.rml`, `ui/pip.rcss` (layout and colour
only) and `PipWindowModel` provide two `ui-symbol` actions, return to the main
window (closes PiP and shows the stream on the Streams route) and close PiP
(keeps watching in the grid), plus a stream volume control along the bottom
edge: a `ui-range` slider with the same 0-200% range as the Streams header
slider and a percentage label. Both sliders call `AppCore::set_stream_volume`,
which drives the stream player and the `audio.stream_volume` preference; the
PiP model is refreshed from `LobbyModel::stream_volume` every frame, and both
change handlers ignore the `change` event a programmatic update dispatches, so
the controls stay in sync without echoing. The volume never touches the
microphone. Clicking the slider gives the PiP window keyboard focus (arrows,
Page Up/Down, Home/End). The overlay dims the video and fades in within 120 ms
while the cursor is over the window, the slider is being dragged, or the window
has keyboard focus. Remembered geometry uses the `window.pip_rect`
preference and `pip_geometry`, which keeps the window fully on a visible work
area and at the stream's aspect ratio.

- **Windows.** `Win32PipWindow` (`client/src/windows/win32_pip_window.*`) is an
  unowned `WS_POPUP` tool window with `WS_EX_TOPMOST | WS_EX_NOACTIVATE`; an
  owned window would be hidden with a minimized owner. `WM_NCHITTEST` returns
  `HTCAPTION` everywhere except overlay actions and the resize edges, and
  `WM_SIZING` keeps the aspect ratio. A press on a `pip-keyboard` control (the
  volume slider) calls `SetForegroundWindow`, the only way the window is
  activated, and keys are then forwarded to RmlUi. `PipWindowHost` adds its own DX12
  renderer, RmlUi context and document; like context windows it is created on
  first use and kept until `Rml::Shutdown`. The render thread renders it after
  the main window and keeps rendering it alone while the main window is
  minimized or hidden to the tray.
- **macOS.** `PartiesPipPanelController` owns a borderless, non-activating
  floating `NSPanel` that joins all Spaces and fullscreen Spaces, ignores
  application hiding (`canHide = NO`) and the menu-bar mode, and enforces
  `contentAspectRatio`. `PartiesPipHost` renders `ui/pip.rml` into the panel
  with its own `MTKView`, `RenderInterface_Metal` and RmlUi context; clicks
  outside the actions drag the panel (`performWindowDragWithEvent:`). The
  panel can become key, but only the volume slider makes it key (the
  application stays inactive), for its arrow keys.
- **iOS.** There is no custom window: `PartiesStreamPictureInPicture` uses
  `AVPictureInPictureController` with an `AVSampleBufferDisplayLayer` content
  source. The same layer shows the watched stream inline, kept over its
  `<video_frame>` cell every frame (the RmlUi element still receives the taps),
  so PiP starts automatically when the app goes to the background
  (`canStartPictureInPictureAutomaticallyFromInline`) and from the cell's
  button. The system restore button returns to the Streams route on that
  stream. While a stream is watched the audio session drops
  `MixWithOthers`, which automatic PiP requires; `UIBackgroundModes` already
  contains `audio`.

## Application audio sharing

Application audio is selected through the regular share-picker route in an
audio-only mode. On Windows, a temporary capture instance enumerates application
windows and creates real thumbnails without touching a concurrently active
screen capture. The selected HWND resolves to a process ID and WASAPI process
loopback captures its process tree at 48 kHz stereo.

`downmix_application_audio` converts each 20 ms block to mono and feeds
`AudioEngine::push_secondary_pcm`. The existing music-profile Opus encoder and
`VOICE2` transport send it independently from microphone voice, so recipients
can mix karaoke/music and speech with separate levels. Leaving the channel,
disconnecting, shutting down, or pressing the active music control stops the
capture before the audio engine is torn down.

## RCSS source layout

```text
client/ui/
  theme.rcss
  primitives.rcss
  components.rcss
  typography.rcss
  shell.rcss
  launcher.rcss
  room.rcss
  chat.rcss
  settings.rcss
  streaming.rcss
  pip.rcss          (desktop picture-in-picture document, ui/pip.rml)
  dialogs.rcss
  desktop.rcss
  mobile.rcss
client/include/client/ui/
  ui_session_model.h
  navigation.h
  modal.h
  preferences.h
```

RmlUI does not provide a component runtime comparable to a browser framework,
so repeated behavior should be implemented as custom elements or small C++ view
models. Repeated styling stays in semantic primitive classes. Screen documents
should remain declarative and contain no duplicated service logic.

## Module ownership rule

1. Color, radius, spacing, or density token: `theme.rcss`.
2. Font scale or semantic text class: `typography.rcss`.
3. Reusable visual control: `components.rcss`.
4. Native RmlUI input pseudo-element: `primitives.rcss`.
5. Screen composition: the matching feature file.
6. Windows/macOS-only override: `desktop.rcss`.
7. Touch, safe-area, orientation, or iOS-only override: `mobile.rcss`.

## Definition of done for a migrated screen

- Uses semantic primitives and the shared spacing/color conventions.
- Has one view model with explicit state ownership.
- Does not call services directly from RML.
- Handles empty, loading, error, populated, and narrow layouts.
- Is exercised by model tests and a screenshot fixture.
- Has no duplicated component rule in its feature stylesheet.

## Visual verification

The designer exposes real-model fixtures for `onboarding`, `recovery`,
`launcher`, `party-modal`, `room`, `streams`, `spotlight`, `member`, `settings`,
`share`, `audio-share`, `context-user`, `context-channel`, and `chat`. Run
`tools/capture-ui.ps1` after building the designer to
render every fixture at an exact viewport. A non-zero exit means the document,
bindings, RmlUI parser, renderer, or screenshot path failed.

The offline `parties_ui_stylesheet_variables` test covers token overrides,
nested references, and nested fallbacks. Audio regression executables are also
registered with CTest so the test suite is discoverable instead of requiring
manual invocation.
