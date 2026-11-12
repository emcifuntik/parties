# Design

The design source is `design.pen` in the repository root (a Pencil document).
This folder holds pictures that go with it.

| Folder | Contents |
|---|---|
| `current-app-2026-11/` | Screenshots of the C++ / RmlUi client as of 10 Nov 2026: 38 desktop states (1440×900) and 20 iPhone states (iPhone 17 Pro profile, 1206×2130, bottom cropped by the capture window). The "Current app" board in `design.pen` shows these files. |
| `redesign-2026-11/` | PNG exports of every frame of the first redesign: 26 desktop frames plus the component sheet, and 12 iPhone screens at 2×. The owner did not fully accept this look; it stays as a record of the page inventory. |
| `directions-2026-11/` | Round 1 of the incremental restart: Home, Chat and Settings drawn in five directions (A Graphite, B Stage, C Daylight, D Terminal, E Glass), with a style card each. One overview picture per direction sits at the top of the folder. |
| `glass-2026-11/` | The direction the owner chose (E Glass), reworked so it no longer imitates its reference, and every page ported to it. Top level: Chat in the eight lights (`eight-lights.png` shows them side by side), Home, Settings (Voice and audio, Appearance), the voice block states. `pages/`: the other 25 desktop frames (inside a party, user settings, party settings, flows, dialogs and menus). `iphone/`: the 13 iPhone screens at 2×, with `iphone-sheet-1..4.png` as overviews. `resize/`: one settings page drawn at smaller window sizes (an exploration, see "Smaller windows"). |

`implementation-2026-11/` is different from the folders above: it holds
captures of the real window, built with lurq in `crates/parties-ui` from
fixture data (the chat window, the same with the presence menu and with the
Stream menu open, the Ember light, and the window at 1020 × 640). Compare
`glass_shell.png` there with `glass-2026-11/chat-night.png`. Refresh them with
`cargo run -p parties-ui --example glass_shell -- --capture <file>` (options
`--menu stream|presence`, `--light <name>`, `--size WxH`).

The pictures inside `design.pen` (avatars, party covers, attachments) are files
in the repository's `images/` folder, where Pencil writes what it generates.

**Vocabulary.** The user interface calls a server a "party" (plural
"parties"); documents about the protocol and the server keep saying "server".
The Glass frames follow this. The frames of the first redesign and of the
direction rounds were drawn earlier and still say "space".

## Map of `design.pen`

| Where on the canvas | What |
|---|---|
| x −44…2616, y 0…2684 | The earlier "Screen v3" frames, unchanged |
| x −44, y 3100 | "Current app · Nov 2026" — the reference screenshot board |
| x 4200, y 0 | `Redesign/Components` — tokens in use and the reusable components |
| x 6200…, y 0 | Core pages: Voice room (the reference screen), Home, Text chat, Stream focus, Streams grid, Share picker |
| x 6200…, y 1040 | Your settings: Voice and audio, Video and sharing, Profile, Keys and devices, Hotkeys, App and notifications |
| x 6200…, y 2080 | Space settings: Overview, Roles, Members, Join codes, Audit log and bans, Channel permissions |
| x 6200…, y 3120 | Onboarding, joining with a code, verifying the space key, hardware keys, profile card with rename and presence, picture crop, dialogs and system states |
| x 6200…, y 4160 | iPhone: Spaces, Channels, Voice room, Stream, Chat, Members, Profile sheet, Join with a code, Verify space key, Hardware key, You, Keys and devices |
| x 6200…, y 5360…11180 | Design directions, round 1: one row per direction (A to E), each with Home, Chat, Settings and a style card. Frames are named `Directions/<letter> <name> · <window>` and use raw colour values, not the `r-` variables |
| x 6200…, y 11700 | Glass, second pass: the chat window in the eight lights (`Glass 2/Chat · <Light>`) |
| x 6200…, y 12860 | Glass, second pass: Home, Settings · Voice and audio, Settings · Appearance (the light picker), the voice block states |
| x 4200, y 11700 | `Glass 2/Components`: the shared title bar and voice block. Pages hold instances of them, so a change here reaches every page |
| x 6200…, y 14020 | Glass, inside a party: Voice room, Stream focus, Streams grid, Share picker, Party menu (where "Create a join code" lives), Profile card with rename and presence |
| x 6200…, y 15180 | Glass, user settings: Profile, Keys, Video and sharing, Hotkeys, App and notifications |
| x 6200…, y 16340 | Glass, party settings: Overview, Roles, Members, Join codes, Audit log and bans, Channel permissions |
| x 6200…, y 17500 | Glass, flows: Create identity, Restore and back up, Enter a join code, Verify the party key, Hardware key, Picture crop, Dialogs, menus and system states, Passkey |
| x 6200…, y 18660 | Glass, iPhone: thirteen screens (the last one is Passkey) |
| x 6200…, y 19820 | Glass, smaller windows (`Glass/Resize · …`): Settings · App and notifications at 1020 × 640 and at 720 × 450 |

