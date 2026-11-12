//! The two menus that open upwards from the voice block: the Stream menu
//! (Screen, Camera, Music) and the presence menu (Online, Idle, Do not
//! disturb, Change picture).

use lurq::{
    components::{Column, Rect, Row, Stack},
    layout::{Alignment, layout_kind::Justify},
    node::{CursorIcon, color::Color},
};

use super::{
    avatar::presence_color,
    icon::{Icon, icon},
    surface::{FILL, glass, surfaced},
};
use crate::{
    commands::Command,
    model::{Presence, StreamSource, Streams},
    theme::{TextRole, Tokens, metrics::SIDE_BLOCK_WIDTH, typography::text},
    view_model::ShellModel,
};

/// Height of a menu row.
const ROW: f32 = 34.0;
/// Padding inside a menu.
const PADDING: f32 = 6.0;
/// Side of the leading mark (icon or presence dot) of a row.
const MARK: f32 = 16.0;
/// Space between the rows of the Stream menu.
const STREAM_ROW_GAP: f32 = 2.0;
/// Space above and below the hairline of the presence menu.
const HAIRLINE_PADDING: f32 = 5.0;

/// A menu row without its leading mark and label.
fn menu_row(tokens: &Tokens, id: String) -> Row {
    let hover = tokens.menu_row_current.fill;
    Row::new()
        .id(id)
        .width(FILL)
        .height(ROW)
        .padding_horizontal(10.0)
        .spacing(10.0)
        .align_items(Alignment::Center)
        .rounded(tokens.menu_row_current.radius)
        .cursor(CursorIcon::Pointer)
        .hovered(move |style| style.background(hover))
}

const fn stream_icon(source: StreamSource) -> Icon {
    match source {
        StreamSource::Screen => Icon::Screen,
        StreamSource::Camera => Icon::Camera,
        StreamSource::Music => Icon::Music,
    }
}

/// The Stream menu. A source that is on is tinted and marked "Live";
/// choosing a row turns that source on or off and leaves the menu open, so
/// several can be switched in one go.
pub(crate) fn stream_menu(tokens: &Tokens, model: &ShellModel, streams: Streams) -> Stack {
    let mut rows = Column::new()
        .width(FILL)
        .padding(PADDING)
        .spacing(STREAM_ROW_GAP);
    for source in StreamSource::ALL {
        let live = streams.is_live(source);
        let id = format!("stream-{}", source.label().to_lowercase());
        let mut row = menu_row(tokens, id)
            .on_click(model.command(Command::ToggleStream(source)))
            .child(icon(
                stream_icon(source),
                MARK,
                if live {
                    tokens.accent
                } else {
                    tokens.text_secondary
                },
            ))
            .child(text(TextRole::Label, tokens.text, source.label()).flex(1.0));
        if live {
            row = surfaced!(row, tokens.menu_row_live)
                .class("live")
                .child(text(TextRole::Control, tokens.accent, "Live"));
        }
        rows = rows.child(row);
    }
    // A popup is measured without a height limit, so the glass (whose border
    // overlay fills its parent) needs its height spelled out.
    let sources = StreamSource::ALL.len() as f32;
    let height = PADDING * 2.0 + sources * ROW + (sources - 1.0) * STREAM_ROW_GAP;
    glass(&tokens.menu_stream, rows)
        .id("stream-menu")
        .width(FILL)
        .height(height)
}

/// The leading mark of a presence row: a dot, or the "do not disturb" sign.
fn presence_mark(tokens: &Tokens, presence: Presence) -> Row {
    let mark = Row::new()
        .size(MARK, MARK)
        .align_items(Alignment::Center)
        .justify(Justify::Center);
    let color: Color = presence_color(tokens, presence).unwrap_or(tokens.text_faint);
    if presence == Presence::DoNotDisturb {
        mark.child(icon(Icon::DoNotDisturb, MARK, color))
    } else {
        mark.child(Rect::new(10.0, 10.0).rounded(5.0).background(color))
    }
}

/// The presence menu: as wide as the voice block, opened from the profile
/// line. Choosing a presence closes it.
pub(crate) fn presence_menu(tokens: &Tokens, model: &ShellModel, current: Presence) -> Stack {
    let mut rows = Column::new().width(FILL).padding(PADDING);
    for presence in [Presence::Online, Presence::Idle, Presence::DoNotDisturb] {
        let id = format!(
            "presence-{}",
            presence.label().to_lowercase().replace(' ', "-")
        );
        let mut row = menu_row(tokens, id)
            .on_click(model.command(Command::SetPresence(presence)))
            .child(presence_mark(tokens, presence))
            .child(text(TextRole::Label, tokens.text, presence.label()).flex(1.0));
        if presence == current {
            row = row
                .background(tokens.menu_row_current.fill)
                .class("current")
                .child(icon(Icon::Check, 14.0, tokens.accent));
        }
        rows = rows.child(row);
    }
    rows = rows
        .child(
            Row::new()
                .width(FILL)
                .padding_vertical(HAIRLINE_PADDING)
                .padding_horizontal(4.0)
                .child(Rect::new(FILL, 1.0).background(tokens.hairline)),
        )
        .child(
            menu_row(tokens, "presence-change-picture".to_owned())
                .on_click(model.command(Command::ChangePicture))
                .child(icon(Icon::Image, MARK, tokens.text_secondary))
                .child(text(TextRole::Label, tokens.text, "Change picture").flex(1.0)),
        );
    // Three presences, the hairline with its padding, "Change picture".
    let height = PADDING * 2.0 + 4.0 * ROW + HAIRLINE_PADDING * 2.0 + 1.0;
    glass(&tokens.menu, rows)
        .id("presence-menu")
        .width(SIDE_BLOCK_WIDTH)
        .height(height)
}
