//! The view model of the main window: the [`ShellState`] split into reactive
//! signals, plus the one way out for user intent, [`ShellModel::dispatch`].
//!
//! A view reads a slice with one of the getters during `render`; lurq then
//! re-renders that component, and only it, when the slice changes. Whoever
//! owns the state pushes a new one with [`ShellModel::apply`], from any
//! thread; slices that did not change notify nobody.

use std::sync::{Arc, Mutex, PoisonError};

use lurq::{app::events::MouseEvent, core::Signal};

use crate::{
    commands::Command,
    fixtures::FixtureCore,
    model::{
        AppSettings, ChannelId, Chat, Home, Hotkeys, IdentityCard, Keys, MemberList, PartyCard,
        PartyHeader, Profile, SelfState, SettingsPage, ShellState, Switcher, TextChannel,
        UpdateNotice, VideoSettings, VoiceBlockMenu, VoiceRoom, VoiceSettings,
    },
    theme::LightId,
};

/// Receives the commands of a [`ShellModel`]. It answers, now or later, by
/// calling [`ShellModel::apply`].
pub type CommandHandler = Arc<dyn Fn(&ShellModel, Command) + Send + Sync>;

struct Signals {
    light: Signal<LightId>,
    backdrop_blur: Signal<bool>,
    switcher: Signal<Switcher>,
    update: Signal<Option<UpdateNotice>>,
    home_parties: Signal<Vec<PartyCard>>,
    identity: Signal<IdentityCard>,
    join_code: Signal<String>,
    settings_page: Signal<SettingsPage>,
    /// The voice settings; its two slider values are stale here, the sliders
    /// own them (`sensitivity`, `output_volume`).
    voice: Signal<VoiceSettings>,
    sensitivity: Signal<i32>,
    output_volume: Signal<i32>,
    /// The video settings; the bitrate is stale here, its slider owns it.
    video: Signal<VideoSettings>,
    share_bitrate: Signal<i32>,
    /// The profile; its display name is stale here, the text input owns it.
    profile: Signal<Profile>,
    display_name: Signal<String>,
    keys: Signal<Keys>,
    hotkeys: Signal<Hotkeys>,
    app: Signal<AppSettings>,
    party: Signal<PartyHeader>,
    voice_rooms: Signal<Vec<VoiceRoom>>,
    text_channels: Signal<Vec<TextChannel>>,
    selected_channel: Signal<ChannelId>,
    chat: Signal<Chat>,
    draft: Signal<String>,
    members: Signal<MemberList>,
    member_search: Signal<String>,
    members_panel_open: Signal<bool>,
    me: Signal<SelfState>,
    stream_menu_open: Signal<bool>,
    presence_menu_open: Signal<bool>,
}

struct Inner {
    signals: Signals,
    handler: CommandHandler,
}

/// A cheap handle to the window's state; clones share it.
#[derive(Clone)]
pub struct ShellModel {
    inner: Arc<Inner>,
}

impl PartialEq for ShellModel {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.inner, &other.inner)
    }
}

impl std::fmt::Debug for ShellModel {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ShellModel").finish_non_exhaustive()
    }
}

/// Writes `value` to `signal` only when it differs, so an unchanged slice
/// re-renders nothing.
fn set_if_changed<T: Clone + PartialEq + Send + Sync + 'static>(signal: &Signal<T>, value: T) {
    if signal.with_untracked(|current| *current != value) {
        signal.set(value);
    }
}

impl ShellModel {
    /// A model that answers commands itself, with the in-memory
    /// [`FixtureCore`]. For tests, screenshots and the example.
    pub fn new(state: ShellState) -> Self {
        let core = Mutex::new(FixtureCore::new());
        Self::with_handler(
            state,
            Arc::new(move |model: &ShellModel, command| {
                let mut state = model.snapshot();
                // A panic in an earlier command cannot leave the fixture core
                // in a state worse than "one command lost", so a poisoned
                // lock is still usable.
                core.lock()
                    .unwrap_or_else(PoisonError::into_inner)
                    .handle(&mut state, command);
                model.apply(state);
            }),
        )
    }

