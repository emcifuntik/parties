//! The message list of the chat panel: messages with attachments,
//! reactions and reply references, system lines and dividers.
//!
//! The fixture chat is a handful of rows, so this is a plain scrolling
//! column that sticks to the bottom. A real channel history wants lurq's
//! `VirtualizedList`.

use lurq::{
    components::{Column, Image, Rect, Row, ScrollVertical, Spacer, TextOverflow},
    images::{ImageKey, ImageSource},
    layout::{
        Alignment,
        layout_kind::{Justify, ScrollState},
    },
    node::{CursorIcon, padding::Padding},
};

use super::{
    avatar::{AvatarLook, avatar},
    icon::{Icon, icon},
    icon_button,
    surface::{FILL, scrollbar, surfaced},
};
use crate::{
    commands::Command,
    model::{
        Attachment, Chat, ChatItem, Message, Picture, Reaction, ReactionKind, ReplyReference,
        SystemLine, SystemPart,
    },
    theme::{
        TextRole, Tokens,
        tokens::{RADIUS_CHIP, RADIUS_CONTROL},
        typography::{paragraph, text},
    },
    view_model::ShellModel,
};

/// Side of a message author's picture.
const AVATAR: f32 = 36.0;
/// Space between the picture column and the message.
const GUTTER: f32 = 12.0;
/// Size of an image attachment's preview.
const IMAGE_PREVIEW: (f32, f32) = (248.0, 140.0);
/// Width of a file attachment card.
const FILE_CARD: f32 = 340.0;

pub(crate) fn message_list(
    tokens: &Tokens,
    model: &ShellModel,
    chat: &Chat,
    scroll: &ScrollState,
) -> Column {
    let mut rows = Column::new()
        .id("messages")
        .width(FILL)
        .padding_custom(Padding::new().top(12.0).right(20.0).bottom(10.0).left(20.0))
        .spacing(12.0);
    for item in &chat.items {
        rows = match item {
            ChatItem::DayDivider(day) => rows.child(day_divider(tokens, day)),
            ChatItem::Message(entry) => rows.child(message(tokens, model, entry)),
            ChatItem::System(line) => rows.child(system_line(tokens, line)),
            ChatItem::NewMessages => rows.child(new_messages(tokens)),
        };
    }
    // A short chat sits at the bottom of the panel, above the composer. A
    // scroll area gives its content unbounded height, so the content cannot
    // align itself to the bottom: a spacer above the scroll area takes the
    // free space, and the scroll area shrinks (and scrolls) once the rows no
    // longer fit.
    Column::new()
        .width(FILL)
        .flex(1.0)
        .child(Spacer::new().flex(1.0))
        .child(
            ScrollVertical::new(rows)
                .with_scroll_state(scroll.clone())
                .scrollbar(scrollbar(tokens))
                .width(FILL)
                .flex_shrink(1.0),
        )
}

fn rule(tokens: &Tokens) -> Rect {
    Rect::new(FILL, 1.0).background(tokens.hairline).flex(1.0)
}

fn day_divider(tokens: &Tokens, day: &str) -> Row {
    Row::new()
        .width(FILL)
        .spacing(12.0)
        .align_items(Alignment::Center)
        .child(rule(tokens))
        .child(text(TextRole::Control, tokens.text_muted, day))
        .child(rule(tokens))
}

fn new_messages(tokens: &Tokens) -> Row {
    Row::new()
        .class("new-messages")
        .width(FILL)
        .spacing(10.0)
        .align_items(Alignment::Center)
        .child(
            Rect::new(FILL, 1.0)
                .background(tokens.accent_line)
                .flex(1.0),
        )
        .child(text(TextRole::MicroStrong, tokens.accent, "New messages"))
}

fn system_line(tokens: &Tokens, line: &SystemLine) -> Row {
    let mut words = Row::new().spacing(4.0).align_items(Alignment::Center);
    for part in &line.parts {
        words = words.child(match part {
            SystemPart::Name(name) => text(TextRole::LabelStrong, tokens.text_secondary, name),
            SystemPart::Text(plain) => text(TextRole::Detail, tokens.text_muted, plain),
        });
    }
    Row::new()
        .class("system-line")
        .width(FILL)
        .spacing(GUTTER)
        .align_items(Alignment::Center)
        .child(
            Row::new()
                .size(AVATAR, 20.0)
                .align_items(Alignment::Center)
                .justify(Justify::Center)
                .child(icon(Icon::Ticket, 16.0, tokens.text_muted)),
        )
        .child(words)
        .child(text(TextRole::Micro, tokens.text_muted, &line.time))
}

