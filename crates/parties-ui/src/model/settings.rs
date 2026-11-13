//! Settings: which page is open, and what the Voice and audio page, the Video
//! and sharing page and the App and notifications page edit. The light and the blur of the Appearance
//! page are fields of [`ShellState`](super::ShellState).

/// The pages of Settings, in the order of its navigation.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum SettingsPage {
    Profile,
    Keys,
    #[default]
    VoiceAndAudio,
    VideoAndSharing,
    Hotkeys,
    AppAndNotifications,
    Appearance,
}

/// The two groups of the navigation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SettingsGroup {
    You,
    App,
}

impl SettingsGroup {
    pub const ALL: [SettingsGroup; 2] = [Self::You, Self::App];

    pub const fn label(self) -> &'static str {
        match self {
            Self::You => "You",
            Self::App => "App",
        }
    }
}

impl SettingsPage {
    pub const ALL: [SettingsPage; 7] = [
        Self::Profile,
        Self::Keys,
        Self::VoiceAndAudio,
        Self::VideoAndSharing,
        Self::Hotkeys,
        Self::AppAndNotifications,
        Self::Appearance,
    ];

    /// The name in the navigation and the title of the page.
    pub const fn label(self) -> &'static str {
        match self {
            Self::Profile => "Profile",
            Self::Keys => "Keys",
            Self::VoiceAndAudio => "Voice and audio",
            Self::VideoAndSharing => "Video and sharing",
            Self::Hotkeys => "Hotkeys",
            Self::AppAndNotifications => "App and notifications",
            Self::Appearance => "Appearance",
        }
    }

    pub const fn group(self) -> SettingsGroup {
        match self {
            Self::Profile | Self::Keys => SettingsGroup::You,
            Self::VoiceAndAudio
            | Self::VideoAndSharing
            | Self::Hotkeys
            | Self::AppAndNotifications
            | Self::Appearance => SettingsGroup::App,
        }
    }

    /// A short name for element ids: "voice-and-audio".
    pub const fn slug(self) -> &'static str {
        match self {
            Self::Profile => "profile",
            Self::Keys => "keys",
            Self::VoiceAndAudio => "voice-and-audio",
            Self::VideoAndSharing => "video-and-sharing",
            Self::Hotkeys => "hotkeys",
            Self::AppAndNotifications => "app-and-notifications",
            Self::Appearance => "appearance",
        }
    }
}

/// What the Voice and audio page shows and edits.
#[derive(Clone, Debug, PartialEq)]
pub struct VoiceSettings {
    /// The name of the microphone in use.
    pub input_device: String,
    /// The name of the speakers or headphones in use.
    pub output_device: String,
    pub input_mode: InputMode,
    /// The microphone level right now: 0 is the bottom of the meter
    /// ([`VoiceSettings::METER_FLOOR_DB`]), 1 is 0 dB.
    pub input_level: f32,
    /// Anything quieter than this is not sent, in dB (the meter's floor to 0).
    pub sensitivity_db: i32,
    pub noise_suppression: bool,
    pub echo_cancellation: bool,
    pub automatic_gain: bool,
    /// Output volume in percent.
    pub output_volume: i32,
}

impl VoiceSettings {
    /// The level at the left end of the input meter.
    pub const METER_FLOOR_DB: i32 = -60;

    /// The microphone level in dB.
    pub fn input_level_db(&self) -> f32 {
        Self::METER_FLOOR_DB as f32 * (1.0 - self.input_level.clamp(0.0, 1.0))
    }

    /// Where the sensitivity sits on the meter, from 0 to 1.
    pub fn sensitivity_position(&self) -> f32 {
        let floor = Self::METER_FLOOR_DB as f32;
        ((self.sensitivity_db as f32 - floor) / -floor).clamp(0.0, 1.0)
    }

    /// The microphone is louder than the sensitivity: the voice is sent.
    pub fn voice_detected(&self) -> bool {
        self.input_level > 0.0 && self.input_level >= self.sensitivity_position()
    }
}

