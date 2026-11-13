//! Settings: the navigation panel, with the voice block under it as in a
//! party, and the page it is on.
//!
//! A page is a title on the backdrop and a column of glass cards that scrolls
//! under the title bar and runs to the bottom edge of the window. This file
//! has the Appearance page and what every page is built from (the card, the
//! switch); the other pages are in the `settings_*` modules next to it.

use lurq::{
    app::{component::Component, ctx::Ctx},
    components::{Column, Rect, Row, ScrollVertical, Spacer, Stack},
    layout::{Alignment, layout_kind::Justify},
    node::{CursorIcon, Element, Gradient, padding::Padding},
};

use super::{
    icon::{Icon, icon},
    settings_account::{keys_cards, profile_cards},
    settings_app::app_cards,
    settings_hotkeys::hotkeys_card,
    settings_video::video_cards,
    settings_voice::voice_cards,
    surface::{FILL, glass, scrollbar, surfaced},
    tokens,
    voice_block::VoiceBlock,
};
use crate::{
    commands::Command,
    model::{SettingsGroup, SettingsPage},
    theme::{
        LightId, Surface, TextRole, Tokens,
        metrics::{PANEL_GAP, PANEL_PADDING, SIDE_PANEL_WIDTH},
        tokens::{RADIUS_BLOCK, RADIUS_CONTROL},
        typography::text,
    },
    view_model::ShellModel,
};

/// Height of a row of the navigation, and of its "Settings" title.
const NAV_ROW: f32 = 36.0;
const NAV_TITLE: f32 = 40.0;
/// Padding inside a card.
pub(crate) const CARD_PADDING: f32 = 28.0;
/// Height of a card's title line and of the line under it.
const CARD_TITLE: f32 = 20.0;
const CARD_HINT: f32 = 17.0;
/// From the title (or the line under it) to the body of a card.
const CARD_BODY_GAP: f32 = 19.0;
/// The same when the title shares its line with a button, and the height of
/// that line.
const CARD_ACTION_GAP: f32 = 32.0;
const CARD_ACTION_LINE: f32 = 34.0;
/// A light's tile on the Appearance page, and the gaps of their grid.
const LIGHT_TILE_HEIGHT: f32 = 148.0;
const LIGHT_PREVIEW_HEIGHT: f32 = 102.0;
const LIGHT_GRID_GAP: (f32, f32) = (16.0, 20.0);
/// Lights in one row of the grid.
const LIGHTS_PER_ROW: usize = 4;
/// Size of a switch.
const SWITCH: (f32, f32) = (36.0, 20.0);

pub(crate) struct Settings;

impl Component for Settings {
    type Props = ShellModel;

    fn create(_ctx: &mut Ctx) -> Self {
        Self
    }

    fn render(&self, ctx: &mut Ctx) -> impl Into<Element> {
        let tokens = tokens(ctx);
        let model = ctx.props::<ShellModel>().clone();
        let page = model.settings_page();

        let navigation = glass(
            &tokens.panel,
            Column::new()
                .width(FILL)
                .height(FILL)
                .padding(PANEL_PADDING)
                .spacing(PANEL_PADDING)
                .child(navigation(&tokens, &model, page))
                .child(ctx.mount::<VoiceBlock>(model.clone())),
        )
        .id("settings-nav")
        .width(SIDE_PANEL_WIDTH)
        .height(FILL);

        let mut content = Column::new()
            .id("settings-page")
            .width(FILL)
            .padding_custom(Padding::new().right(PANEL_GAP).bottom(PANEL_GAP))
            .spacing(PANEL_GAP)
            // The shadow of the last card reaches past the end of the column.
            .overflow_visible()
            .child(page_header(&tokens, page, summary(page)));
        match page {
            SettingsPage::VoiceAndAudio => {
                for card in voice_cards(&tokens, &model) {
                    content = content.child(card);
                }
            }
            SettingsPage::Appearance => {
                content = content
                    .child(lights_card(&tokens, &model))
                    .child(glass_card(&tokens, &model));
            }
            SettingsPage::AppAndNotifications => {
                content = content.child(app_cards(&tokens, &model));
            }
            SettingsPage::VideoAndSharing => {
                for card in video_cards(&tokens, &model) {
                    content = content.child(card);
                }
            }
            SettingsPage::Hotkeys => {
                content = content.child(hotkeys_card(&tokens, &model));
            }
            SettingsPage::Profile => {
                for card in profile_cards(&tokens, &model) {
                    content = content.child(card);
                }
            }
            SettingsPage::Keys => {
                for card in keys_cards(&tokens, &model) {
                    content = content.child(card);
                }
            }
        }

        // The navigation ends above the window edge like every side panel;
        // the page scrolls to the edge itself.
        Row::new()
            .width(FILL)
            .height(FILL)
            .padding_left(PANEL_GAP)
            .spacing(PANEL_GAP)
            .child(
                Column::new()
                    .height(FILL)
                    .padding_bottom(PANEL_GAP)
                    .child(navigation),
            )
            .child(
                ScrollVertical::new(content)
                    .scrollbar(scrollbar(&tokens))
                    .flex(1.0)
                    .height(FILL),
            )
    }
}

