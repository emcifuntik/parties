# 05 — Permissions and roles

> Status: Draft — 2026-11-10.

## 1. Goals

1. The same expressive power as Discord: any number of roles, an ordered
   hierarchy, per-channel allow/deny overwrites for roles and for individual
   members, categories, and an administrator bypass.
2. One implementation (`parties-perm`) used by the server to enforce, by the
   client to render and preview, and by `partiesctl`.
3. Enforcement that cannot be forgotten: a request type without a declared
   rule does not compile.
4. Rules that hold on the media path too: permission to speak or stream is
   checked for every forwarded packet through the routing snapshot, not only
   when a stream starts.
5. A few numeric limits per role, borrowed from TeamSpeak, for things a
   yes/no bit cannot express (stream quality, upload size).

Today's model is four fixed roles and a 15-bit mask of which most bits are
never checked (finding F7 in [04](04-connection-security.md)). Nothing of it
is kept except the migration path in section 12.

## 2. Model

```mermaid
erDiagram
    SERVER ||--|| USER : "owned by"
    SERVER ||--o{ ROLE : has
    SERVER ||--o{ CHANNEL : has
    USER }o--o{ ROLE : "member_roles"
    CHANNEL ||--o{ CHANNEL : "category contains"
    CHANNEL ||--o{ OVERWRITE : has
    OVERWRITE }o--|| ROLE : "targets role, or"
    OVERWRITE }o--|| USER : "targets member"
```

| Entity | Key properties |
|---|---|
| Server | One community per server process. Has exactly one **owner** |
| Role | Name, colour, `position` (unique integer, higher is more senior), `permissions` (allow mask), numeric limits, display flags (shown separately, mentionable) |
| `@everyone` | The built-in role every member has. Position 0. Cannot be deleted, moved or assigned |
| Member | A user who has joined: set of roles, nickname, moderation state |
| Channel | Kind (category, voice, text), parent category, position, per-channel settings |
| Overwrite | For one channel and one target (a role or a member): an `allow` mask and a `deny` mask |

Categories carry overwrites like any channel. A channel created inside a
category starts with a copy of the category's overwrites. A channel whose
overwrites equal its category's is shown as "synced"; editing the category
then updates it too, and `SyncChannelPermissions` restores the link.
Resolution itself only ever looks at the channel's own overwrites, so there
is one level to reason about.

## 3. Permission catalogue

Permissions are bits in a 64-bit mask. Bit 63 is never used, so a mask fits a
signed 64-bit integer in SQLite and in clients without unsigned types.
Scope **S** means the bit is evaluated server-wide only; **C** means it can
also be allowed or denied per channel.

### General

| Bit | Name | Scope | Grants |
|---|---|---|---|
| 0 | `ViewChannel` | C | See the channel, its members and activity |
| 1 | `ManageChannels` | C | Create, edit, reorder and delete channels |
| 2 | `ManageRoles` | C | Server-wide: create and edit roles below one's own. In a channel: edit its overwrites |
| 3 | `ManageServer` | S | Edit server settings |
| 4 | `ViewAuditLog` | S | Read the audit log |
| 5 | `CreateInvite` | C | Create invites |
| 6 | `ManageInvites` | S | List and revoke others' invites |
| 7 | `KickMembers` | S | Remove a member |
| 8 | `BanMembers` | S | Ban and unban |
| 9 | `TimeoutMembers` | S | Temporarily silence a member |
| 10 | `ChangeNickname` | S | Change one's own nickname |
| 11 | `ManageNicknames` | S | Change others' nicknames |
| 12 | `ManageExpressions` | S | Manage custom emoji and soundboard clips |
| 13 | `ApproveMembers` | S | Approve or reject join requests |

### Text

| Bit | Name | Scope | Grants |
|---|---|---|---|
| 16 | `SendMessages` | C | Post messages |
| 17 | `ReadMessageHistory` | C | Read earlier messages, search, download attachments |
| 18 | `AttachFiles` | C | Upload files |
| 19 | `EmbedLinks` | C | Have links expanded into previews |
| 20 | `AddReactions` | C | Add new reactions |
| 21 | `MentionEveryone` | C | Use mentions that notify everyone or a whole role |
| 22 | `ManageMessages` | C | Delete others' messages, remove reactions |
| 23 | `PinMessages` | C | Pin and unpin |
| 24 | `BypassSlowMode` | C | Ignore the channel's slow mode |