/// How the microphone is opened.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InputMode {
    /// Transmit when the user speaks.
    VoiceActivity,
    /// Transmit while a key is held.
    PushToTalk,
}

impl InputMode {
    pub const ALL: [InputMode; 2] = [Self::VoiceActivity, Self::PushToTalk];

    pub const fn label(self) -> &'static str {
        match self {
            Self::VoiceActivity => "Voice activity",
            Self::PushToTalk => "Push to talk",
        }
    }

    /// The line under the name.
    pub const fn hint(self) -> &'static str {
        match self {
            Self::VoiceActivity => "Transmits when you speak.",
            Self::PushToTalk => "Transmits while you hold a key.",
        }
    }
}

/// What the Video and sharing page shows and edits.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VideoSettings {
    /// The name of the camera in use, and its format, ready to show:
    /// "1280 \u{d7} 720 (720p)", "30 fps".
    pub camera: String,
    pub camera_resolution: String,
    pub camera_frame_rate: String,
    /// The preview on the page is running.
    pub camera_preview: bool,
    /// The codec a screen share asks for.
    pub codec: CodecChoice,
    /// The encoder found on this machine: "NVIDIA NVENC, hardware".
    pub encoder: String,
    /// The format a screen share starts with, ready to show.
    pub share_resolution: String,
    pub share_frame_rate: String,
    /// The average bitrate of a screen share, in Mbps.
    pub share_bitrate_mbps: i32,
    /// A share sends the sound of what is shared, unless turned off.
    pub share_audio: bool,
}

/// The codec a screen share asks for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CodecChoice {
    /// The best one everyone in the room can decode.
    Automatic,
    Av1,
    H265,
    H264,
}

impl CodecChoice {
    pub const ALL: [CodecChoice; 4] = [Self::Automatic, Self::Av1, Self::H265, Self::H264];

    pub const fn label(self) -> &'static str {
        match self {
            Self::Automatic => "Automatic",
            Self::Av1 => "AV1",
            Self::H265 => "H.265",
            Self::H264 => "H.264",
        }
    }

    /// The line under the choices.
    pub const fn hint(self) -> &'static str {
        match self {
            Self::Automatic => "Automatic picks the best one everyone can decode.",
            Self::Av1 | Self::H265 | Self::H264 => {
                "Used when everyone in the room can decode it; H.264 otherwise."
            }
        }
    }

    /// A short name for element ids.
    pub const fn slug(self) -> &'static str {
        match self {
            Self::Automatic => "automatic",
            Self::Av1 => "av1",
            Self::H265 => "h265",
            Self::H264 => "h264",
        }
    }
}

/// What the App and notifications page shows and edits.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AppSettings {
    /// The switches of the page that are on.
    pub switches: Vec<AppSwitch>,
    /// The name of the sound played with notifications.
    pub notification_sound: String,
    pub update_channel: UpdateChannel,
    /// The version that runs, and what is known about a newer one: "0.8.0",
    /// "up to date".
    pub version: String,
    pub update_state: String,
    /// When updates were last looked for, ready to show: "today at 14:02".
    pub last_checked: String,
    /// The name of the language of the interface.
    pub language: String,
    /// What a bug report should quote: "Parties 0.8.0 · protocol 2.0".
    pub build: String,
}

impl AppSettings {
    pub fn is_on(&self, switch: AppSwitch) -> bool {
        self.switches.contains(&switch)
    }

    pub fn set(&mut self, switch: AppSwitch, on: bool) {
        self.switches.retain(|entry| *entry != switch);
        if on {
            self.switches.push(switch);
        }
    }
}

/// The settings of the App and notifications page that are on or off.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum AppSwitch {
    NotifyMentions,
    NotifyRoomJoins,
    NotifyStreams,
    InstallUpdates,
    StartWithSystem,
    StartMinimised,
    CloseToTray,
    DiagnosticsOverlay,
}