const fn page_icon(page: SettingsPage) -> Icon {
    match page {
        SettingsPage::Profile => Icon::Profile,
        SettingsPage::Keys => Icon::Key,
        SettingsPage::VoiceAndAudio => Icon::Mic,
        SettingsPage::VideoAndSharing => Icon::Camera,
        SettingsPage::Hotkeys => Icon::Keyboard,
        SettingsPage::AppAndNotifications => Icon::Bell,
        SettingsPage::Appearance => Icon::Palette,
    }
}

/// The line under a page's title.
const fn summary(page: SettingsPage) -> &'static str {
    match page {
        SettingsPage::VoiceAndAudio => "Choose your devices and how your voice is picked up.",
        SettingsPage::Appearance => {
            "Pick the light behind the glass. It sets the backdrop and the accent everywhere in the app."
        }
        SettingsPage::AppAndNotifications => "How Parties behaves on this computer.",
        SettingsPage::VideoAndSharing => {
            "Camera and screen share defaults. You can still change them each time you start sharing."
        }
        SettingsPage::Hotkeys => {
            "Shortcuts for the things you do mid-conversation. Click a shortcut to change it."
        }
        SettingsPage::Profile => "Your picture and name. They follow you to every party you join.",
        SettingsPage::Keys => {
            "Your identity is a key, not a password. Manage the keys that sign you in."
        }
    }
}

fn navigation(tokens: &Tokens, model: &ShellModel, current: SettingsPage) -> Column {
    let mut list = Column::new().width(FILL).flex(1.0).spacing(2.0).child(
        Row::new()
            .width(FILL)
            .height(NAV_TITLE)
            .padding_horizontal(8.0)
            .align_items(Alignment::Center)
            .child(text(TextRole::HeadingPage, tokens.text, "Settings")),
    );
    for (index, group) in SettingsGroup::ALL.into_iter().enumerate() {
        list = list.child(
            Row::new()
                .width(FILL)
                .padding_custom(
                    Padding::new()
                        .top(if index == 0 { 8.0 } else { 12.0 })
                        .bottom(if index == 0 { 4.0 } else { 6.0 })
                        .left(8.0),
                )
                .child(text(TextRole::Control, tokens.text_muted, group.label())),
        );
        for page in SettingsPage::ALL
            .into_iter()
            .filter(|page| page.group() == group)
        {
            list = list.child(nav_row(tokens, model, page, page == current));
        }
    }
    list
}

fn nav_row(tokens: &Tokens, model: &ShellModel, page: SettingsPage, current: bool) -> Row {
    let (icon_color, label_color) = if current {
        (tokens.accent, tokens.text)
    } else {
        (tokens.text_secondary, tokens.text_secondary)
    };
    let row = Row::new()
        .id(format!("settings-{}", page.slug()))
        .width(FILL)
        .height(NAV_ROW)
        .padding_horizontal(10.0)
        .spacing(10.0)
        .align_items(Alignment::Center)
        .cursor(CursorIcon::Pointer)
        .on_click(model.command(Command::ShowSettingsPage(page)))
        .child(icon(page_icon(page), 16.0, icon_color))
        .child(text(TextRole::Label, label_color, page.label()));
    if current {
        surfaced!(row, tokens.row_selected).class("current")
    } else {
        let hover = tokens.row_hover.fill;
        row.rounded(RADIUS_CONTROL)
            .hovered(move |style| style.background(hover))
    }
}