## Glass lights

The chosen direction is dark floating glass panels over a soft coloured
backdrop. The colour is a user setting (Settings, Appearance): one of eight
lights. A light is a base colour, two radial washes and one accent; panels,
text and state colours are the same in every light.

| Light | Base | Wash 1 (top left) | Wash 1 at 45% | Wash 2 (bottom right) | Accent |
|---|---|---|---|---|---|
| Night (default) | `#090B0F` | `#17607F` | `#0F435C` | `#0F4A46` | `#8FDCFF` |
| Ember | `#0C0A0C` | `#7A2436` | `#5A1A2B` | `#4A2140` | `#FFB38F` |
| Smoke | `#0B0C0D` | `#444B57` | `#2E333C` | `#2C322D` | `#C8F169` |
| Ocean | `#090A10` | `#1F4596` | `#16316E` | `#123A5E` | `#A3BCFF` |
| Dusk | `#0B0A10` | `#51308A` | `#3A2266` | `#2A2460` | `#C9B0FF` |
| Rose | `#0D0A0C` | `#86245F` | `#5F1A45` | `#47205A` | `#FFA3CF` |
| Honey | `#0C0B09` | `#71501A` | `#523A12` | `#4E2C16` | `#FFD27A` |
| Moss | `#090C0A` | `#1E5E43` | `#154533` | `#3B4A1E` | `#86E8AE` |

The top-left wash has a second, darker stop at 45% of its radius, at 60%
opacity, and then fades to nothing; the bottom-right wash fades straight out.
The values in code are in `crates/parties-ui/src/theme/lights.rs`.

Labels on the accent are near-black (`#0A0A0C`). Leaving a call and errors stay
red (`#FF5A52` family) in every light; presence stays green and amber.
Typefaces: Bricolage Grotesque for titles, Hanken Grotesk for text, Geist Mono
for codes and fingerprints. Radii: 20 panels, 14 blocks inside a panel, 12
controls, 8 chips.

