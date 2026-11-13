//! Icons: the Phosphor set, taken as SVG source from the `phosphor-svgs`
//! crate (MIT) and drawn through lurq's `svg` feature.
//!
//! Components name an [`Icon`]; only this file knows which Phosphor glyph it
//! is. Every Phosphor file paints with `currentColor`, so the colour is set
//! with an SVG fill override.

use lurq::{components::Svg, node::color::Color, svg::SvgData};
use phosphor_svgs::style::{fill, regular};

/// Every icon the shell draws.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Icon {
    // Brand and switcher.
    Brand,
    Home,
    Settings,
    PartyMoon,
    PartyFilm,
    PartyShield,
    PartyServer,
    Plus,
    CaretDown,
    CaretUp,
    // Window buttons.
    Minimise,
    Maximise,
    Restore,
    Close,
    // Left panel.
    Lock,
    More,
    Speaker,
    SpeakerOff,
    Speaking,
    Sharing,
    Mic,
    MicOff,
    Hash,
    // Voice block.
    Signal,
    Headphones,
    Leave,
    Stream,
    Screen,
    Camera,
    Music,
    Check,
    DoNotDisturb,
    // Chat.
    Pin,
    Search,
    Members,
    Image,
    Reply,
    FileCode,
    Download,
    ThumbsUp,
    Fire,
    Ticket,
    Attach,
    Emoji,
    Send,
    // Home and the update notice.
    ArrowRight,
    Retry,
    Copy,
    Key,
    Update,
    // Settings.
    Profile,
    Keyboard,
    Bell,
    Palette,
    Selected,
    Play,
    Globe,
    Folder,
    Refresh,
    CameraOff,
    Chip,
    Reset,
    Warning,
    Edit,
    Fingerprint,
    Info,
}

impl Icon {
    /// The SVG source of the icon.
    pub const fn svg(self) -> &'static str {
        match self {
            Self::Brand => fill::CONFETTI,
            Self::Home => regular::HOUSE,
            Self::Settings => regular::GEAR_SIX,
            Self::PartyMoon => fill::MOON_STARS,
            Self::PartyFilm => regular::FILM_SLATE,
            Self::PartyShield => regular::SHIELD,
            Self::PartyServer => regular::HARD_DRIVES,
            Self::Plus => regular::PLUS,
            Self::CaretDown => regular::CARET_DOWN,
            Self::CaretUp => regular::CARET_UP,
            Self::Minimise => regular::MINUS,
            Self::Maximise => regular::SQUARE,
            Self::Restore => regular::COPY,
            Self::Close => regular::X,
            Self::Lock => regular::LOCK_SIMPLE,
            Self::More => regular::DOTS_THREE,
            Self::Speaker => regular::SPEAKER_HIGH,
            Self::SpeakerOff => regular::SPEAKER_SLASH,
            Self::Speaking => regular::WAVE_SINE,
            Self::Sharing => regular::SCREENCAST,
            Self::Mic => regular::MICROPHONE,
            Self::MicOff => regular::MICROPHONE_SLASH,
            Self::Hash => regular::HASH,
            Self::Signal => regular::CELL_SIGNAL_HIGH,
            Self::Headphones => regular::HEADPHONES,
            Self::Leave => regular::PHONE_DISCONNECT,
            Self::Stream => regular::BROADCAST,
            Self::Screen => regular::MONITOR,
            Self::Camera => regular::VIDEO_CAMERA,
            Self::Music => regular::MUSIC_NOTES,
            Self::Check => regular::CHECK,
            Self::DoNotDisturb => fill::MINUS_CIRCLE,
            Self::Pin => regular::PUSH_PIN,
            Self::Search => regular::MAGNIFYING_GLASS,
            Self::Members => regular::USERS,
            Self::Image => regular::IMAGE,
            Self::Reply => regular::ARROW_BEND_UP_LEFT,
            Self::FileCode => regular::BRACKETS_CURLY,
            Self::Download => regular::DOWNLOAD_SIMPLE,
            Self::ThumbsUp => regular::THUMBS_UP,
            Self::Fire => regular::FIRE,
            Self::Ticket => regular::TICKET,
            Self::Attach => regular::PAPERCLIP,
            Self::Emoji => regular::SMILEY,
            Self::Send => fill::PAPER_PLANE_RIGHT,
            Self::ArrowRight => regular::ARROW_RIGHT,
            Self::Retry => regular::ARROW_CLOCKWISE,
            Self::Copy => regular::COPY,
            Self::Key => regular::KEY,
            Self::Update => regular::ARROW_CIRCLE_UP,
            Self::Profile => regular::USER_CIRCLE,
            Self::Keyboard => regular::KEYBOARD,
            Self::Bell => regular::BELL,
            Self::Palette => regular::PALETTE,
            Self::Selected => fill::CHECK_CIRCLE,
            Self::Play => regular::PLAY,
            Self::Globe => regular::GLOBE,
            Self::Folder => regular::FOLDER_OPEN,
            Self::Refresh => regular::ARROWS_CLOCKWISE,
            Self::CameraOff => regular::VIDEO_CAMERA_SLASH,
            Self::Chip => regular::CPU,
            Self::Reset => regular::ARROW_COUNTER_CLOCKWISE,
            Self::Warning => regular::WARNING,
            Self::Edit => regular::PENCIL_SIMPLE,
            Self::Fingerprint => regular::FINGERPRINT,
            Self::Info => regular::INFO,
        }
    }
}

/// `icon` drawn `size` logical pixels square in `color`.
pub fn icon(icon: Icon, size: f32, color: Color) -> Svg {
    Svg::new(SvgData::from_str(icon.svg()).with_fill(color)).size(size, size)
}
