# Parties UI architecture

## Direction

The UI is migrated incrementally from the original monolithic RML/RCSS surface.
`style.rcss` is the compatibility layer; `redesign.rcss` is the v2 design-system
and screen layer loaded after it. A feature moves completely to v2 before its
old rules are removed. This keeps every intermediate commit runnable.

`redesign.rcss` owns the v2 design tokens through RmlUI's native CSS-compatible
custom properties and `var()` support. Tokens cascade and inherit through the
element tree, so a screen or component can override a token locally. Undefined
names, cycles, and malformed expressions are reported through RmlUI's logging
system; fallbacks use `var(--name, value)`.

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

## Target source layout

```text
client/ui/
  app.rml
  styles/
    foundation.rcss
    primitives.rcss
    shell.rcss
    screens/
  screens/
    launcher.rml
    room.rml
    chat.rml
    settings.rml
  components/
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

## Migration sequence

1. Establish v2 foundation and migrate launcher, shell, and voice stage.
2. Introduce typed preferences and move settings state out of `LobbyModel`.
3. Add `UiSessionModel` with route and modal enums; remove mutually conflicting
   `show_*` flags.
4. Split settings, chat, share picker, and onboarding into screen documents.
5. Replace duplicated buttons, toggles, fields, menus, and dialogs with shared
   primitives/custom elements.
6. Delete migrated legacy RCSS and add screenshot regression fixtures at desktop,
   narrow desktop, macOS, and iOS sizes.

## Definition of done for a migrated screen

- Uses v2 primitives and spacing/color conventions.
- Has one view model with explicit state ownership.
- Does not call services directly from RML.
- Handles empty, loading, error, populated, and narrow layouts.
- Is exercised by model tests and a screenshot fixture.
- Has no remaining selector in `style.rcss`.

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
