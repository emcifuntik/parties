//! What the user can ask for from the main window.
//!
//! A control never changes state itself: it sends a [`Command`] through
//! [`ShellModel::dispatch`](crate::view_model::ShellModel::dispatch). Whoever
//! owns the state (the client core later, the fixture core today) answers by
//! applying a new state to the view model.

use crate::{
    model::{
        AppSwitch, ChannelId, CodecChoice, HotkeyAction, InputMode, MessageId, PartyId, Presence,
        RoomId, SettingsPage, StreamSource, UpdateChannel, View,
    },
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
    /// Blur what is behind the glass, or draw solid panels ("Reduce
    /// transparency" on the Appearance page is the opposite of this).
    SetBackdropBlur(bool),
    /// Open a page of Settings.
    ShowSettingsPage(SettingsPage),
    /// Go back to having no picture.
    RemovePicture,
    /// Take the text of the display name field as the user's name.
    CommitDisplayName,
    /// Change the name the user goes by in one party.
    EditPartyName(PartyId),
    /// Remove one of the user's keys, by its id.
    RemoveKey(u32),
    AddPasskey,
    AddHardwareKey,
    ShowRecoveryPhrase,
    /// On this device only a hardware key signs in.
    SetAlwaysAskHardwareKey(bool),
    SetInputMode(InputMode),
    SetNoiseSuppression(bool),
    SetEchoCancellation(bool),
    SetAutomaticGain(bool),
    /// Play the microphone back to the user for a moment.
    TestMicrophone,
    /// Play a sound on the output device.
    TestOutput,
    /// Start or stop the camera preview on the Video and sharing page.
    SetCameraPreview(bool),
    /// The codec a screen share asks for.
    SetPreferredCodec(CodecChoice),
    /// Whether a screen share sends the sound of what is shared.
    SetShareAudio(bool),
    /// Start capturing a new shortcut for an action: the next keys the user
    /// presses become it.
    CaptureHotkey(HotkeyAction),
    /// Stop capturing without changing the shortcut.
    CancelHotkeyCapture,
    /// Give an action a shortcut (keys ready to show), or take it away.
    SetHotkey(HotkeyAction, Option<String>),
    /// Put every shortcut back to what a new installation has.
    ResetHotkeys,
    /// Turn a setting of the App and notifications page on or off.
    SetAppSwitch(AppSwitch, bool),
    SetUpdateChannel(UpdateChannel),
    /// Look for a newer version now.
    CheckForUpdates,
    /// Show what changed in the version that runs.
    ShowWhatsNew,
    /// Open the folder with the log files in the file manager.
    OpenLogsFolder,
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
    /// Try again to reach a party that is offline.
    RetryParty(PartyId),
    /// Join a party with the code in Home's join code field.
    JoinWithCode,
    /// Start adding a party by its address.
    AddPartyByAddress,
    /// Copy the user's key fingerprint to the clipboard.
    CopyFingerprint,
    /// Restart into the downloaded update.
    RestartToUpdate,
}
