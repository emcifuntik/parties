//! The chat panel: channel header, message list, composer.

use std::sync::{Mutex, PoisonError};

use lurq::{
    app::{component::Component, ctx::Ctx},
    components::{Column, Rect, Row, TextOverflow},
    layout::{Alignment, layout_kind::ScrollState},
    node::{Element, border::Border, padding::Padding},
};

use super::{
    composer::Composer,
    icon::{Icon, icon},
    icon_button,
    message_list::message_list,
    surface::{FILL, surfaced},
    tokens,
};
use crate::{
    commands::Command,
    model::{ChannelId, Chat},
    theme::{TextRole, Tokens, tokens::RADIUS_CONTROL, typography::text},
    view_model::ShellModel,
};

/// Height of the channel header.
const HEADER: f32 = 52.0;
/// Side of a header action button.
const ACTION: f32 = 32.0;

pub(crate) struct ChatPanel {
    scroll: ScrollState,
    /// The channel and the number of rows the list last showed, to tell when
    /// to jump to the newest message.
    shown: Mutex<Option<(ChannelId, usize)>>,
}

impl Component for ChatPanel {
    type Props = ShellModel;

    fn create(_ctx: &mut Ctx) -> Self {
        Self {
            scroll: ScrollState::new(),
            shown: Mutex::new(None),
        }
    }

    fn render(&self, ctx: &mut Ctx) -> impl Into<Element> {
        let tokens = tokens(ctx);
        let model = ctx.props::<ShellModel>().clone();
        let chat = model.chat();

        // A chat opens at its newest message and follows new ones. Editing
        // a row (a reaction) keeps the place the user scrolled to.
        let showing = Some((chat.channel, chat.items.len()));
        let mut shown = self.shown.lock().unwrap_or_else(PoisonError::into_inner);
        if *shown != showing {
            *shown = showing;
            self.scroll.scroll_to_bottom_pending();
        }
        drop(shown);

        Column::new()
            .id("chat")
            .width(FILL)
            .height(FILL)
            .child(header(&tokens, &model, &chat, model.members_panel_open()))
            .child(message_list(&tokens, &model, &chat, &self.scroll))
            .child(ctx.mount::<Composer>(model.clone()))
    }
}

fn header(tokens: &Tokens, model: &ShellModel, chat: &Chat, members_open: bool) -> Row {
    let action = |glyph: Icon| {
        icon_button(
            glyph,
            18.0,
            tokens.text_secondary,
            (ACTION, ACTION),
            RADIUS_CONTROL,
            tokens.row_hover.fill,
        )
    };
    let members = if members_open {
        surfaced!(
            icon_button(
                Icon::Members,
                18.0,
                tokens.text,
                (ACTION, ACTION),
                RADIUS_CONTROL,
                tokens.tile_on.fill,
            ),
            tokens.tile_on
        )
        .class("on")
    } else {
        action(Icon::Members)
    };

    Row::new()
        .id("chat-header")
        .width(FILL)
        .height(HEADER)
        .padding_custom(Padding::new().right(10.0).left(18.0))
        .spacing(10.0)
        .align_items(Alignment::Center)
        .border_bottom(Border::inside(1.0, tokens.hairline))
        .child(icon(Icon::Hash, 18.0, tokens.text_muted))
        .child(text(TextRole::HeadingPage, tokens.text, &chat.name).id("chat-title"))
        .child(Rect::new(1.0, 16.0).background(tokens.divider))
        .child(
            text(TextRole::Detail, tokens.text_muted, &chat.topic)
                .text_overflow(TextOverflow::Elipsis)
                .flex(1.0),
        )
        .child(
            Row::new()
                .spacing(4.0)
                .align_items(Alignment::Center)
                .child(action(Icon::Pin).id("chat-pinned"))
                .child(action(Icon::Search).id("chat-search"))
                .child(
                    members
                        .id("chat-members")
                        .on_click(model.command(Command::ToggleMembersPanel)),
                ),
        )
}
