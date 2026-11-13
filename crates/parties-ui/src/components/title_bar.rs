//! The title bar: brand, switcher (Home, Settings, the parties, "+") and
//! the window buttons. It sits inside lurq's window chrome, which makes the
//! bar a drag region; every control here swallows its press so it does not
//! start a drag.

use lurq::{
    app::{component::Component, ctx::Ctx, events::MouseEvent, window::WindowHandle},
    components::{Rect, Row, Spacer},
    layout::{Alignment, layout_kind::Justify},
    node::{CursorIcon, Element, color::Color},
};

use super::{
    icon::{Icon, icon},
    icon_button,
    surface::{FILL, surfaced},
    swallow_press, tokens,
};
use crate::{
    commands::Command,
    model::{PartyIcon, PartyTab, Switcher, View},
    theme::{TextRole, Tokens, metrics::TITLE_BAR_HEIGHT, tokens::RADIUS_CHIP, typography::text},
    view_model::ShellModel,
};

/// Side of a switcher tile and height of the wide item.
const ITEM: f32 = 28.0;
/// Size of a window button.
const WINDOW_BUTTON: (f32, f32) = (40.0, 36.0);

pub(crate) struct TitleBar;

impl Component for TitleBar {
    type Props = ShellModel;

    fn create(_ctx: &mut Ctx) -> Self {
        Self
    }

    fn render(&self, ctx: &mut Ctx) -> impl Into<Element> {
        let tokens = tokens(ctx);
        let model = ctx.props::<ShellModel>().clone();
        let switcher = model.switcher();
        let window = ctx.window();

        Row::new()
            .id("title-bar")
            .width(FILL)
            .height(TITLE_BAR_HEIGHT)
            .padding_horizontal(8.0)
            .align_items(Alignment::Center)
            .child(
                Row::new()
                    .padding_left(8.0)
                    .spacing(14.0)
                    .align_items(Alignment::Center)
                    .child(brand(&tokens, &model))
                    .child(switcher_bar(&tokens, &model, &switcher)),
            )
            .child(Spacer::new().flex(1.0))
            .child(window_buttons(&tokens, &window))
    }
}

fn brand(tokens: &Tokens, model: &ShellModel) -> Row {
    Row::new()
        .spacing(8.0)
        .align_items(Alignment::Center)
        .child(
            // Until Settings > Appearance exists, a click on the mark steps
            // through the lights. It is the only control for the light in
            // this window.
            Row::new()
                .id("brand-mark")
                .size(20.0, 20.0)
                .align_items(Alignment::Center)
                .justify(Justify::Center)
                .rounded(RADIUS_CHIP)
                .background(tokens.accent)
                .cursor(CursorIcon::Pointer)
                .on_mouse_down(swallow_press)
                .on_click(model.command(Command::CycleLight))
                .child(icon(Icon::Brand, 13.0, tokens.on_accent)),
        )
        .child(text(TextRole::Heading, tokens.text, "Parties"))
}

const fn party_icon(icon: PartyIcon) -> Icon {
    match icon {
        PartyIcon::Moon => Icon::PartyMoon,
        PartyIcon::Film => Icon::PartyFilm,
        PartyIcon::Shield => Icon::PartyShield,
        PartyIcon::Server => Icon::PartyServer,
    }
}

fn switcher_bar(tokens: &Tokens, model: &ShellModel, switcher: &Switcher) -> Row {
    let mut bar = surfaced!(
        Row::new()
            .id("switcher")
            .height(36.0)
            .padding(4.0)
            .spacing(2.0)
            .align_items(Alignment::Center),
        tokens.switcher
    )
    .backdrop_blur(tokens.switcher_blur)
    .child(
        view_item(tokens, Icon::Home, "Home", switcher.view == View::Home)
            .id("switch-home")
            .on_click(model.command(Command::ShowView(View::Home))),
    )
    .child(
        view_item(
            tokens,
            Icon::Settings,
            "Settings",
            switcher.view == View::Settings,
        )
        .id("switch-settings")
        .on_click(model.command(Command::ShowView(View::Settings))),
    )
    .child(Rect::new(1.0, 16.0).background(tokens.divider));

    for party in &switcher.parties {
        let connected = switcher.connected == Some(party.id);
        let in_view = switcher.view == View::Party(party.id);
        bar = bar.child(
            party_item(tokens, party, connected, in_view)
                .id(format!("switch-party-{}", party.id.0))
                .on_click(model.command(Command::ShowView(View::Party(party.id)))),
        );
    }

    bar.child(tile(tokens, Icon::Plus, 14.0, tokens.text_muted).id("switch-add"))
}