### Voice and video

| Bit | Name | Scope | Grants |
|---|---|---|---|
| 32 | `Connect` | C | Join the voice channel |
| 33 | `Speak` | C | Have one's voice forwarded |
| 34 | `UseVoiceActivity` | C | Transmit without push-to-talk |
| 35 | `PrioritySpeaker` | C | Others are attenuated while this member speaks |
| 36 | `StreamScreen` | C | Publish a screen share and its audio |
| 37 | `StreamCamera` | C | Publish a camera |
| 38 | `UseAuxAudio` | C | Publish auxiliary audio: soundboard, music, application audio |
| 39 | `MuteMembers` | C | Server-mute others |
| 40 | `DeafenMembers` | C | Server-deafen others |
| 41 | `MoveMembers` | C | Move others between channels the actor can connect to |
| 42 | `DisconnectMembers` | C | Remove others from voice |
| 43 | `BypassUserLimit` | C | Join a full channel |
| 44 | `RequestToSpeak` | C | Raise a hand in a moderated channel |
| 45 | `SetChannelStatus` | C | Set the voice channel's status line |

### Special

| Bit | Name | Scope | Grants |
|---|---|---|---|
| 62 | `Administrator` | S | Every permission, in every channel; overwrites do not apply |

Bits 14–15, 25–31, 46–61 are reserved. Unknown bits are stored and passed
through unchanged, so an older client can edit a role without erasing bits a
newer server understands.

## 4. Resolution

```rust
pub fn effective(member: &MemberView, server: &ServerView, channel: Option<&ChannelView>) -> Permissions {
    if member.user_id == server.owner_id {
        return Permissions::ALL;
    }

    // 1. Server-wide base: @everyone plus every role the member has.
    let mut perms = server.everyone.permissions;
    for role in &member.roles {
        perms |= role.permissions;
    }
    if perms.contains(Permissions::ADMINISTRATOR) {
        return Permissions::ALL;
    }

    // 2. Channel overwrites, least specific first.
    let base = perms;
    if let Some(ch) = channel {
        if let Some(ow) = ch.overwrite_for_role(server.everyone.id) {
            perms = (perms & !ow.deny) | ow.allow;
        }
        let (mut allow, mut deny) = (Permissions::empty(), Permissions::empty());
        for role in &member.roles {
            if let Some(ow) = ch.overwrite_for_role(role.id) {
                allow |= ow.allow;
                deny |= ow.deny;
            }
        }
        perms = (perms & !deny) | allow;          // among roles, allow wins over deny
        if let Some(ow) = ch.overwrite_for_member(member.user_id) {
            perms = (perms & !ow.deny) | ow.allow; // the member overwrite has the last word
        }
        // Overwrites cannot grant or remove a server-scope bit.
        perms = (perms & !Permissions::SERVER_SCOPE) | (base & Permissions::SERVER_SCOPE);
    }

    // 3. Implied restrictions.
    apply_implications(perms, member, channel)
}
```

Implied restrictions, applied last:

| Condition | Effect |
|---|---|
| No `ViewChannel` in the channel | No permission in that channel at all |
| No `Connect` | No voice or video permission in that channel |
| No `SendMessages` | No `AttachFiles`, `EmbedLinks`, `MentionEveryone` |
| Member is timed out | Only `ViewChannel` and `ReadMessageHistory` remain |
| Member is younger than the server's new-member period | The bits listed in the server's "restricted until verified" setting are removed |

This is deliberately the algorithm Discord documents, so administrators who
know Discord get the behaviour they expect, and so its edge cases are well
understood.

## 5. Hierarchy rules

Let `top(m)` be the highest position among a member's roles (0 with only
`@everyone`).

