//! Hotkeys: the shortcuts of the things the user does mid-conversation.

/// What the Hotkeys page shows.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Hotkeys {
    /// One entry per action, in the order of the page.
    pub shortcuts: Vec<Shortcut>,
    /// The action whose shortcut is being captured: the next keys the user
    /// presses become it.
    pub capturing: Option<HotkeyAction>,
}

impl Hotkeys {
    pub fn shortcut(&self, action: HotkeyAction) -> Option<&Shortcut> {
        self.shortcuts.iter().find(|entry| entry.action == action)
    }

    /// Gives `action` the shortcut `keys`, or none. It ends a capture of
    /// that action; whether another application holds the keys is not known
    /// until the core reports it.
    pub fn set(&mut self, action: HotkeyAction, keys: Option<String>) {
        if let Some(entry) = self
            .shortcuts
            .iter_mut()
            .find(|entry| entry.action == action)
        {
            entry.keys = keys;
            entry.conflict = false;
        }
        if self.capturing == Some(action) {
            self.capturing = None;
        }
    }
}

/// The shortcut of one action.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Shortcut {
    pub action: HotkeyAction,
    /// The keys, ready to show ("Ctrl + Shift + M"); `None` is not set.
    pub keys: Option<String>,
    /// Another application has registered the same keys, so the shortcut may
    /// not reach the client while its window is in the background.
    pub conflict: bool,
}

/// What a shortcut does.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum HotkeyAction {
    PushToTalk,
    ToggleMute,
    ToggleDeafen,
    ToggleScreenShare,
    ToggleCamera,
    DiagnosticsOverlay,
    PushToMute,
}

impl HotkeyAction {
    pub const ALL: [HotkeyAction; 7] = [
        Self::PushToTalk,
        Self::ToggleMute,
        Self::ToggleDeafen,
        Self::ToggleScreenShare,
        Self::ToggleCamera,
        Self::DiagnosticsOverlay,
        Self::PushToMute,
    ];

    pub const fn title(self) -> &'static str {
        match self {
            Self::PushToTalk => "Push to talk",
            Self::ToggleMute => "Toggle mute",
            Self::ToggleDeafen => "Toggle deafen",
            Self::ToggleScreenShare => "Start or stop screen share",
            Self::ToggleCamera => "Toggle camera",
            Self::DiagnosticsOverlay => "Show diagnostics overlay",
            Self::PushToMute => "Push to mute",
        }
    }

    /// The line under the name.
    pub const fn hint(self) -> &'static str {
        match self {
            Self::PushToTalk => "Hold to talk when the input mode is push to talk.",
            Self::ToggleMute => "Mute or restore your microphone.",
            Self::ToggleDeafen => "Silence everything you hear, and your microphone.",
            Self::ToggleScreenShare => "Opens the share picker, or ends your share.",
            Self::ToggleCamera => "Turn your camera on or off.",
            Self::DiagnosticsOverlay => "Traffic, packet loss and freezes on top of Parties.",
            Self::PushToMute => "Hold to mute when the input mode is voice activity.",
        }
    }

    /// A short name for element ids: "toggle-mute".
    pub const fn slug(self) -> &'static str {
        match self {
            Self::PushToTalk => "push-to-talk",
            Self::ToggleMute => "toggle-mute",
            Self::ToggleDeafen => "toggle-deafen",
            Self::ToggleScreenShare => "toggle-screen-share",
            Self::ToggleCamera => "toggle-camera",
            Self::DiagnosticsOverlay => "diagnostics-overlay",
            Self::PushToMute => "push-to-mute",
        }
    }

    /// The shortcut a new installation starts with.
    pub const fn default_keys(self) -> Option<&'static str> {
        match self {
            Self::PushToTalk => Some("Mouse 4"),
            Self::ToggleMute => Some("Ctrl + Shift + M"),
            Self::ToggleDeafen => Some("Ctrl + Shift + D"),
            Self::ToggleScreenShare => Some("Ctrl + Shift + S"),
            Self::DiagnosticsOverlay => Some("Ctrl + Shift + F3"),
            Self::ToggleCamera | Self::PushToMute => None,
        }
    }
}

impl Hotkeys {
    /// The shortcuts of a new installation.
    pub fn defaults() -> Self {
        Self {
            shortcuts: HotkeyAction::ALL
                .into_iter()
                .map(|action| Shortcut {
                    action,
                    keys: action.default_keys().map(str::to_owned),
                    conflict: false,
                })
                .collect(),
            capturing: None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_defaults_have_every_action_once() {
        let hotkeys = Hotkeys::defaults();
        assert_eq!(hotkeys.shortcuts.len(), HotkeyAction::ALL.len());
        for action in HotkeyAction::ALL {
            let shortcut = hotkeys.shortcut(action).expect("every action has an entry");
            assert_eq!(shortcut.keys.as_deref(), action.default_keys());
            assert!(!shortcut.conflict);
        }
    }

    #[test]
    fn setting_a_shortcut_ends_its_capture_and_its_conflict() {
        let mut hotkeys = Hotkeys::defaults();
        hotkeys.shortcuts[1].conflict = true;
        hotkeys.capturing = Some(HotkeyAction::ToggleMute);

        hotkeys.set(HotkeyAction::ToggleMute, Some("F9".to_owned()));

        let shortcut = hotkeys.shortcut(HotkeyAction::ToggleMute).unwrap();
        assert_eq!(shortcut.keys.as_deref(), Some("F9"));
        assert!(!shortcut.conflict);
        assert_eq!(hotkeys.capturing, None);

        // Setting another action leaves a running capture alone.
        hotkeys.capturing = Some(HotkeyAction::ToggleCamera);
        hotkeys.set(HotkeyAction::PushToMute, None);
        assert_eq!(hotkeys.capturing, Some(HotkeyAction::ToggleCamera));
    }
}