/// The title of a page and the line under it, on the backdrop.
fn page_header(tokens: &Tokens, page: SettingsPage, summary: &str) -> Column {
    Column::new()
        .width(FILL)
        .padding_custom(Padding::new().top(20.0).bottom(23.0).left(10.0))
        .spacing(3.0)
        .child(text(TextRole::HeadingLarge, tokens.text, page.label()).id("settings-title"))
        .child(text(TextRole::Body, tokens.text_secondary, summary))
}

/// A card of a page: a title, an optional line under it or a button next to
/// it, and `body`. `body` is `body_height` tall: a glass surface cannot take
/// its height from its content (see `surface::glass`), so a card computes it.
pub(crate) fn card(
    tokens: &Tokens,
    id: &'static str,
    title: &str,
    hint: Option<&str>,
    action: Option<Element>,
    body_height: f32,
    body: impl Into<Element>,
) -> Stack {
    let title = text(TextRole::HeadingPage, tokens.text, title);
    let (mut head, mut head_height, gap) = match action {
        Some(action) => (
            Column::new().width(FILL).child(
                Row::new()
                    .width(FILL)
                    .height(CARD_ACTION_LINE)
                    .align_items(Alignment::Center)
                    .child(title)
                    .child(Spacer::new().flex(1.0))
                    .child(action),
            ),
            CARD_ACTION_LINE,
            CARD_ACTION_GAP,
        ),
        None => (
            Column::new().width(FILL).child(title),
            CARD_TITLE,
            CARD_BODY_GAP,
        ),
    };
    if let Some(hint) = hint {
        head = head.child(Spacer::new().height(4.0)).child(text(
            TextRole::Detail,
            tokens.text_muted,
            hint,
        ));
        head_height += 4.0 + CARD_HINT;
    }
    glass(
        &tokens.panel_reading,
        Column::new()
            .width(FILL)
            .padding(CARD_PADDING)
            .child(head)
            .child(Spacer::new().height(gap))
            .child(body),
    )
    .id(id)
    .width(FILL)
    .height(CARD_PADDING * 2.0 + head_height + gap + body_height)
}

/// A switch. It shows `on`; the row around it carries the click.
pub(crate) fn switch(tokens: &Tokens, on: bool) -> Row {
    let (track, knob, justify) = if on {
        (tokens.accent, tokens.on_accent, Justify::End)
    } else {
        (tokens.switch_track, tokens.switch_knob, Justify::Start)
    };
    Row::new()
        .size(SWITCH.0, SWITCH.1)
        .padding(2.0)
        .align_items(Alignment::Center)
        .justify(justify)
        .rounded(SWITCH.1 / 2.0)
        .background(track)
        .child(
            Rect::new(SWITCH.1 - 4.0, SWITCH.1 - 4.0)
                .rounded((SWITCH.1 - 4.0) / 2.0)
                .background(knob),
        )
}

// --- Appearance ---

/// "Light": the eight lights, four to a row.
fn lights_card(tokens: &Tokens, model: &ShellModel) -> Stack {
    let current = model.light();
    let rows = LightId::ALL.len().div_ceil(LIGHTS_PER_ROW);
    let mut grid = Column::new().width(FILL).spacing(LIGHT_GRID_GAP.1);
    for lights in LightId::ALL.chunks(LIGHTS_PER_ROW) {
        let mut row = Row::new().width(FILL).spacing(LIGHT_GRID_GAP.0);
        for light in lights {
            row = row.child(light_tile(tokens, model, *light, *light == current));
        }
        grid = grid.child(row);
    }
    let grid_height =
        rows as f32 * LIGHT_TILE_HEIGHT + (rows as f32 - 1.0).max(0.0) * LIGHT_GRID_GAP.1;
    card(
        tokens,
        "settings-lights",
        "Light",
        Some("Eight to choose from. Night is the one you start with."),
        None,
        grid_height,
        grid,
    )
}

