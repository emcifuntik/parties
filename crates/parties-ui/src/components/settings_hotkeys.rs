//! The Hotkeys page of Settings: one card with a shortcut for each thing the
//! user does mid-conversation.
//!
//! A click on a shortcut starts capturing a new one, and a click on the
//! capturing field stops it. Reading the keys themselves is the work of the
//! client core and its shell (the shortcuts work while the window is in the
//! background); this page shows the state they report.

use lurq::{
    components::{Column, Rect, Row, Spacer, Stack},
    layout::Alignment,
    node::{CursorIcon, Element},
};

use super::{
    icon::{Icon, icon},
    settings::card,
    settings_voice::icon_text_button,
    surface::{FILL, surfaced},
};
use crate::{
    commands::Command,
    model::{HotkeyAction, Hotkeys, Shortcut},
    theme::{
        Surface, TextRole, Tokens,
        tokens::{RADIUS_CHIP, RADIUS_CONTROL},
        typography::text,
    },
    view_model::ShellModel,
};

/// Height of a shortcut's row: two lines of text, and the field.
const ROW: f32 = 36.0;
/// Width of a shortcut field.
const FIELD_WIDTH: f32 = 200.0;
/// Space above and below the hairline between two shortcuts.
const DIVIDER_GAP: (f32, f32) = (9.0, 10.0);
/// The notice under a shortcut another application has taken, and the space
/// above it.
const NOTICE: f32 = 38.0;
const NOTICE_GAP: f32 = 9.0;

/// The card of the page.
pub(crate) fn hotkeys_card(tokens: &Tokens, model: &ShellModel) -> Stack {
    let hotkeys = model.hotkeys();
    let mut list = Column::new().width(FILL);
    let mut height = 0.0;
    for (index, shortcut) in hotkeys.shortcuts.iter().enumerate() {
        if index > 0 {
            list = list
                .child(Spacer::new().height(DIVIDER_GAP.0))
                .child(Rect::new(FILL, 1.0).background(tokens.hairline))
                .child(Spacer::new().height(DIVIDER_GAP.1));
            height += DIVIDER_GAP.0 + 1.0 + DIVIDER_GAP.1;
        }
        list = list.child(shortcut_row(tokens, model, &hotkeys, shortcut));
        height += ROW;
        if let (true, Some(keys)) = (shortcut.conflict, &shortcut.keys) {
            list = list
                .child(Spacer::new().height(NOTICE_GAP))
                .child(conflict_notice(tokens, model, shortcut.action, keys));
            height += NOTICE_GAP + NOTICE;
        }
    }
    card(
        tokens,
        "settings-shortcuts",
        "Shortcuts",
        None,
        Some(
            icon_text_button(tokens, Icon::Reset, "Reset to defaults")
                .id("hotkeys-reset")
                .on_click(model.command(Command::ResetHotkeys))
                .into(),
        ),
        height,
        list,
    )
}

/// The field of a shortcut, in one of its three states.
fn shortcut_field(
    tokens: &Tokens,
    model: &ShellModel,
    shortcut: &Shortcut,
    capturing: bool,
) -> Row {
    let slug = shortcut.action.slug();
    // The text of the field, whatever its state.
    let keys_id = format!("hotkey-{slug}-keys");
    let field = Row::new()
        .id(format!("hotkey-{slug}"))
        .size(FIELD_WIDTH, ROW)
        .padding_horizontal(12.0)
        .spacing(8.0)
        .align_items(Alignment::Center)
        .cursor(CursorIcon::Pointer);
    if capturing {
        return surfaced!(
            field.on_click(model.command(Command::CancelHotkeyCapture)),
            Surface {
                radius: RADIUS_CONTROL,
                ..tokens.block_lit
            }
        )
        .border_inside(1.5, tokens.accent)
        .class("capturing")
        .child(Rect::new(8.0, 8.0).rounded(4.0).background(tokens.accent))
        .child(text(TextRole::CodeBody, tokens.accent_text, "Press keys").id(keys_id));
    }
    let hover = tokens.control_hover.fill;
    let field = surfaced!(
        field
            .on_click(model.command(Command::CaptureHotkey(shortcut.action)))
            .hovered(move |style| style.background(hover)),
        tokens.field
    );
    match &shortcut.keys {
        Some(keys) => field.child(text(TextRole::CodeBody, tokens.text, keys).id(keys_id)),
        None => field
            .child(icon(Icon::Plus, 14.0, tokens.text_secondary))
            .child(text(TextRole::Label, tokens.text, "Add shortcut").id(keys_id)),
    }
}

/// A shortcut: what it does, a note about its state, and its field.
fn shortcut_row(
    tokens: &Tokens,
    model: &ShellModel,
    hotkeys: &Hotkeys,
    shortcut: &Shortcut,
) -> Row {
    let capturing = hotkeys.capturing == Some(shortcut.action);
    let note: Element = if capturing {
        text(TextRole::Caption, tokens.text_muted, "Esc to cancel").into()
    } else if shortcut.keys.is_none() {
        text(TextRole::Caption, tokens.text_muted, "Not set").into()
    } else if shortcut.conflict {
        icon(Icon::Warning, 16.0, tokens.idle).into()
    } else {
        Spacer::new().into()
    };
    Row::new()
        .width(FILL)
        .height(ROW)
        .spacing(13.0)
        .align_items(Alignment::Center)
        .child(
            Column::new()
                .flex(1.0)
                .spacing(3.0)
                .child(text(TextRole::Label, tokens.text, shortcut.action.title()))
                .child(text(
                    TextRole::Caption,
                    tokens.text_muted,
                    shortcut.action.hint(),
                )),
        )
        .child(note)
        .child(shortcut_field(tokens, model, shortcut, capturing))
}

/// Says that another application has taken the shortcut, and offers to
/// change it.
fn conflict_notice(tokens: &Tokens, model: &ShellModel, action: HotkeyAction, keys: &str) -> Row {
    let hover = tokens.control_hover.fill;
    surfaced!(
        Row::new()
            .id(format!("hotkey-{}-conflict", action.slug()))
            .width(FILL)
            .height(NOTICE)
            .padding_left(12.0)
            .padding_right(5.0)
            .spacing(10.0)
            .align_items(Alignment::Center),
        tokens.card
    )
    .child(icon(Icon::Warning, 14.0, tokens.idle))
    .child(text(
        TextRole::Caption,
        tokens.text_secondary,
        &format!(
            "{keys} is already taken by another app, so {} may not work in the background.",
            action.title()
        ),
    ))
    .child(Spacer::new().flex(1.0))
    .child(
        surfaced!(
            Row::new()
                .id(format!("hotkey-{}-change", action.slug()))
                .height(28.0)
                .padding_horizontal(10.0)
                .align_items(Alignment::Center)
                .cursor(CursorIcon::Pointer)
                .hovered(move |style| style.background(hover))
                .on_click(model.command(Command::CaptureHotkey(action))),
            Surface {
                radius: RADIUS_CHIP,
                ..tokens.control
            }
        )
        .child(text(TextRole::CaptionStrong, tokens.text, "Change")),
    )
}