fn message(tokens: &Tokens, model: &ShellModel, entry: &Message) -> Column {
    let mut body = Column::new()
        .flex(1.0)
        .spacing(3.0)
        .child(
            Row::new()
                .spacing(8.0)
                .align_items(Alignment::Center)
                .child(text(TextRole::BodyStrong, tokens.text, &entry.author))
                .child(text(TextRole::Micro, tokens.text_muted, &entry.time)),
        )
        .child(
            paragraph(TextRole::Body, tokens.text_body, &entry.text)
                .id(format!("message-text-{}", entry.id.0))
                .selectable(true)
                .selection_color(tokens.accent_line)
                .width(FILL),
        );
    if let Some(attachment) = &entry.attachment {
        body = body.child(Row::new().padding_top(5.0).child(match attachment {
            Attachment::Image {
                name,
                size,
                picture,
            } => image_attachment(tokens, name, size, picture.as_ref()),
            Attachment::File { name, size } => Column::new().child(file_card(tokens, name, size)),
        }));
    }
    if !entry.reactions.is_empty() {
        let mut reactions = Row::new().padding_top(5.0).spacing(6.0);
        for (index, reaction) in entry.reactions.iter().enumerate() {
            reactions = reactions.child(reaction_chip(tokens, reaction).on_click(model.command(
                Command::ToggleReaction {
                    message: entry.id,
                    index,
                },
            )));
        }
        body = body.child(reactions);
    }

    let row = Row::new()
        .width(FILL)
        .spacing(GUTTER)
        .child(avatar(tokens, &entry.avatar, AvatarLook::plain(AVATAR)))
        .child(body);
    let mut block = Column::new()
        .id(format!("message-{}", entry.id.0))
        .class("message")
        .width(FILL)
        .spacing(5.0);
    if let Some(reply) = &entry.reply_to {
        block = block.child(reply_reference(tokens, reply));
    }
    block.child(row)
}

fn reply_reference(tokens: &Tokens, reply: &ReplyReference) -> Row {
    Row::new()
        .class("reply-reference")
        .width(FILL)
        .padding_left(AVATAR + GUTTER)
        .spacing(6.0)
        .align_items(Alignment::Center)
        .child(icon(Icon::Reply, 14.0, tokens.text_muted))
        .child(avatar(tokens, &reply.avatar, AvatarLook::plain(16.0)))
        .child(text(
            TextRole::CaptionStrong,
            tokens.text_secondary,
            &reply.author,
        ))
        .child(
            text(TextRole::Caption, tokens.text_muted, &reply.excerpt)
                .text_overflow(TextOverflow::Elipsis)
                .flex(1.0),
        )
}

fn image_attachment(tokens: &Tokens, name: &str, size: &str, picture: Option<&Picture>) -> Column {
    let (width, height) = IMAGE_PREVIEW;
    let frame = Row::new()
        .class("image-attachment")
        .size(width, height)
        .rounded(RADIUS_CONTROL)
        .background(tokens.card.fill)
        .border_inside(1.0, tokens.image_border)
        .clip();
    let frame = match picture {
        Some(picture) => frame.child(
            Image::from_source(
                ImageSource::Bytes(picture.bytes.clone()),
                ImageKey::new(picture.key),
            )
            .cover()
            .size(width, height),
        ),
        None => frame,
    };
    Column::new().spacing(6.0).child(frame).child(
        Row::new()
            .spacing(6.0)
            .align_items(Alignment::Center)
            .child(icon(Icon::Image, 14.0, tokens.text_muted))
            .child(text(TextRole::Control, tokens.text_secondary, name))
            .child(text(TextRole::Caption, tokens.text_muted, size)),
    )
}

fn file_card(tokens: &Tokens, name: &str, size: &str) -> Row {
    surfaced!(
        Row::new()
            .class("file-attachment")
            .width(FILE_CARD)
            .padding(8.0)
            .spacing(10.0)
            .align_items(Alignment::Center),
        tokens.card
    )
    .child(
        Row::new()
            .size(36.0, 36.0)
            .align_items(Alignment::Center)
            .justify(Justify::Center)
            .rounded(RADIUS_CHIP)
            .background(tokens.tile.fill)
            .child(icon(Icon::FileCode, 18.0, tokens.text)),
    )
    .child(
        Column::new()
            .flex(1.0)
            .spacing(1.0)
            .child(text(TextRole::Label, tokens.text, name))
            .child(text(TextRole::Caption, tokens.text_muted, size)),
    )
    .child(
        icon_button(
            Icon::Download,
            18.0,
            tokens.text_secondary,
            (32.0, 32.0),
            RADIUS_CHIP,
            tokens.tile.fill,
        )
        .class("download"),
    )
}

const fn reaction_icon(kind: ReactionKind) -> Icon {
    match kind {
        ReactionKind::ThumbsUp => Icon::ThumbsUp,
        ReactionKind::Fire => Icon::Fire,
    }
}

fn reaction_chip(tokens: &Tokens, reaction: &Reaction) -> Row {
    let (surface, icon_color, count_color) = if reaction.mine {
        (tokens.chip_mine, tokens.accent, tokens.text)
    } else {
        (tokens.chip, tokens.text_secondary, tokens.text_secondary)
    };
    let chip = surfaced!(
        Row::new()
            .class("reaction")
            .height(26.0)
            .padding_custom(Padding::new().right(8.0).left(7.0))
            .spacing(6.0)
            .align_items(Alignment::Center)
            .cursor(CursorIcon::Pointer),
        surface
    )
    .child(icon(reaction_icon(reaction.kind), 14.0, icon_color))
    .child(text(
        TextRole::CaptionStrong,
        count_color,
        &reaction.count.to_string(),
    ));
    if reaction.mine {
        chip.class("mine")
    } else {
        chip
    }
}