    /// A model whose commands go to `handler`: the seam for the client core.
    pub fn with_handler(state: ShellState, handler: CommandHandler) -> Self {
        let signals = Signals {
            light: Signal::new(state.light),
            backdrop_blur: Signal::new(state.backdrop_blur),
            switcher: Signal::new(state.switcher),
            update: Signal::new(state.update),
            home_parties: Signal::new(state.home.parties),
            identity: Signal::new(state.home.identity),
            join_code: Signal::new(state.home.join_code),
            settings_page: Signal::new(state.settings_page),
            sensitivity: Signal::new(state.voice.sensitivity_db),
            output_volume: Signal::new(state.voice.output_volume),
            voice: Signal::new(state.voice),
            share_bitrate: Signal::new(state.video.share_bitrate_mbps),
            video: Signal::new(state.video),
            display_name: Signal::new(state.profile.display_name.clone()),
            profile: Signal::new(state.profile),
            keys: Signal::new(state.keys),
            hotkeys: Signal::new(state.hotkeys),
            app: Signal::new(state.app),
            party: Signal::new(state.party),
            voice_rooms: Signal::new(state.voice_rooms),
            text_channels: Signal::new(state.text_channels),
            selected_channel: Signal::new(state.selected_channel),
            chat: Signal::new(state.chat),
            draft: Signal::new(state.draft),
            members: Signal::new(state.members),
            member_search: Signal::new(state.member_search),
            members_panel_open: Signal::new(state.members_panel_open),
            me: Signal::new(state.me),
            stream_menu_open: Signal::new(state.open_menu == Some(VoiceBlockMenu::Stream)),
            presence_menu_open: Signal::new(state.open_menu == Some(VoiceBlockMenu::Presence)),
        };
        Self {
            inner: Arc::new(Inner { signals, handler }),
        }
    }

    /// Sends what the user asked for to the owner of the state.
    pub fn dispatch(&self, command: Command) {
        (self.inner.handler)(self, command);
    }

