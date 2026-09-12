# RmlUI Designer

The designer can run interactively or as a deterministic screenshot validator.
Automated captures render directly from the DX12 back buffer, so they also work
with the DirectComposition swap chain used by Parties.

## macOS production capture

`rmlui-designer` currently uses the Win32/DX12 backend and therefore cannot run
on macOS. The macOS regression suite uses the same fixture approach directly in
the production AppKit + Metal client, which also covers native `NSMenu`,
`NSPopover`, and `SCContentSharingPicker` UI that RmlUI cannot render:

```zsh
cmake --build --preset macos-arm64-debug --target parties_client
tools/capture-ui-macos.sh
```

The 37 reachable macOS states are written to `build/ui-screenshots-macos`. Pass an app
bundle and output directory as the first and second arguments to override the
defaults. Every state is independently reproducible with, for example:

```zsh
build-macos/client/parties_client.app/Contents/MacOS/parties_client \
  --ui-fixture native-user
```

## iOS layout profile

On macOS, capture the production UIKit + Metal client in a booted iOS Simulator:

```zsh
cmake --preset ios-simulator-arm64-debug
cmake --build --preset ios-simulator-arm64-debug
tools/capture-ui-ios.sh
```

The iOS states are written to `build/ui-screenshots-ios`, with one runtime
log per screenshot. Optional positional arguments select the app bundle, output
directory, and simulator UDID. The harness fails on launch, model initialization,
RmlUI, or screenshot errors. Stream fixtures render the shared deterministic
video frames; `stream-fullscreen` also exercises the landscape transition.
Normal screens remain locked to portrait by the production controller. Manually
verify touch scrolling, keyboard input, and returning from fullscreen in the
simulator; fixtures do not connect to a server or exercise live audio/video.

Use the production iOS RCSS media theme at the exact iPhone 17 Pro logical
viewport (402 x 874 points at 3x) with:

```powershell
build/designer/rmlui_designer.exe client/ui/lobby.rml `
  --asset-dir client/ui --profile iphone-17-pro --fixture onboarding
```

The equivalent explicit form is `--size 1206x2622 --density 3 --theme ios`.
This is a fast layout preview; final touch, safe-area, keyboard, and Metal
validation still runs in the iOS Simulator.

## Interactive preview

Desktop previews automatically receive the same `platform-windows` and
`platform-desktop` classes as the production client. The application UI is
split by responsibility: `theme.rcss` owns tokens, `typography.rcss` owns the
type scale, `components.rcss` owns reusable controls, feature files own their
screen composition, and `desktop.rcss` / `mobile.rcss` contain platform-only
density and interaction overrides.

```powershell
build/designer/rmlui_designer.exe client/ui/lobby.rml `
  --asset-dir client/ui --fixture room --size 1440x900
```

## Screenshot validation

```powershell
build/designer/rmlui_designer.exe client/ui/lobby.rml `
  --asset-dir client/ui --fixture chat --size 1440x900 `
  --screenshot build/ui-screenshots/chat.bmp
```

Screenshot mode defaults to density `1.0` and at least 240 settling frames. It
continues updating until the document tree is stable, so nested `data-for`
controllers have completed before the GPU backbuffer is captured.
It exits non-zero when the document fails to load, RmlUI reports an error, the
fixture is unknown, or the GPU readback cannot be written.

Available Parties fixtures are `onboarding`, `recovery`, `launcher`,
`party-modal`, `room`, `stream-single`, `stream-fps-overflow`, `streams`, `member`,
`settings`, `settings-select-open`, `settings-screen-share`, `settings-hotkeys`, `settings-account`,
`share`, `audio-share`, and `chat`. They use the real `LobbyModel`, `ServerListModel`, and `ChatModel`,
including structured arrays and the production custom elements. Stream
fixtures feed deterministic frames through `VideoElement`; the `share` fixture
uses the same path for target thumbnails so the picker cards are covered by
automated rendering as well.

Capture the primary regression set with:

```powershell
tools/capture-ui.ps1
```

Capture the production iOS theme at iPhone scale with:

```powershell
tools/capture-ui.ps1 -Profile iphone-17-pro `
  -OutputDirectory build/ui-screenshots-ios
```

Use `--vars file.vars` for generic scalar/string-array documents. Parties
screens should use named fixtures because their models contain nested structs.

### Control layout audit

Both Apple capture scripts wait for a completed Metal frame and check the production
RmlUI layout before saving a screenshot. They fail on clipped or off-center labels,
unlaid-out button text, unreachable actions outside a non-scrolling viewport, and
iOS click targets smaller than 44 by 44 points. Each scenario has its own log.
The `island-idle` and `island-long-name` fixtures also check the compact nickname
and fingerprint group against the avatar center, including text truncation.
Screenshots still require visual review for composition, contrast, and native UI.

Run the macOS matrix at both the default 1280 by 720 points and the minimum size:

```sh
UI_WIDTH=800 UI_HEIGHT=500 tools/capture-ui-macos.sh
```

Use `UI_SCENARIOS="launcher onboarding recovery"` with either capture script to
repeat a subset. Verify scrolling, keyboard input, focus, and menu interactions in
the running app in addition to the static fixtures. Do not run a capture script
and manual interaction against the same native app at the same time.

The `chat` and `chat-draft` fixtures check empty and populated composers. The layout
audit measures the rendered input text against the capsule center on both platforms.
On iOS, page backgrounds extend behind the home indicator. Only the sidebar island,
chat composer and stream controls consume the bottom safe-area inset. Do not shorten
the entire page or add a native background strip. Check the channels page as well as
chat, settings, voice rooms and dialogs; each page must retain its own background.

The iOS `chat-keyboard` fixture focuses the production composer after data binding
has settled and fails if the software keyboard does not appear. Enable the
Simulator software keyboard before running this scenario. The safe-area regression
also checks portrait, landscape and keyboard viewport sizes, including fullscreen
controls that do not inherit the document's horizontal padding.

The `settings-airpods` fixture uses deterministic AVAudioSession-style port data
to render the selected headset. It does not emulate Bluetooth hardware. Verify
input, output, speaker switching and headset disconnection on a physical iPhone
with connected AirPods before marking hardware audio validation complete.

For iOS navigation, verify the sequence fullscreen stream → stream viewer → voice
room → channels, and settings → channels. Voice membership must survive all of
these returns. Verify chat back with and without the software keyboard. Short drags,
vertical movement and drags starting in the middle must keep the page open. Use
`UI_SCENARIOS="chat-back-blocked room-back-blocked"` to verify that an open dialog
prevents navigation behind it. Some Simulator control bridges
deliver only touch-down and touch-up for a drag; the client handles that coalesced
sequence as well as UIKit's continuous screen-edge pan recognizer.