| # | Rule |
|---|---|
| H1 | The owner passes every check and cannot be the target of any moderation action. Only the owner can transfer ownership |
| H2 | A member can create, edit, delete or reorder a role only if the role's position is **strictly below** `top(actor)` |
| H3 | A member can assign or revoke a role only if the role is strictly below `top(actor)`, and, when the target is someone else, `top(target)` is strictly below `top(actor)` |
| H4 | **No escalation.** A member cannot put a permission bit into a role, or into the `allow` mask of an overwrite, unless they hold that bit themselves (server-wide for roles, in that channel for overwrites). The same applies to numeric limits: nobody can grant a limit above their own |
| H5 | Kick, ban, timeout, nickname change, server mute and deafen, move and disconnect require `top(target)` strictly below `top(actor)` |
| H6 | Editing a channel's overwrites needs `ManageRoles` in that channel. An overwrite for a role at or above `top(actor)` cannot be edited |
| H7 | Equal top positions: neither member can act on the other |
| H8 | `@everyone` cannot be deleted, moved or assigned. Its permissions can be edited under H4 |
| H9 | `Administrator` holders are still bound by H2, H3 and H5. Only position decides who outranks whom |
| H10 | The owner cannot leave the server or be removed; ownership must be transferred first |

H4 and H9 together mean a moderator can never hand out more than they have,
and two administrators cannot demote each other unless one outranks the
other.

## 6. Role limits

Numeric limits answer "how much", where a bit can only answer "whether".

| Limit | Unit | Default for `@everyone` |
|---|---|---|
| `max_video_bitrate` | kbit/s | 8000 |
| `max_video_height` | pixels | 1080 |
| `max_video_fps` | frames/s | 60 |
| `max_aux_audio_bitrate` | kbit/s | 128 |
| `max_upload_size` | MiB | 50 |
| `daily_upload_quota` | MiB | 500 |
| `max_published_video_tracks` | count | 2 |

A member's limit is the **maximum** over their roles. The value actually
applied is the minimum of that, the server-wide cap, and the channel's own
cap where one exists (for example a channel bitrate). The owner and
administrators get the server-wide cap. Limits are sent to the client in
`Ready` so the UI can offer only what the server will accept; the server
enforces them regardless.

## 7. Enforcement

### 7.1 One gate for requests

```rust
pub enum Scope { Server, Channel(ChannelId), ChannelOf(MessageId) }
pub enum Hierarchy { None, TargetMember(UserId), TargetRole(RoleId) }

pub struct Requirement {
    pub scope: Scope,
    pub all_of: Permissions,
    pub hierarchy: Hierarchy,
    pub extra: Extra,          // e.g. "or is the author", "no escalation check on these masks"
}

pub fn requirement(request: &Request) -> Requirement {
    match request {            // exhaustive: a new request type must add an arm
        Request::SendMessage(r)   => channel(r.channel_id, SEND_MESSAGES),
        Request::PinMessage(r)    => channel_of(r.message_id, PIN_MESSAGES),
        Request::DeleteMessage(r) => channel_of(r.message_id, MANAGE_MESSAGES).or_author(),
        Request::KickMember(r)    => server(KICK_MEMBERS).over_member(r.user_id),
        Request::UpdateRole(r)    => server(MANAGE_ROLES).over_role(r.role_id).no_escalation(r.permissions),
        Request::JoinVoice(r)     => channel(r.channel_id, VIEW_CHANNEL | CONNECT),
        // ...
    }
}
```

The world task calls `authorize(actor, requirement)` before it touches state.
Handlers receive a proof-of-authorization value that only `authorize` can
construct, so a handler cannot be called without the check.

### 7.2 Representative requirements