    /// A click handler for a control:
    /// `on_click(model.command(Command::ToggleMute))`. The click stops at the
    /// control, so a clickable row around it does not act as well.
    pub fn command(&self, command: Command) -> impl Fn(MouseEvent) + Send + Sync + 'static {
        let model = self.clone();
        move |event: MouseEvent| {
            event.stop_propagation();
            model.dispatch(command.clone());
        }
    }

    /// The whole state as it is now. Does not subscribe the caller.
    pub fn snapshot(&self) -> ShellState {
        let signals = &self.inner.signals;
        ShellState {
            light: signals.light.get_untracked(),
            backdrop_blur: signals.backdrop_blur.get_untracked(),
            switcher: signals.switcher.get_untracked(),
            update: signals.update.get_untracked(),
            home: Home {
                parties: signals.home_parties.get_untracked(),
                identity: signals.identity.get_untracked(),
                join_code: signals.join_code.get_untracked(),
            },
            settings_page: signals.settings_page.get_untracked(),
            voice: VoiceSettings {
                sensitivity_db: signals.sensitivity.get_untracked(),
                output_volume: signals.output_volume.get_untracked(),
                ..signals.voice.get_untracked()
            },
            video: VideoSettings {
                share_bitrate_mbps: signals.share_bitrate.get_untracked(),
                ..signals.video.get_untracked()
            },
            profile: Profile {
                display_name: signals.display_name.get_untracked(),
                ..signals.profile.get_untracked()
            },
            keys: signals.keys.get_untracked(),
            hotkeys: signals.hotkeys.get_untracked(),
            app: signals.app.get_untracked(),
            party: signals.party.get_untracked(),
            voice_rooms: signals.voice_rooms.get_untracked(),
            text_channels: signals.text_channels.get_untracked(),
            selected_channel: signals.selected_channel.get_untracked(),
            chat: signals.chat.get_untracked(),
            draft: signals.draft.get_untracked(),
            members: signals.members.get_untracked(),
            member_search: signals.member_search.get_untracked(),
            members_panel_open: signals.members_panel_open.get_untracked(),
            me: signals.me.get_untracked(),
            open_menu: if signals.stream_menu_open.get_untracked() {
                Some(VoiceBlockMenu::Stream)
            } else if signals.presence_menu_open.get_untracked() {
                Some(VoiceBlockMenu::Presence)
            } else {
                None
            },
        }
    }

    /// Replaces the state. Only the slices that changed notify their views.
    pub fn apply(&self, state: ShellState) {
        let signals = &self.inner.signals;
        set_if_changed(&signals.light, state.light);
        set_if_changed(&signals.backdrop_blur, state.backdrop_blur);
        set_if_changed(&signals.switcher, state.switcher);
        set_if_changed(&signals.update, state.update);
        set_if_changed(&signals.home_parties, state.home.parties);
        set_if_changed(&signals.identity, state.home.identity);
        set_if_changed(&signals.join_code, state.home.join_code);
        set_if_changed(&signals.settings_page, state.settings_page);
        set_if_changed(&signals.sensitivity, state.voice.sensitivity_db);
        set_if_changed(&signals.output_volume, state.voice.output_volume);
        set_if_changed(&signals.voice, state.voice);
        set_if_changed(&signals.share_bitrate, state.video.share_bitrate_mbps);
        set_if_changed(&signals.video, state.video);
        set_if_changed(&signals.display_name, state.profile.display_name.clone());
        set_if_changed(&signals.profile, state.profile);
        set_if_changed(&signals.keys, state.keys);
        set_if_changed(&signals.hotkeys, state.hotkeys);
        set_if_changed(&signals.app, state.app);
        set_if_changed(&signals.party, state.party);
        set_if_changed(&signals.voice_rooms, state.voice_rooms);
        set_if_changed(&signals.text_channels, state.text_channels);
        set_if_changed(&signals.selected_channel, state.selected_channel);
        set_if_changed(&signals.chat, state.chat);
        set_if_changed(&signals.draft, state.draft);
        set_if_changed(&signals.members, state.members);
        set_if_changed(&signals.member_search, state.member_search);
        set_if_changed(&signals.members_panel_open, state.members_panel_open);
        set_if_changed(&signals.me, state.me);
        // Close before opening, so the two menus are never open together,
        // not even between two signal writes.
        let stream = state.open_menu == Some(VoiceBlockMenu::Stream);
        let presence = state.open_menu == Some(VoiceBlockMenu::Presence);
        if !stream {
            set_if_changed(&signals.stream_menu_open, false);
        }
        if !presence {
            set_if_changed(&signals.presence_menu_open, false);
        }
        set_if_changed(&signals.stream_menu_open, stream);
        set_if_changed(&signals.presence_menu_open, presence);
    }

    // --- Slices. Reading one during `render` subscribes the component. ---

    pub fn light(&self) -> LightId {
        self.inner.signals.light.get()
    }

    pub fn backdrop_blur(&self) -> bool {
        self.inner.signals.backdrop_blur.get()
    }

    pub fn switcher(&self) -> Switcher {
        self.inner.signals.switcher.get()
    }

    /// The downloaded update the title bar offers, if any.
    pub fn update(&self) -> Option<UpdateNotice> {
        self.inner.signals.update.get()
    }

    /// The rows of "Your parties" on Home.
    pub fn home_parties(&self) -> Vec<PartyCard> {
        self.inner.signals.home_parties.get()
    }

    pub fn identity(&self) -> IdentityCard {
        self.inner.signals.identity.get()
    }

    /// The page Settings is on.
    pub fn settings_page(&self) -> SettingsPage {
        self.inner.signals.settings_page.get()
    }

    /// The voice settings, with the values the sliders hold right now.
    pub fn voice(&self) -> VoiceSettings {
        let signals = &self.inner.signals;
        VoiceSettings {
            sensitivity_db: signals.sensitivity.get(),
            output_volume: signals.output_volume.get(),
            ..signals.voice.get()
        }
    }

    pub fn party(&self) -> PartyHeader {
        self.inner.signals.party.get()
    }

    pub fn voice_rooms(&self) -> Vec<VoiceRoom> {
        self.inner.signals.voice_rooms.get()
    }

    pub fn text_channels(&self) -> Vec<TextChannel> {
        self.inner.signals.text_channels.get()
    }

    pub fn selected_channel(&self) -> ChannelId {
        self.inner.signals.selected_channel.get()
    }

    pub fn chat(&self) -> Chat {
        self.inner.signals.chat.get()
    }

    pub fn members(&self) -> MemberList {
        self.inner.signals.members.get()
    }

    pub fn member_search(&self) -> String {
        self.inner.signals.member_search.get()
    }

    pub fn members_panel_open(&self) -> bool {
        self.inner.signals.members_panel_open.get()
    }

    pub fn me(&self) -> SelfState {
        self.inner.signals.me.get()
    }

    pub fn open_menu(&self) -> Option<VoiceBlockMenu> {
        let signals = &self.inner.signals;
        if signals.stream_menu_open.get() {
            Some(VoiceBlockMenu::Stream)
        } else if signals.presence_menu_open.get() {
            Some(VoiceBlockMenu::Presence)
        } else {
            None
        }
    }

    // --- Signals the toolkit writes itself. ---

    /// The composer's text. The text input edits it in place.
    pub fn draft_signal(&self) -> Signal<String> {
        self.inner.signals.draft.clone()
    }

    /// The input sensitivity in dB. The slider edits it in place.
    pub fn sensitivity_signal(&self) -> Signal<i32> {
        self.inner.signals.sensitivity.clone()
    }

    /// The output volume in percent. The slider edits it in place.
    pub fn output_volume_signal(&self) -> Signal<i32> {
        self.inner.signals.output_volume.clone()
    }

    /// The video settings, with the bitrate its slider holds right now.
    pub fn video(&self) -> VideoSettings {
        let signals = &self.inner.signals;
        VideoSettings {
            share_bitrate_mbps: signals.share_bitrate.get(),
            ..signals.video.get()
        }
    }

    /// The bitrate of a screen share in Mbps. The slider edits it in place.
    pub fn share_bitrate_signal(&self) -> Signal<i32> {
        self.inner.signals.share_bitrate.clone()
    }

    /// The profile, with the display name its field holds right now.
    pub fn profile(&self) -> Profile {
        let signals = &self.inner.signals;
        Profile {
            display_name: signals.display_name.get(),
            ..signals.profile.get()
        }
    }

    /// The display name field. The text input edits it in place.
    pub fn display_name_signal(&self) -> Signal<String> {
        self.inner.signals.display_name.clone()
    }

    /// What the Keys page shows.
    pub fn keys(&self) -> Keys {
        self.inner.signals.keys.get()
    }

    /// What the Hotkeys page shows.
    pub fn hotkeys(&self) -> Hotkeys {
        self.inner.signals.hotkeys.get()
    }

    /// What the App and notifications page shows.
    pub fn app(&self) -> AppSettings {
        self.inner.signals.app.get()
    }

    /// The join code on Home. The text input edits it in place.
    pub fn join_code_signal(&self) -> Signal<String> {
        self.inner.signals.join_code.clone()
    }

    /// The member search text. The text input edits it in place.
    pub fn member_search_signal(&self) -> Signal<String> {
        self.inner.signals.member_search.clone()
    }

    /// Whether the Stream menu is open. lurq closes a popup by writing
    /// `false` here when the user presses outside it or presses Escape.
    pub fn stream_menu_signal(&self) -> Signal<bool> {
        self.inner.signals.stream_menu_open.clone()
    }

    /// Whether the presence menu is open; see [`Self::stream_menu_signal`].
    pub fn presence_menu_signal(&self) -> Signal<bool> {
        self.inner.signals.presence_menu_open.clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        fixtures::{CHANNEL_GENERAL, Pictures, glass_shell},
        model::Presence,
    };

    fn model() -> ShellModel {
        ShellModel::new(glass_shell(&Pictures::none()))
    }

    #[test]
    fn snapshot_round_trips_the_state() {
        let state = glass_shell(&Pictures::none());
        assert_eq!(ShellModel::new(state.clone()).snapshot(), state);
    }

    #[test]
    fn dispatch_reaches_the_slices() {
        let model = model();
        model.dispatch(Command::ToggleMute);
        assert!(model.snapshot().me.muted);
        model.dispatch(Command::SelectChannel(CHANNEL_GENERAL));
        assert_eq!(model.snapshot().selected_channel, CHANNEL_GENERAL);
        model.dispatch(Command::SetPresence(Presence::DoNotDisturb));
        assert_eq!(model.snapshot().me.presence, Presence::DoNotDisturb);
    }

    #[test]
    fn opening_one_menu_closes_the_other() {
        let model = model();
        model.dispatch(Command::ToggleStreamMenu);
        assert!(model.stream_menu_signal().get_untracked());
        model.dispatch(Command::TogglePresenceMenu);
        assert!(!model.stream_menu_signal().get_untracked());
        assert!(model.presence_menu_signal().get_untracked());
        // The toolkit closes a popup by writing the signal.
        model.presence_menu_signal().set(false);
        assert_eq!(model.snapshot().open_menu, None);
    }

    #[test]
    fn a_custom_handler_receives_commands() {
        let seen = Arc::new(Mutex::new(Vec::new()));
        let log = seen.clone();
        let model = ShellModel::with_handler(
            glass_shell(&Pictures::none()),
            Arc::new(move |_, command| log.lock().unwrap().push(command)),
        );
        model.dispatch(Command::LeaveRoom);
        assert_eq!(*seen.lock().unwrap(), vec![Command::LeaveRoom]);
        assert!(
            model.snapshot().me.room.is_some(),
            "the handler decides what changes"
        );
    }
}