/// One light: a small picture of a window in it, and its name.
fn light_tile(tokens: &Tokens, model: &ShellModel, light: LightId, current: bool) -> Column {
    // The picture shows the light itself, not the one the window is in.
    let colors = Tokens::for_light(light);
    let surface = if current {
        Surface {
            radius: RADIUS_BLOCK,
            ..tokens.block_lit
        }
    } else {
        Surface {
            radius: RADIUS_BLOCK,
            ..tokens.card
        }
    };
    let mut label = Row::new()
        .width(FILL)
        .flex(1.0)
        .padding_horizontal(4.0)
        .spacing(8.0)
        .align_items(Alignment::Center)
        .child(Rect::new(12.0, 12.0).rounded(6.0).background(colors.accent))
        .child(text(TextRole::Label, tokens.text, light.name()))
        .child(Spacer::new().flex(1.0));
    if current {
        label = label.child(icon(Icon::Selected, 16.0, tokens.accent));
    }
    let mut tile = surfaced!(
        Column::new()
            .id(format!("light-{}", light.name().to_lowercase()))
            .flex(1.0)
            .height(LIGHT_TILE_HEIGHT)
            // The name's line takes the rest of the tile, down to its edge.
            .padding_custom(Padding::new().top(8.0).right(8.0).left(8.0))
            .cursor(CursorIcon::Pointer)
            .on_click(model.command(Command::SetLight(light))),
        surface
    )
    .child(light_preview(&colors))
    .child(label);
    if current {
        tile = tile.class("current").border_inside(2.0, tokens.accent);
    } else {
        let hover = tokens.control_hover.border;
        tile = tile.hovered(move |style| style.border_inside(1.0, hover));
    }
    tile
}

/// A window in miniature: the light's backdrop, a side panel with the
/// selected row, the chat panel with two lines, and the voice block.
fn light_preview(colors: &Tokens) -> Stack {
    let backdrop = &colors.backdrop;
    let panel = colors.panel_reading.fill_top;
    let outline = colors.hairline;
    let block = |x: f32, y: f32, width: f32, height: f32, radius: f32| {
        Stack::new()
            .absolute(x, y, width, height)
            .rounded(radius)
            .background(panel)
            .border_inside(1.0, outline)
    };
    let line = |x: f32, y: f32, width: f32, color| {
        Rect::new(width, 3.0)
            .absolute(x, y, width, 3.0)
            .rounded(1.5)
            .background(color)
    };
    Stack::new()
        .width(FILL)
        .height(LIGHT_PREVIEW_HEIGHT)
        .rounded(10.0)
        .clip()
        .background_gradient(Gradient::linear(
            135.0,
            [
                backdrop.wash_top_left.stops[1].0,
                backdrop.base,
                backdrop.wash_bottom_right.stops[1].0,
            ],
        ))
        .pointer_events_none()
        .child(
            block(13.0, 12.0, 62.0, 78.0, 6.0).child(
                Rect::new(50.0, 9.0)
                    .absolute(6.0, 11.0, 50.0, 9.0)
                    .rounded(4.5)
                    .background(colors.row_selected.fill)
                    .border_inside(1.0, colors.row_selected.border),
            ),
        )
        .child(
            block(85.0, 11.0, 142.0, 53.0, 6.0)
                .child(line(11.0, 15.0, 76.0, colors.divider))
                .child(line(11.0, 27.0, 108.0, colors.hairline)),
        )
        .child(
            block(103.0, 72.0, 109.0, 18.0, 9.0)
                .child(
                    Rect::new(8.0, 8.0)
                        .absolute(7.0, 5.0, 8.0, 8.0)
                        .rounded(4.0)
                        .background(colors.accent),
                )
                .child(
                    Rect::new(22.0, 8.0)
                        .absolute(79.0, 5.0, 22.0, 8.0)
                        .rounded(4.0)
                        .background(colors.accent),
                ),
        )
}

/// "Glass": solid panels in place of blurred glass.
fn glass_card(tokens: &Tokens, model: &ShellModel) -> Stack {
    let reduced = !model.backdrop_blur();
    let row = Row::new()
        .id("settings-reduce-transparency")
        .width(FILL)
        .align_items(Alignment::Center)
        .cursor(CursorIcon::Pointer)
        // The command names what the glass does; the switch names its opposite.
        .on_click(model.command(Command::SetBackdropBlur(reduced)))
        .child(
            Column::new()
                .flex(1.0)
                .spacing(3.0)
                .child(text(TextRole::Label, tokens.text, "Reduce transparency"))
                .child(text(
                    TextRole::Detail,
                    tokens.text_muted,
                    "Solid panels instead of blurred glass. Easier to read and lighter on the graphics card.",
                )),
        )
        .child(switch(tokens, reduced));
    let row = if reduced { row.class("on") } else { row };
    // Two lines of text and the gap between them.
    card(tokens, "settings-glass", "Glass", None, None, 37.0, row)
}
