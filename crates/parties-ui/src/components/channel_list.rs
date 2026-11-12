//! The upper part of the left panel: the party header, the voice rooms with
//! their members, and the text channels.

use lurq::{
    app::{component::Component, ctx::Ctx},
    components::{Column, Rect, Row, ScrollVertical, Spacer},
    layout::{Alignment, layout_kind::Justify},
    node::{CursorIcon, Element, color::Color, padding::Padding},
};

use super::{
    avatar::{AvatarLook, avatar},
    icon::{Icon, icon},
    icon_button,
    surface::{FILL, scrollbar, surfaced},
    tokens,
};
use crate::{
    commands::Command,
    model::{PartyHeader, PartyIcon, RoomActivity, RoomMember, TextChannel, UserId, VoiceRoom},
    theme::{TextRole, Tokens, tokens::RADIUS_CONTROL, typography::text},
    view_model::ShellModel,
};

/// Height of a room or channel row.
const ROW: f32 = 32.0;
/// Height of a member row under a room.
const MEMBER_ROW: f32 = 28.0;

pub(crate) struct ChannelList;

impl Component for ChannelList {
    type Props = ShellModel;

    fn create(_ctx: &mut Ctx) -> Self {
        Self
    }

    fn render(&self, ctx: &mut Ctx) -> impl Into<Element> {
        let tokens = tokens(ctx);
        let model = ctx.props::<ShellModel>().clone();
        let me = model.me();
        let own_room = me.room.as_ref().map(|room| room.id);
        let selected = model.selected_channel();

        let mut list =
            Column::new()
                .width(FILL)
                .spacing(2.0)
                .child(group_header(&tokens, "Voice rooms", 8.0));

        for room in model.voice_rooms() {
            let lit = own_room == Some(room.id);
            list = list.child(voice_room(&tokens, &model, &room, lit, me.user));
        }

        list = list.child(group_header(&tokens, "Text channels", 14.0));
        for channel in model.text_channels() {
            list = list.child(text_channel(
                &tokens,
                &model,
                &channel,
                channel.id == selected,
            ));
        }

        // The header stays put; the rooms and channels take the height that
        // is left above the voice block and scroll when they need more.
        Column::new()
            .id("channel-list")
            .width(FILL)
            .flex(1.0)
            .spacing(2.0)
            .child(party_header(&tokens, &model.party()))
            .child(
                ScrollVertical::new(list)
                    .scrollbar(scrollbar(&tokens))
                    .width(FILL)
                    .flex(1.0),
            )
    }
}

const fn party_icon(icon: PartyIcon) -> Icon {
    match icon {
        PartyIcon::Moon => Icon::PartyMoon,
        PartyIcon::Film => Icon::PartyFilm,
        PartyIcon::Shield => Icon::PartyShield,
        PartyIcon::Server => Icon::PartyServer,
    }
}

fn party_header(tokens: &Tokens, party: &PartyHeader) -> Row {
    let mut name_row = Row::new()
        .spacing(6.0)
        .align_items(Alignment::Center)
        .child(text(TextRole::Title, tokens.text, &party.name));
    if party.locked {
        name_row = name_row.child(icon(Icon::Lock, 12.0, tokens.text_muted));
    }
    Row::new()
        .id("party-header")
        .width(FILL)
        .padding_custom(Padding::new().top(6.0).right(4.0).bottom(10.0).left(8.0))
        .spacing(10.0)
        .align_items(Alignment::Center)
        .child(
            surfaced!(
                Row::new()
                    .size(36.0, 36.0)
                    .align_items(Alignment::Center)
                    .justify(Justify::Center),
                tokens.tile_party
            )
            .child(icon(party_icon(party.icon), 18.0, tokens.text)),
        )
        .child(
            Column::new()
                .flex(1.0)
                .spacing(1.0)
                .child(name_row)
                .child(text(
                    TextRole::Caption,
                    tokens.text_muted,
                    &format!("{} of {} online", party.online, party.capacity),
                )),
        )
        .child(
            icon_button(
                Icon::More,
                18.0,
                tokens.text_muted,
                (28.0, 28.0),
                RADIUS_CONTROL,
                tokens.row_hover.fill,
            )
            .id("party-menu"),
        )
}

fn group_header(tokens: &Tokens, title: &str, padding_top: f32) -> Row {
    Row::new()
        .width(FILL)
        .padding_custom(
            Padding::new()
                .top(padding_top)
                .right(6.0)
                .bottom(4.0)
                .left(8.0),
        )
        .align_items(Alignment::Center)
        .justify(Justify::SpaceBetween)
        .child(text(TextRole::Control, tokens.text_muted, title))
        .child(icon(Icon::Plus, 14.0, tokens.text_muted))
}