| Request | Scope | Permission | Hierarchy / extra |
|---|---|---|---|
| `JoinVoice` | Channel | `ViewChannel`, `Connect` | User limit unless `BypassUserLimit` |
| `PublishTrack` (screen) | Current voice channel | `StreamScreen` | Role limits |
| `SendMessage` | Channel | `SendMessages` | Attachments need `AttachFiles`; slow mode unless `BypassSlowMode` |
| `FetchHistory`, `SearchMessages`, download | Channel | `ViewChannel`, `ReadMessageHistory` | |
| `DeleteMessage` | Channel of message | `ManageMessages` | Or the author |
| `PinMessage` | Channel of message | `PinMessages` | |
| `CreateChannel` | Parent category or server | `ManageChannels` | |
| `SetOverwrite` | Channel | `ManageRoles` | H4, H6 |
| `CreateRole`, `UpdateRole` | Server | `ManageRoles` | H2, H4 |
| `AssignRole` | Server | `ManageRoles` | H3 |
| `KickMember` | Server | `KickMembers` | H5 |
| `BanMember` | Server | `BanMembers` | H5 |
| `ServerMute` | Target's voice channel | `MuteMembers` | H5 |
| `MoveMember` | Source and destination channel | `MoveMembers` in source, `Connect` in destination | H5 |
| `UpdateServerSettings` | Server | `ManageServer` | |
| `FetchAuditLog` | Server | `ViewAuditLog` | |
| `TransferOwnership` | Server | Owner only | |

### 7.3 Visibility

`ViewChannel` controls what a session is told, not only what it may do. The
world filters the `Ready` snapshot and every event per recipient: a hidden
channel's name, members, voice states, tracks and messages are never sent.
When a member gains or loses `ViewChannel`, they receive a channel-created or
channel-deleted event for it.

### 7.4 Media path

The routing snapshot stores, per track, whether it may be forwarded, computed
by the world from the same `effective()` function. Forwarders only read that
flag. Any change to roles, overwrites, member roles or moderation state bumps
a permission epoch; the world recomputes the affected members and publishes a
new snapshot.

### 7.5 Revocation takes effect immediately

| The member loses | What happens |
|---|---|
| `ViewChannel` or `Connect` on their current voice channel | They are removed from the channel |
| `Speak` | Their voice is no longer forwarded; the client shows why |
| `StreamScreen`, `StreamCamera`, `UseAuxAudio` | The server unpublishes those tracks |
| A text permission | Later requests are refused |
| `ViewChannel` on a text channel | The channel disappears from their client |

## 8. Moderation state

| State | Set with | Effect |
|---|---|---|
| Timeout until T | `TimeoutMembers` | Section 4: read-only everywhere. Ends automatically |
| Server-muted | `MuteMembers` | Voice not forwarded, in every channel, until cleared |
| Server-deafened | `DeafenMembers` | Receives no audio |
| Temporary speak grant | `MuteMembers` in a moderated channel | Lets a member who raised a hand speak until they leave the channel |
| Ban | `BanMembers` | Identity and/or address range; optional expiry; see [04](04-connection-security.md) §6.3 |

A **moderated channel** is an ordinary voice channel where `@everyone` is
denied `Speak` and a speaker role is allowed it. `RequestToSpeak` plus the
temporary grant give it the "hand raise" flow without a separate channel
type. This also covers TeamSpeak's talk-power use case.

Every action in this section and every structural change writes an audit-log
entry: actor, action, target, before and after values, optional reason.

## 9. Defaults for a new server

| Role | Position | Permissions |
|---|---|---|
| `@everyone` | 0 | `ViewChannel`, `CreateInvite`, `ChangeNickname`, `SendMessages`, `ReadMessageHistory`, `AttachFiles`, `EmbedLinks`, `AddReactions`, `Connect`, `Speak`, `UseVoiceActivity`, `StreamScreen`, `StreamCamera`, `UseAuxAudio`, `RequestToSpeak` |
| Moderator | 1 | Adds `KickMembers`, `TimeoutMembers`, `ManageNicknames`, `ManageMessages`, `PinMessages`, `MuteMembers`, `DeafenMembers`, `MoveMembers`, `DisconnectMembers`, `ViewAuditLog`, `ApproveMembers` |
| Admin | 2 | `Administrator` |

The first-run wizard offers two presets on top of this: **Friends** (open
defaults as above) and **Community** (invite-only join policy, new members
restricted for ten minutes, `MentionEveryone` and `CreateInvite` removed from
`@everyone`).

## 10. What clients see

- All roles, in full. Role names, colours and permission masks are not
  secret, and members need them to render names and the member list.
