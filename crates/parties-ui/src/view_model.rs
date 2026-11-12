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
        ChannelId, Chat, MemberList, PartyHeader, SelfState, ShellState, Switcher, TextChannel,
        VoiceBlockMenu, VoiceRoom,
    },
    theme::LightId,
};

/// Receives the commands of a [`ShellModel`]. It answers, now or later, by
/// calling [`ShellModel::apply`].
pub type CommandHandler = Arc<dyn Fn(&ShellModel, Command) + Send + Sync>;

struct Signals {
    light: Signal<LightId>,
    switcher: Signal<Switcher>,
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
            switcher: Signal::new(state.switcher),
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
            switcher: signals.switcher.get_untracked(),
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
        set_if_changed(&signals.switcher, state.switcher);
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

    pub fn switcher(&self) -> Switcher {
        self.inner.signals.switcher.get()
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
