//! The members panel: a search field and the members in groups (Owner,
//! Moderators, Online, Offline).

use lurq::{
    app::{component::Component, ctx::Ctx},
    components::{Column, Row, ScrollVertical},
    layout::{Alignment, layout_kind::Justify},
    node::{CursorIcon, Element, padding::Padding},
};

use super::{
    avatar::{AvatarLook, avatar},
    icon::{Icon, icon},
    surface::{FILL, scrollbar, surfaced},
    tokens,
};
use crate::{
    model::{Member, MemberGroup, Presence},
    theme::{
        TextRole, Tokens,
        metrics::PANEL_PADDING,
        tokens::RADIUS_CONTROL,
        typography::{text, text_input},
    },
    view_model::ShellModel,
};

/// Height of a member row.
const ROW: f32 = 44.0;
/// Side of a member's picture.
const AVATAR: f32 = 28.0;

pub(crate) struct MemberList;

impl Component for MemberList {
    type Props = ShellModel;

    fn create(_ctx: &mut Ctx) -> Self {
        Self
    }

    fn render(&self, ctx: &mut Ctx) -> impl Into<Element> {
        let tokens = tokens(ctx);
        let model = ctx.props::<ShellModel>().clone();
        let members = model.members();
        let query = model.member_search().trim().to_lowercase();

        let mut list = Column::new().width(FILL).spacing(2.0);
        for group in &members.groups {
            let shown: Vec<&Member> = group
                .members
                .iter()
                .filter(|member| query.is_empty() || member.name.to_lowercase().contains(&query))
                .collect();
            if shown.is_empty() {
                continue;
            }
            list = list.child(group_header(&tokens, group, query.is_empty()));
            for member in shown {
                list = list.child(member_row(&tokens, member));
            }
        }
        // "Show more" counts members the list does not hold, so it cannot
        // say how many of them match a search.
        if members.hidden > 0 && query.is_empty() {
            list = list.child(show_more(&tokens, members.hidden));
        }

        Column::new()
            .id("members")
            .width(FILL)
            .height(FILL)
            .padding(PANEL_PADDING)
            .spacing(2.0)
            .child(search_field(&tokens, &model))
            .child(
                ScrollVertical::new(list)
                    .scrollbar(scrollbar(&tokens))
                    .width(FILL)
                    .flex(1.0),
            )
    }
}

fn search_field(tokens: &Tokens, model: &ShellModel) -> Row {
    surfaced!(
        Row::new()
            .id("member-search")
            .width(FILL)
            .height(36.0)
            .padding_horizontal(10.0)
            .spacing(8.0)
            .align_items(Alignment::Center),
        tokens.field
    )
    .child(icon(Icon::Search, 15.0, tokens.text_muted))
    .child(
        text_input(
            TextRole::Detail,
            tokens.text,
            tokens.text_muted,
            model.member_search_signal(),
        )
        .id("member-search-input")
        .placeholder("Search members")
        .caret_color(tokens.accent)
        .selection_color(tokens.accent_line)
        .flex(1.0),
    )
}

fn group_header(tokens: &Tokens, group: &MemberGroup, show_count: bool) -> Row {
    let mut header = Row::new()
        .width(FILL)
        .padding_custom(Padding::new().top(12.0).right(8.0).bottom(4.0).left(8.0))
        .align_items(Alignment::Center)
        .justify(Justify::SpaceBetween)
        .child(text(TextRole::Control, tokens.text_muted, &group.title));
    if let Some(count) = group.count.filter(|_| show_count) {
        header = header.child(text(
            TextRole::Control,
            tokens.text_muted,
            &count.to_string(),
        ));
    }
    header
}

fn member_row(tokens: &Tokens, member: &Member) -> Row {
    let offline = member.presence == Presence::Offline;
    let (name_color, status_color) = if member.speaking {
        (tokens.text, tokens.accent)
    } else if offline {
        (tokens.text_muted, tokens.text_offline)
    } else {
        (tokens.text, tokens.text_muted)
    };
    // The speaking ring takes the place of the presence dot.
    let look = AvatarLook::plain(AVATAR)
        .speaking(member.speaking)
        .dimmed(offline);
    let look = if member.speaking {
        look
    } else {
        look.presence(member.presence)
    };
    let hover = tokens.row_hover.fill;
    let mut row = Row::new()
        .id(format!("member-{}", member.user.0))
        .class("member")
        .width(FILL)
        .height(ROW)
        .padding_horizontal(8.0)
        .spacing(10.0)
        .align_items(Alignment::Center)
        .rounded(RADIUS_CONTROL)
        .hovered(move |style| style.background(hover))
        .child(avatar(tokens, &member.avatar, look))
        .child(
            Column::new()
                .flex(1.0)
                .child(text(TextRole::Label, name_color, &member.name))
                .child(text(TextRole::Caption, status_color, &member.status)),
        );
    if member.speaking {
        row = row.class("speaking");
    }
    row
}

fn show_more(tokens: &Tokens, hidden: u32) -> Row {
    let hover = tokens.row_hover.fill;
    Row::new()
        .id("members-show-more")
        .width(FILL)
        .height(32.0)
        .padding_horizontal(8.0)
        .spacing(6.0)
        .align_items(Alignment::Center)
        .rounded(RADIUS_CONTROL)
        .cursor(CursorIcon::Pointer)
        .hovered(move |style| style.background(hover))
        .child(text(
            TextRole::Control,
            tokens.text_secondary,
            &format!("Show {hidden} more"),
        ))
        .child(icon(Icon::CaretDown, 11.0, tokens.text_muted))
}