- The overwrites of channels they can view.
- Their own effective permissions per channel and their limits, computed by
  the server and updated through `PermissionsUpdated`. The client uses these
  to show or hide controls. It uses `parties-perm` locally only for the role
  editor's live preview and a "view server as role" mode.

A client never needs to be trusted: hiding a button is a convenience, the
gate in section 7 is the rule.

## 11. Storage

```sql
CREATE TABLE roles (
    id           INTEGER PRIMARY KEY,
    name         TEXT    NOT NULL,
    color        INTEGER NOT NULL DEFAULT 0,
    position     INTEGER NOT NULL UNIQUE,
    permissions  INTEGER NOT NULL,          -- 64-bit mask, bit 63 unused
    limits       TEXT    NOT NULL DEFAULT '{}',
    flags        INTEGER NOT NULL DEFAULT 0, -- everyone, hoisted, mentionable, managed
    created_at   INTEGER NOT NULL
);

CREATE TABLE member_roles (
    user_id  INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    role_id  INTEGER NOT NULL REFERENCES roles(id) ON DELETE CASCADE,
    PRIMARY KEY (user_id, role_id)
) WITHOUT ROWID;

CREATE TABLE overwrites (
    channel_id   INTEGER NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
    target_kind  INTEGER NOT NULL,          -- 0 role, 1 member
    target_id    INTEGER NOT NULL,
    allow        INTEGER NOT NULL,
    deny         INTEGER NOT NULL,
    PRIMARY KEY (channel_id, target_kind, target_id),
    CHECK (allow & deny = 0)
) WITHOUT ROWID;
```

Role reordering, role deletion and overwrite sync run in one transaction
each. The in-memory model in the world task is authoritative at run time and
is rebuilt from these tables on start. The remaining tables are in
[06-server-design.md](06-server-design.md).

## 12. Migration from 1.4

| 1.4 | 2.0 |
|---|---|
| User with role Owner (from `root_fingerprints`) | Server owner. If several, the oldest becomes owner and the rest get Admin |
| Admin | Member with the Admin role |
| Moderator | Member with the Moderator role |
| User | Member with no extra role |
| `channel_permissions` rows | Overwrites: a role that lacked `JoinChannel` on a channel gets `deny Connect` there |
| Voice channels and text channels | Channels of the matching kind under two generated categories |
| Server password | Join policy "Password" with the same password; existing users are already members |

The importer prints a report of every mapping it applied so the operator can
review the result before opening the server.

## 13. Mapping to Discord and TeamSpeak concepts

| Discord | TeamSpeak 3 | Parties 2.0 |
|---|---|---|
| Role, role position | Server group, group sort and member-add power | Role, position (H2–H5) |
| Channel permission overwrite | Channel group, channel-specific permission | Overwrite on role or member |
| Category with synced permissions | Parent channel with inherited settings | Category, `SyncChannelPermissions` |
| `ADMINISTRATOR` | Server Admin group | `Administrator` |
| Server owner | ServerAdmin privilege key holder | Owner, claimed with a one-time token ([07](07-server-management.md)) |
| Timeout | — | Timeout |
| Stage channel, request to speak | Talk power, talk request | Moderated channel, `RequestToSpeak` |
| Nitro stream quality | Numeric permissions such as maximum bitrate | Role limits |
| Invite with role (via onboarding) | Privilege key | Invite that grants roles |
| — | Identity security level | Identity level |

## 14. Testing

| Kind | What it proves |
|---|---|
| Golden vectors (JSON, shared with any future non-Rust client) | The algorithm in section 4 on hand-written cases, including every implied restriction |
| Property tests | Owner and administrator always resolve to all; a member overwrite always overrides role overwrites; adding a role never removes a server-wide bit |
| Model-based test of H4 | For random sequences of permitted operations by a non-administrator, the actor's effective permissions never gain a bit |
| Hierarchy tests | No request succeeds against a target at or above the actor's top position |
| Authorization matrix | Generated from `requirement()`: every request type is refused without its permission |
| Visibility test | A bot without `ViewChannel` records every frame it receives; none mentions the hidden channel |
| Revocation tests | Section 7.5, end to end, including that forwarded audio stops within one snapshot update |