/// A switcher item that is one icon.
fn tile(tokens: &Tokens, glyph: Icon, icon_size: f32, color: Color) -> Row {
    icon_button(
        glyph,
        icon_size,
        color,
        (ITEM, ITEM),
        RADIUS_CHIP,
        tokens.tile.fill,
    )
    .on_mouse_down(swallow_press)
}

/// The item of the view the window is on: the icon with its label.
fn wide_item(contents: Row) -> Row {
    contents
        .height(ITEM)
        .padding_left(7.0)
        .padding_right(8.0)
        .spacing(7.0)
        .align_items(Alignment::Center)
        .cursor(CursorIcon::Pointer)
        .on_mouse_down(swallow_press)
}

/// Home or Settings: a tile, or the wide item with its label when in view.
fn view_item(tokens: &Tokens, glyph: Icon, label: &str, in_view: bool) -> Row {
    if !in_view {
        return tile(tokens, glyph, 16.0, tokens.text_secondary);
    }
    wide_item(
        Row::new()
            .rounded(tokens.tile.radius)
            .background(tokens.tile.fill)
            .child(icon(glyph, 16.0, tokens.text))
            .child(text(TextRole::Label, tokens.text, label)),
    )
}

/// A party: lit in the accent while connected, wide with its name while in view.
fn party_item(tokens: &Tokens, party: &PartyTab, connected: bool, in_view: bool) -> Row {
    let glyph = party_icon(party.icon);
    if !in_view {
        let color = if connected {
            tokens.accent
        } else if party.reachable {
            tokens.text_secondary
        } else {
            tokens.text_faint
        };
        let item = tile(tokens, glyph, 16.0, color);
        return if connected {
            surfaced!(item, tokens.pill_active)
        } else {
            item
        };
    }
    let surface = if connected {
        tokens.pill_active
    } else {
        tokens.tile
    };
    let icon_color = if connected {
        tokens.accent
    } else {
        tokens.text
    };
    wide_item(
        surfaced!(Row::new(), surface)
            .child(icon(glyph, 15.0, icon_color))
            .child(text(TextRole::Label, tokens.text, &party.name))
            .child(icon(Icon::CaretDown, 11.0, tokens.text_muted)),
    )
}

fn window_buttons(tokens: &Tokens, window: &WindowHandle) -> Row {
    let button = |glyph: Icon, icon_size: f32, hover: Color| {
        icon_button(
            glyph,
            icon_size,
            tokens.text_secondary,
            WINDOW_BUTTON,
            RADIUS_CHIP,
            hover,
        )
        .cursor(CursorIcon::Default)
        .on_mouse_down(swallow_press)
    };
    let maximized = window.is_maximized;
    let (minimise, maximise, close) = (window.clone(), window.clone(), window.clone());
    Row::new()
        .align_items(Alignment::Center)
        .child(
            button(Icon::Minimise, 14.0, tokens.tile.fill)
                .id("window-minimise")
                .on_click(move |_: MouseEvent| minimise.set_minimized(true)),
        )
        .child(
            button(
                if maximized {
                    Icon::Restore
                } else {
                    Icon::Maximise
                },
                13.0,
                tokens.tile.fill,
            )
            .id("window-maximise")
            .on_click(move |_: MouseEvent| maximise.set_maximized(!maximized)),
        )
        .child(
            button(Icon::Close, 14.0, tokens.control_danger.border)
                .id("window-close")
                // `request_close` lets a close handler veto it (unsent text,
                // an active call); `close` would not ask.
                .on_click(move |_: MouseEvent| close.request_close()),
        )
}