There is no floating bar at the bottom of the window. The voice state is a
block at the bottom of the left panel, so the panels run to the bottom edge.
The block is also the mini profile: your picture with the presence dot, your
name, and under it the room you are in ("In General"; your presence when you
are in no room), with the latency on the right. Clicking the picture or the
name opens the presence menu upwards (Online, Idle, Do not disturb, Change
picture). That menu is the one place on the desktop where presence is
changed: it is deliberately not a settings page, so it stays one click away.
On the iPhone the same three options are on the You tab. Who is speaking is not repeated here; the channel list and the
member list already show it. Under the profile line sits a row of three
labelled buttons (Mic, Sound, Leave) and, under it, one full-width Stream
button. Stream opens a list upwards with Screen, Camera and Music; each can be
on by itself or together, and a source that is on is marked "Live" in the list.
While anything is live the button is lit in the accent and names it ("Screen is
live", "Camera is live", "2 streams live"). `voice-block-states.png` shows the
states. On the iPhone voice room the same two rows sit above the tab bar.
"Create a join code" is in the party menu, not in the left panel.

The title bar carries the brand, the switcher, the update chip when there is
one, and the window buttons; no search field and no profile chip (the profile
is in the voice block, and on Home in the card on the right). The
switcher is Home, Settings, a divider, then the parties and "+". The view you
are on is the wide item with its label. The party you are connected to stays
lit in the accent (tinted fill and ring) on every view, including Settings, and
clicking it is how you go back: Settings has no Close button.

### Removed because the first version does not build it

Decision D-30 in [../rust-rewrite/12-decisions.md](../rust-rewrite/12-decisions.md).
These were drawn and then deleted from the Glass frames, so a frame shows what
is built:

- the "Signed-in devices" card (desktop and iPhone). Its page was called
  "Keys and devices" and is now "Keys";
- the "Global shortcuts" card on Hotkeys (shortcuts simply work in the
  background, with no setting);
- the "Message" action in the profile card, the member menu and the iPhone
  profile sheet (no direct messages);
- status text, the "Invisible" presence and "Show as idle after 10 minutes
  away": presence is Online, Idle or Do not disturb, chosen by hand.

### Where the key lives

A key has three possible homes, offered when an identity is created and
listed on the Keys page: in a passkey (recommended), on this device, on a
hardware key. The passkey is presented as the copy that cannot be lost, not as
a security feature: it is asked for once per device and never at every start.
An account can hold several keys; a new one is linked to each party as the
client connects, and the pages show that progress. `pages/flow-passkey.png`
has the dialogs (unlock, save in a passkey, linking, not supported here);
the technical design is in
[../rust-rewrite/13-profiles-access-and-hardware-keys.md](../rust-rewrite/13-profiles-access-and-hardware-keys.md)
section 7.

### Smaller windows

An exploration, not yet agreed. The desktop frames are drawn at 1440 × 900;
`resize/` shows how one settings page gives way as the window shrinks:

| Window | What changes |
|---|---|
| 1020 × 640 (half the area) | The two columns of cards become one column that scrolls. The left panel, with the profile and voice block, is unchanged |
| 720 × 450 (half of each side) | The left panel becomes a rail of icons, the page subtitle is dropped and cards get tighter padding. The voice block shrinks to your picture, Mic and Leave; clicking the picture opens the full block as a popover |

A page that scrolls runs to the bottom edge of the window and is cut by the
edge itself, with no fade (a fade into the backdrop was tried at two heights
and rejected). Only the panels keep the 12 px margin from the bottom edge;
scrolling content is never cut off above it.

The current client refuses to go below 800 × 600, so the second size is
smaller than any window a user can have today. At 450 px of height the rail is
exactly full (seven section icons plus the picture, Mic and Leave), so this
layout cannot go lower without scrolling the rail.

Redesign tokens are document variables prefixed `r-` (backgrounds, lines, text,
accent, presence, status and role colours, radii). The typefaces are Inter and,
for fingerprints, join codes and technical read-outs only, DM Mono.

The features the redesign introduces are specified in
[../rust-rewrite/13-profiles-access-and-hardware-keys.md](../rust-rewrite/13-profiles-access-and-hardware-keys.md);
its last section lists which frame draws each of them.

## Refreshing the current-app screenshots

```powershell
cmake --build --preset default --target rmlui_designer
tools/capture-ui.ps1 -Reference -OutputDirectory build/ui-reference/desktop
tools/capture-ui.ps1 -Reference -Profile iphone-17-pro -OutputDirectory build/ui-reference/ios
```

`-Reference` captures every state as PNG, including the secondary ones that
have no layout contract; see [../../designer/README.md](../../designer/README.md).
On the iPhone profile chat, streams and settings are native views, so they
come out as the room or as an empty frame and are left out of this folder.