impl AppSwitch {
    /// The name of the setting.
    pub const fn title(self) -> &'static str {
        match self {
            Self::NotifyMentions => "Mentions",
            Self::NotifyRoomJoins => "Someone joins your room",
            Self::NotifyStreams => "A stream started",
            Self::InstallUpdates => "Install automatically",
            Self::StartWithSystem => "Start with the system",
            Self::StartMinimised => "Start minimised",
            Self::CloseToTray => "Close to tray",
            Self::DiagnosticsOverlay => "Show overlay",
        }
    }

    /// The line under the name.
    pub const fn hint(self) -> &'static str {
        match self {
            Self::NotifyMentions => "When someone @mentions you.",
            Self::NotifyRoomJoins => "A short sound while you are in voice.",
            Self::NotifyStreams => "A screen share or camera goes live.",
            Self::InstallUpdates => "Applied the next time Parties starts.",
            Self::StartWithSystem => "Open Parties when you sign in.",
            Self::StartMinimised => "Open straight to the tray.",
            Self::CloseToTray => "Closing the window keeps you in voice.",
            Self::DiagnosticsOverlay => "Traffic, packet loss and freezes, live.",
        }
    }

    /// A short name for element ids: "start-with-system".
    pub const fn slug(self) -> &'static str {
        match self {
            Self::NotifyMentions => "notify-mentions",
            Self::NotifyRoomJoins => "notify-room-joins",
            Self::NotifyStreams => "notify-streams",
            Self::InstallUpdates => "install-updates",
            Self::StartWithSystem => "start-with-system",
            Self::StartMinimised => "start-minimised",
            Self::CloseToTray => "close-to-tray",
            Self::DiagnosticsOverlay => "diagnostics-overlay",
        }
    }
}

/// Which releases the client updates to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UpdateChannel {
    Stable,
    /// Gets features earlier.
    Beta,
}

impl UpdateChannel {
    pub const ALL: [UpdateChannel; 2] = [Self::Stable, Self::Beta];

    pub const fn label(self) -> &'static str {
        match self {
            Self::Stable => "Stable",
            Self::Beta => "Beta",
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_app_switch_is_on_once_or_not_at_all() {
        let mut app = AppSettings {
            switches: vec![AppSwitch::CloseToTray],
            notification_sound: String::new(),
            update_channel: UpdateChannel::Stable,
            version: String::new(),
            update_state: String::new(),
            last_checked: String::new(),
            language: String::new(),
            build: String::new(),
        };
        assert!(app.is_on(AppSwitch::CloseToTray));
        app.set(AppSwitch::CloseToTray, true);
        app.set(AppSwitch::StartMinimised, true);
        assert_eq!(
            app.switches,
            [AppSwitch::CloseToTray, AppSwitch::StartMinimised]
        );
        app.set(AppSwitch::CloseToTray, false);
        assert!(!app.is_on(AppSwitch::CloseToTray));
        assert!(app.is_on(AppSwitch::StartMinimised));
    }

    #[test]
    fn the_voice_is_detected_above_the_sensitivity() {
        let mut voice = VoiceSettings {
            input_device: String::new(),
            output_device: String::new(),
            input_mode: InputMode::VoiceActivity,
            input_level: 0.6,
            sensitivity_db: -36,
            noise_suppression: true,
            echo_cancellation: true,
            automatic_gain: false,
            output_volume: 80,
        };
        assert!((voice.input_level_db() + 24.0).abs() < 1e-4);
        assert_eq!(voice.sensitivity_position(), 0.4);
        assert!(voice.voice_detected());

        voice.sensitivity_db = -12;
        assert!(!voice.voice_detected());
        // Silence is never a voice, wherever the sensitivity is.
        voice.sensitivity_db = VoiceSettings::METER_FLOOR_DB;
        voice.input_level = 0.0;
        assert!(!voice.voice_detected());
    }

    #[test]
    fn every_page_is_listed_once_and_grouped() {
        let mut slugs: Vec<_> = SettingsPage::ALL.iter().map(|page| page.slug()).collect();
        slugs.sort_unstable();
        slugs.dedup();
        assert_eq!(slugs.len(), SettingsPage::ALL.len());

        let you = SettingsPage::ALL
            .iter()
            .filter(|page| page.group() == SettingsGroup::You)
            .count();
        assert_eq!(you, 2);
    }
}
