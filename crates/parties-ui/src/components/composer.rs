//! The composer at the bottom of the chat panel: attach, the draft, emoji,
//! send.

use lurq::{
    app::{
        component::Component,
        ctx::Ctx,
        events::{KeyboardEvent, MouseEvent},
    },
    components::{Column, Row},
    layout::{Alignment, layout_kind::Justify},
    node::{CursorIcon, Element, padding::Padding},
};

use super::{
    icon::{Icon, icon},
    icon_button,
    surface::{FILL, surfaced},
    tokens,
};
use crate::{
    commands::Command,
    theme::{TextRole, tokens::RADIUS_CHIP, typography::text_input},
    view_model::ShellModel,
};

/// Height of the composer field.
const FIELD: f32 = 48.0;
/// Side of a button inside the field.
const BUTTON: f32 = 32.0;

pub(crate) struct Composer;

impl Component for Composer {
    type Props = ShellModel;

    fn create(_ctx: &mut Ctx) -> Self {
        Self
    }

    fn render(&self, ctx: &mut Ctx) -> impl Into<Element> {
        let tokens = tokens(ctx);
        let model = ctx.props::<ShellModel>().clone();

        let side_button = |glyph: Icon| {
            icon_button(
                glyph,
                18.0,
                tokens.text_secondary,
                (BUTTON, BUTTON),
                RADIUS_CHIP,
                tokens.tile.fill,
            )
        };
        let send_on_enter = model.clone();
        let draft = text_input(
            TextRole::Body,
            tokens.text,
            tokens.text_muted,
            model.draft_signal(),
        )
        .id("composer-input")
        .placeholder("Write a message")
        .caret_color(tokens.accent)
        .selection_color(tokens.accent_line)
        .flex(1.0)
        .on_key_down(move |event: KeyboardEvent| {
            if event.key == "Enter" && !event.shift {
                event.prevent_default();
                send_on_enter.dispatch(Command::SendDraft);
            }
        });

        let field = surfaced!(
            Row::new()
                .id("composer")
                .width(FILL)
                .height(FIELD)
                .padding_horizontal(8.0)
                .spacing(6.0)
                .align_items(Alignment::Center),
            tokens.composer
        )
        .child(side_button(Icon::Attach).id("composer-attach"))
        .child(Row::new().flex(1.0).padding_left(2.0).child(draft))
        .child(side_button(Icon::Emoji).id("composer-emoji"))
        .child(
            Row::new()
                .id("composer-send")
                .size(BUTTON, BUTTON)
                .align_items(Alignment::Center)
                .justify(Justify::Center)
                .rounded(tokens.send.radius)
                .background(tokens.send.fill)
                .cursor(CursorIcon::Pointer)
                .on_click(move |_: MouseEvent| model.dispatch(Command::SendDraft))
                .child(icon(Icon::Send, 16.0, tokens.on_accent)),
        );

        Column::new()
            .width(FILL)
            .padding_custom(Padding::new().top(2.0).right(16.0).bottom(16.0).left(16.0))
            .child(field)
    }
}
