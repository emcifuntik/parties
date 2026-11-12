//! Tests of the fixture core: each command leaves the state as a person
//! would expect it.

use super::*;
use crate::{
    fixtures::{
        CHANNEL_CLIPS, CHANNEL_OFF_TOPIC, MESSAGE_SARA, Pictures, ROOM_LATE_NIGHT, USER_TUXICK,
        glass_shell,
    },
    model::{StreamSource, View},
    theme::LightId,
};

fn fixture() -> (FixtureCore, ShellState) {
    (FixtureCore::new(), glass_shell(&Pictures::none()))
}

fn own_member(state: &ShellState) -> &crate::model::Member {
    state
        .members
        .groups
        .iter()
        .flat_map(|group| &group.members)
        .find(|member| member.user == USER_TUXICK)
        .expect("the fixture lists the user among the members")
}

#[test]
fn selecting_a_channel_swaps_the_chat_and_clears_unread() {
    let (mut core, mut state) = fixture();
    core.handle(&mut state, Command::SelectChannel(CHANNEL_OFF_TOPIC));
    assert_eq!(state.selected_channel, CHANNEL_OFF_TOPIC);
    assert_eq!(state.chat.name, "off-topic");
    assert!(state.text_channels.iter().all(|channel| !channel.unread));

    core.handle(&mut state, Command::SelectChannel(CHANNEL_CLIPS));
    assert_eq!(state.chat.name, "clips-and-links");
    assert_eq!(
        state.chat.items.len(),
        8,
        "the parked chat comes back whole"
    );
}

#[test]
fn the_two_menus_are_never_open_together() {
    let (mut core, mut state) = fixture();
    core.handle(&mut state, Command::ToggleStreamMenu);
    assert_eq!(state.open_menu, Some(VoiceBlockMenu::Stream));
    core.handle(&mut state, Command::TogglePresenceMenu);
    assert_eq!(state.open_menu, Some(VoiceBlockMenu::Presence));
    core.handle(&mut state, Command::TogglePresenceMenu);
    assert_eq!(state.open_menu, None);
}

#[test]
fn choosing_a_presence_updates_the_member_row_and_closes_the_menu() {
    let (mut core, mut state) = fixture();
    core.handle(&mut state, Command::TogglePresenceMenu);
    core.handle(&mut state, Command::SetPresence(Presence::Idle));
    assert_eq!(state.me.presence, Presence::Idle);
    assert_eq!(own_member(&state).presence, Presence::Idle);
    assert_eq!(own_member(&state).status, "In General");
    assert_eq!(state.open_menu, None);
}

#[test]
fn mute_and_deafen_mark_the_room_entry() {
    let (mut core, mut state) = fixture();
    let own_activity = |state: &ShellState| state.voice_rooms[0].members[0].activity;
    core.handle(&mut state, Command::ToggleMute);
    assert!(state.me.muted);
    assert_eq!(own_activity(&state), RoomActivity::Muted);
    core.handle(&mut state, Command::ToggleDeafen);
    assert_eq!(own_activity(&state), RoomActivity::Deafened);
    core.handle(&mut state, Command::ToggleMute);
    assert!(!state.me.muted && !state.me.deafened);
    assert_eq!(own_activity(&state), RoomActivity::Listening);
}

#[test]
fn joining_and_leaving_rooms_moves_the_user() {
    let (mut core, mut state) = fixture();
    core.handle(&mut state, Command::JoinRoom(ROOM_LATE_NIGHT));
    assert_eq!(state.voice_rooms[0].members.len(), 4);
    assert_eq!(state.voice_rooms[1].members[0].user, USER_TUXICK);
    assert_eq!(own_member(&state).status, "In Late night");

    core.handle(&mut state, Command::ToggleStream(StreamSource::Screen));
    assert!(state.me.streams.screen);
    core.handle(&mut state, Command::LeaveRoom);
    assert_eq!(state.me.room, None);
    assert_eq!(state.me.streams.live_count(), 0);
    assert_eq!(own_member(&state).status, "Online");
    core.handle(&mut state, Command::ToggleStream(StreamSource::Screen));
    assert!(!state.me.streams.screen, "no stream outside a room");
}

#[test]
fn reactions_toggle_and_count() {
    let (mut core, mut state) = fixture();
    let reaction = |state: &ShellState, index: usize| {
        state
            .chat
            .items
            .iter()
            .find_map(|item| match item {
                ChatItem::Message(message) if message.id == MESSAGE_SARA => {
                    Some(message.reactions[index].clone())
                }
                _ => None,
            })
            .expect("the fixture has Sara's message")
    };
    core.handle(
        &mut state,
        Command::ToggleReaction {
            message: MESSAGE_SARA,
            index: 1,
        },
    );
    assert_eq!(
        (reaction(&state, 1).count, reaction(&state, 1).mine),
        (3, true)
    );
    core.handle(
        &mut state,
        Command::ToggleReaction {
            message: MESSAGE_SARA,
            index: 0,
        },
    );
    assert_eq!(
        (reaction(&state, 0).count, reaction(&state, 0).mine),
        (3, false)
    );
}

#[test]
fn sending_appends_the_draft_and_clears_it() {
    let (mut core, mut state) = fixture();
    let before = state.chat.items.len();
    core.handle(&mut state, Command::SendDraft);
    assert_eq!(state.chat.items.len(), before + 1);
    assert!(state.draft.is_empty());
    core.handle(&mut state, Command::SendDraft);
    assert_eq!(
        state.chat.items.len(),
        before + 1,
        "an empty draft sends nothing"
    );
}

#[test]
fn light_and_view_follow_commands() {
    let (mut core, mut state) = fixture();
    core.handle(&mut state, Command::CycleLight);
    assert_eq!(state.light, LightId::Ember);
    core.handle(&mut state, Command::SetLight(LightId::Moss));
    assert_eq!(state.light, LightId::Moss);
    core.handle(&mut state, Command::ShowView(View::Home));
    assert_eq!(state.switcher.view, View::Home);
}
