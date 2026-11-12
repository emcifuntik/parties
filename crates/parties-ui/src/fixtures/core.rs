//! A stand-in for the client core: it answers every [`Command`] by editing
//! the state in memory, so the fixture window reacts to clicks.
//!
//! This is deliberately naive (no network, no clock, no validation). It
//! exists to exercise the reactive path from a control to the views.

use std::collections::HashMap;

use super::other_chat;
use crate::{
    commands::Command,
    model::{
        ChannelId, Chat, ChatItem, Message, MessageId, Presence, RoomActivity, RoomId, RoomMember,
        RoomRef, ShellState, Streams, VoiceBlockMenu,
    },
};

/// Remembers the chats of the channels that are not in view.
#[derive(Debug)]
pub struct FixtureCore {
    parked_chats: HashMap<ChannelId, Chat>,
    next_message: u32,
}

impl Default for FixtureCore {
    fn default() -> Self {
        Self::new()
    }
}

impl FixtureCore {
    pub fn new() -> Self {
        Self {
            parked_chats: HashMap::new(),
            next_message: 1000,
        }
    }

    /// Applies `command` to `state`.
    pub fn handle(&mut self, state: &mut ShellState, command: Command) {
        match command {
            Command::ShowView(view) => state.switcher.view = view,
            Command::SelectChannel(channel) => self.select_channel(state, channel),
            Command::JoinRoom(room) => join_room(state, room),
            Command::LeaveRoom => leave_room(state),
            Command::ToggleMute => {
                state.me.muted = !state.me.muted;
                if !state.me.muted {
                    // Speaking while deafened makes no sense: unmuting turns the sound back on.
                    state.me.deafened = false;
                }
                sync_own_activity(state);
            }
            Command::ToggleDeafen => {
                state.me.deafened = !state.me.deafened;
                sync_own_activity(state);
            }
            Command::ToggleStreamMenu => toggle_menu(state, VoiceBlockMenu::Stream),
            Command::TogglePresenceMenu => toggle_menu(state, VoiceBlockMenu::Presence),
            Command::CloseMenus => state.open_menu = None,
            Command::ToggleStream(source) => {
                // Streams go to a voice room; outside one there is nothing to start.
                if state.me.room.is_some() {
                    let live = state.me.streams.is_live(source);
                    state.me.streams.set(source, !live);
                }
            }
            Command::SetPresence(presence) => {
                set_presence(state, presence);
                state.open_menu = None;
            }
            // The picture crop flow is not built yet: the row only closes the menu.
            Command::ChangePicture => state.open_menu = None,
            Command::SetLight(light) => state.light = light,
            Command::CycleLight => state.light = state.light.next(),
            Command::ToggleReaction { message, index } => toggle_reaction(state, message, index),
            Command::ToggleMembersPanel => state.members_panel_open = !state.members_panel_open,
            Command::SendDraft => self.send_draft(state),
        }
    }

    fn select_channel(&mut self, state: &mut ShellState, channel: ChannelId) {
        let Some(target) = state
            .text_channels
            .iter_mut()
            .find(|entry| entry.id == channel)
        else {
            return;
        };
        target.unread = false;
        let target = target.clone();
        state.selected_channel = channel;
        if state.chat.channel == channel {
            return;
        }
        let next = self
            .parked_chats
            .remove(&channel)
            .unwrap_or_else(|| other_chat(&target));
        let previous = std::mem::replace(&mut state.chat, next);
        self.parked_chats.insert(previous.channel, previous);
        // A draft belongs to the channel it was typed in; the fixture drops it.
        state.draft.clear();
    }

    fn send_draft(&mut self, state: &mut ShellState) {
        let text = state.draft.trim();
        if text.is_empty() {
            return;
        }
        self.next_message += 1;
        state.chat.items.push(ChatItem::from(Message {
            id: MessageId(self.next_message),
            author: state.me.name.clone(),
            avatar: state.me.avatar.clone(),
            time: "now".to_owned(),
            text: text.to_owned(),
            reply_to: None,
            attachment: None,
            reactions: Vec::new(),
        }));
        state.draft.clear();
    }
}

fn toggle_menu(state: &mut ShellState, menu: VoiceBlockMenu) {
    state.open_menu = if state.open_menu == Some(menu) {
        None
    } else {
        Some(menu)
    };
}

fn own_activity(state: &ShellState) -> RoomActivity {
    if state.me.deafened {
        RoomActivity::Deafened
    } else if state.me.muted {
        RoomActivity::Muted
    } else {
        RoomActivity::Listening
    }
}

/// Copies the user's mute state to their entry in the room they are in.
fn sync_own_activity(state: &mut ShellState) {
    let activity = own_activity(state);
    let user = state.me.user;
    for member in state
        .voice_rooms
        .iter_mut()
        .flat_map(|room| &mut room.members)
    {
        if member.user == user {
            member.activity = activity;
        }
    }
}

fn set_own_status(state: &mut ShellState) {
    let status = match &state.me.room {
        Some(room) => format!("In {}", room.name),
        None => state.me.presence.label().to_owned(),
    };
    let (user, presence) = (state.me.user, state.me.presence);
    for member in state
        .members
        .groups
        .iter_mut()
        .flat_map(|group| &mut group.members)
    {
        if member.user == user {
            member.presence = presence;
            member.status.clone_from(&status);
        }
    }
}

fn set_presence(state: &mut ShellState, presence: Presence) {
    state.me.presence = presence;
    set_own_status(state);
}

fn remove_from_rooms(state: &mut ShellState) {
    let user = state.me.user;
    for room in &mut state.voice_rooms {
        room.members.retain(|member| member.user != user);
    }
}

fn join_room(state: &mut ShellState, room: RoomId) {
    if state
        .me
        .room
        .as_ref()
        .is_some_and(|current| current.id == room)
    {
        return;
    }
    let activity = own_activity(state);
    remove_from_rooms(state);
    let Some(target) = state.voice_rooms.iter_mut().find(|entry| entry.id == room) else {
        state.me.room = None;
        set_own_status(state);
        return;
    };
    target.members.insert(
        0,
        RoomMember {
            user: state.me.user,
            name: state.me.name.clone(),
            avatar: state.me.avatar.clone(),
            activity,
        },
    );
    state.me.room = Some(RoomRef {
        id: room,
        name: target.name.clone(),
    });
    set_own_status(state);
}

fn leave_room(state: &mut ShellState) {
    remove_from_rooms(state);
    state.me.room = None;
    // Leaving a room ends whatever was being shared into it.
    state.me.streams = Streams::default();
    if state.open_menu == Some(VoiceBlockMenu::Stream) {
        state.open_menu = None;
    }
    set_own_status(state);
}

fn toggle_reaction(state: &mut ShellState, message: MessageId, index: usize) {
    let reaction = state
        .chat
        .items
        .iter_mut()
        .filter_map(|item| match item {
            ChatItem::Message(entry) if entry.id == message => Some(entry),
            _ => None,
        })
        .find_map(|entry| entry.reactions.get_mut(index));
    if let Some(reaction) = reaction {
        if reaction.mine {
            reaction.count = reaction.count.saturating_sub(1);
        } else {
            reaction.count += 1;
        }
        reaction.mine = !reaction.mine;
    }
}

#[cfg(test)]
mod tests;
