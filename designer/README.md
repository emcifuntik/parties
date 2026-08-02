# RmlUI Designer

The designer can run interactively or as a deterministic screenshot validator.
Automated captures render directly from the DX12 back buffer, so they also work
with the DirectComposition swap chain used by Parties.

## Interactive preview

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
`settings`, `settings-select-open`, `settings-screen-share`, `settings-account`,
`share`, `audio-share`, and `chat`. They use the real `LobbyModel`, `ServerListModel`, and `ChatModel`,
including structured arrays and the production custom elements. Stream
fixtures feed deterministic frames through `VideoElement`; the `share` fixture
uses the same path for target thumbnails so the picker cards are covered by
automated rendering as well.

Capture the primary regression set with:

```powershell
tools/capture-ui.ps1
```

Use `--vars file.vars` for generic scalar/string-array documents. Parties
screens should use named fixtures because their models contain nested structs.
