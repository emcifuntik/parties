//! What the user can ask for from the main window.
//!
//! A control never changes state itself: it sends a [`Command`] through
//! [`ShellModel::dispatch`](crate::view_model::ShellModel::dispatch). Whoever
//! owns the state (the client core later, the fixture core today) answers by
//! applying a new state to the view model.

use crate::{
    model::{ChannelId, MessageId, Presence, RoomId, StreamSource, View},
    theme::LightId,
};

#[derive(Clone, Debug, PartialEq)]
pub enum Command {
    /// Show Home, Settings or a party (the switcher in the title bar).
    ShowView(View),
    /// Open a text channel in the chat panel.
    SelectChannel(ChannelId),
    /// Join a voice room, leaving the current one.
    JoinRoom(RoomId),
    /// Leave the current voice room.
    LeaveRoom,
    ToggleMute,
    ToggleDeafen,
    /// Open the Stream menu, or close it when it is open.
    ToggleStreamMenu,
    /// Open the presence menu, or close it when it is open.
    TogglePresenceMenu,
    /// Close whichever menu is open above the voice block.
    CloseMenus,
    /// Start or stop sharing one source.
    ToggleStream(StreamSource),
    SetPresence(Presence),
    /// The "Change picture" row of the presence menu.
    ChangePicture,
    SetLight(LightId),
    /// Switch to the next light of the table.
    CycleLight,
    /// Add or remove the user's own reaction; `index` is the reaction's
    /// position under the message.
    ToggleReaction {
        message: MessageId,
        index: usize,
    },
    /// Show or hide the members panel.
    ToggleMembersPanel,
    /// Send the composer's draft to the open channel.
    SendDraft,
}