/// A room row and the rows of its members. The room the user is in is drawn
/// as a lit block around both.
fn voice_room(
    tokens: &Tokens,
    model: &ShellModel,
    room: &VoiceRoom,
    lit: bool,
    me: UserId,
) -> Column {
    let (icon_color, name_color) = if lit {
        (tokens.accent, tokens.text)
    } else {
        (tokens.text_muted, tokens.text_secondary)
    };
    let mut row = Row::new()
        .id(format!("room-{}", room.id.0))
        .width(FILL)
        .height(ROW)
        .padding_horizontal(8.0)
        .spacing(8.0)
        .align_items(Alignment::Center)
        .rounded(RADIUS_CONTROL)
        .cursor(CursorIcon::Pointer)
        .on_click(model.command(Command::JoinRoom(room.id)))
        .child(icon(Icon::Speaker, 16.0, icon_color))
        .child(text(TextRole::Label, name_color, &room.name))
        .child(Spacer::new().flex(1.0));
    if !lit {
        let hover = tokens.row_hover.fill;
        row = row.hovered(move |style| style.background(hover));
    }
    if !room.members.is_empty() {
        row = row.child(text(
            TextRole::Control,
            tokens.text_muted,
            &room.members.len().to_string(),
        ));
    }

    // In the lit block the rows touch; outside it they keep the panel's
    // 2 px rhythm.
    let mut block = Column::new()
        .width(FILL)
        .spacing(if lit { 0.0 } else { 2.0 })
        .child(row);
    for member in &room.members {
        block = block.child(room_member(tokens, member, member.user == me));
    }
    if lit {
        surfaced!(
            block.padding_custom(Padding::new().top(2.0).right(2.0).bottom(4.0).left(2.0)),
            tokens.block_lit
        )
        .class("lit-room")
    } else {
        block
    }
}

fn activity_mark(tokens: &Tokens, activity: RoomActivity) -> Option<(Icon, Color)> {
    match activity {
        RoomActivity::Listening => None,
        RoomActivity::Speaking => Some((Icon::Speaking, tokens.accent)),
        RoomActivity::SharingScreen => Some((Icon::Sharing, tokens.text_secondary)),
        RoomActivity::Muted => Some((Icon::MicOff, tokens.text_muted)),
        RoomActivity::Deafened => Some((Icon::SpeakerOff, tokens.text_muted)),
    }
}

fn room_member(tokens: &Tokens, member: &RoomMember, is_self: bool) -> Row {
    let speaking = member.activity == RoomActivity::Speaking;
    let name = if speaking {
        text(TextRole::Label, tokens.text, &member.name)
    } else {
        text(TextRole::Detail, tokens.text_secondary, &member.name)
    };
    let mut row = Row::new()
        .width(FILL)
        .height(MEMBER_ROW)
        .padding_custom(Padding::new().right(8.0).left(32.0))
        .spacing(8.0)
        .align_items(Alignment::Center)
        .child(avatar(
            tokens,
            &member.avatar,
            AvatarLook::plain(20.0).speaking(speaking),
        ))
        .child(name);
    if is_self {
        row = row.child(text(TextRole::Caption, tokens.text_muted, "you"));
    }
    row = row.child(Spacer::new().flex(1.0));
    if let Some((glyph, color)) = activity_mark(tokens, member.activity) {
        row = row.child(icon(glyph, 14.0, color));
    }
    row
}

fn text_channel(tokens: &Tokens, model: &ShellModel, channel: &TextChannel, selected: bool) -> Row {
    let (icon_color, name) = if selected {
        (
            tokens.accent,
            text(TextRole::Label, tokens.text, &channel.name),
        )
    } else if channel.unread {
        (
            tokens.text,
            text(TextRole::LabelStrong, tokens.text, &channel.name),
        )
    } else {
        (
            tokens.text_muted,
            text(TextRole::Label, tokens.text_secondary, &channel.name),
        )
    };
    let mut row = Row::new()
        .id(format!("channel-{}", channel.id.0))
        .width(FILL)
        .height(ROW)
        .padding_custom(
            Padding::new()
                .right(if channel.unread { 10.0 } else { 8.0 })
                .left(8.0),
        )
        .spacing(8.0)
        .align_items(Alignment::Center)
        .cursor(CursorIcon::Pointer)
        .on_click(model.command(Command::SelectChannel(channel.id)))
        .child(icon(Icon::Hash, 16.0, icon_color))
        .child(name)
        .child(Spacer::new().flex(1.0));
    if channel.unread {
        row = row.child(Rect::new(8.0, 8.0).rounded(4.0).background(tokens.accent));
    }
    if selected {
        surfaced!(row, tokens.row_selected).class("selected-channel")
    } else {
        let hover = tokens.row_hover.fill;
        row.rounded(RADIUS_CONTROL)
            .hovered(move |style| style.background(hover))
    }
}
