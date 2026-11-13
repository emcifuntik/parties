//! Avatars: a round picture, or a monogram when the person has none, with
//! an optional presence dot and an optional speaking ring.

use lurq::{
    components::{Image, Rect, Row, Stack},
    images::{ImageKey, ImageSource},
    layout::{Alignment, layout_kind::Justify},
    node::{Gradient, color::Color},
};

use crate::{
    model::{Avatar, Presence},
    theme::{TextRole, Tokens, typography::text},
};

/// Diameter of the presence dot, ring included.
const DOT: f32 = 13.0;
/// The same on a large picture (the identity card on Home).
const DOT_LARGE: f32 = 16.0;
/// From this size on a picture is large.
const LARGE: f32 = 48.0;
/// Width of the ring around the dot and of the speaking ring.
const RING: f32 = 2.0;
/// Opacity of the picture of someone who is offline.
const DIMMED: f32 = 0.55;

/// How an avatar is drawn.
#[derive(Clone, Copy, Debug)]
pub(crate) struct AvatarLook {
    pub size: f32,
    /// The presence dot at the bottom right; `None` draws no dot.
    pub presence: Option<Presence>,
    /// The accent ring of someone who is speaking.
    pub speaking: bool,
    /// Offline: the picture is dimmed.
    pub dimmed: bool,
}

impl AvatarLook {
    pub const fn plain(size: f32) -> Self {
        Self {
            size,
            presence: None,
            speaking: false,
            dimmed: false,
        }
    }

    pub const fn presence(mut self, presence: Presence) -> Self {
        self.presence = Some(presence);
        self
    }

    pub const fn speaking(mut self, speaking: bool) -> Self {
        self.speaking = speaking;
        self
    }

    pub const fn dimmed(mut self, dimmed: bool) -> Self {
        self.dimmed = dimmed;
        self
    }
}

/// The colour of a presence dot; `None` for offline, which has no dot.
pub(crate) fn presence_color(tokens: &Tokens, presence: Presence) -> Option<Color> {
    match presence {
        Presence::Online => Some(tokens.online),
        Presence::Idle => Some(tokens.idle),
        Presence::DoNotDisturb => Some(tokens.do_not_disturb),
        Presence::Offline => None,
    }
}

fn monogram_role(size: f32) -> TextRole {
    if size >= 28.0 {
        TextRole::CaptionStrong
    } else {
        TextRole::MicroStrong
    }
}

/// The side, in pixels, a picture is decoded at for an avatar drawn `size`
/// logical pixels wide. Three sizes serve every avatar of the shell: each is
/// at least twice the drawn size, so it stays sharp up to a 200% display, and
/// a picture is decoded at most three times however many places show it (the
/// sources can be megapixel files).
fn decode_size(size: f32) -> u32 {
    if size <= 24.0 {
        48
    } else if size < LARGE {
        72
    } else {
        128
    }
}

pub(crate) fn avatar(tokens: &Tokens, avatar: &Avatar, look: AvatarLook) -> Stack {
    let size = look.size;
    let radius = size / 2.0;
    let opacity = if look.dimmed { DIMMED } else { 1.0 };
    let decode_size = decode_size(size);
    // lurq clips children to their parent by default; the speaking ring and
    // the presence dot hang over the edge of the picture on purpose.
    let mut stack = Stack::new().width(size).height(size).overflow_visible();

    stack = match &avatar.picture {
        Some(picture) => stack.child(
            Image::from_source(
                ImageSource::Bytes(picture.bytes.clone()),
                ImageKey::new(picture.key),
            )
            .downscale_to(decode_size, decode_size)
            .cover()
            .size(size, size)
            .rounded(radius)
            .clip()
            .opacity(opacity),
        ),
        None => stack.child(
            Row::new()
                .size(size, size)
                .align_items(Alignment::Center)
                .justify(Justify::Center)
                .rounded(radius)
                .background_gradient(Gradient::linear(
                    180.0,
                    [tokens.monogram_top, tokens.monogram_bottom],
                ))
                .border_inside(1.0, tokens.monogram_border)
                .opacity(opacity)
                .child(text(monogram_role(size), tokens.text, &avatar.initials)),
        ),
    };

    if look.speaking {
        // Outside the picture, as the design strokes it.
        let outer = size + RING * 2.0;
        stack = stack.child(
            Rect::new(outer, outer)
                .absolute(-RING, -RING, outer, outer)
                .rounded(outer / 2.0)
                .border_inside(RING, tokens.accent)
                .pointer_events_none(),
        );
    }

    if let Some(color) = look
        .presence
        .and_then(|presence| presence_color(tokens, presence))
    {
        // The design puts the 13 px dot 22 px into a 34 px picture and 17 px
        // into a 28 px one: it hangs over the edge by one or two pixels. On a
        // large picture the dot is 16 px and ends on the picture's corner.
        let (dot, offset) = if size >= LARGE {
            (DOT_LARGE, size - DOT_LARGE)
        } else if size >= 32.0 {
            (DOT, size - 12.0)
        } else {
            (DOT, size - 11.0)
        };
        stack = stack.child(
            Rect::new(dot, dot)
                .absolute(offset, offset, dot, dot)
                .rounded(dot / 2.0)
                .background(color)
                .border_inside(RING, tokens.presence_ring)
                .pointer_events_none(),
        );
    }

    stack
}
