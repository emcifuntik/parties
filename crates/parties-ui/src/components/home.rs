//! Home: "Your parties" with the state of each, the user's identity card and
//! the "Add a party" card.
//!
//! The page has no panel of its own: a title on the backdrop, then one glass
//! card with the parties and a column of two cards next to it. It is as wide
//! as the design draws it (1136 px) and centred; a narrower window takes the
//! width from the list of parties.

use lurq::{
    app::{component::Component, ctx::Ctx},
    components::{Column, Rect, Row, ScrollVertical, Spacer, Stack},
    layout::{Alignment, layout_kind::Justify},
    node::{CursorIcon, Element, color::Color, padding::Padding},
};

use super::{
    avatar::{AvatarLook, avatar},
    icon::{Icon, icon},
    icon_button,
    surface::{FILL, glass, scrollbar, surfaced},
    tokens,
};
use crate::{
    commands::Command,
    model::{Avatar, IdentityCard, PartyCard, PartyIcon, PartyLink, RoomPreview, SelfState, View},
    theme::{
        Surface, TextRole, Tokens,
        metrics::PANEL_GAP,
        tokens::{RADIUS_CHIP, RADIUS_CONTROL},
        typography::{paragraph, text, text_input},
    },
    view_model::ShellModel,
};

/// Width of the page: the list, the gap and the side column.
const PAGE_WIDTH: f32 = 1136.0;
/// Width of the column with the identity card and "Add a party".
const SIDE_WIDTH: f32 = 380.0;
/// From the title bar to the title.
const TITLE_TOP: f32 = 80.0;
/// Padding inside the two side cards.
const CARD_PADDING: f32 = 20.0;
/// Heights of the two side cards, as the design draws them. A glass surface
/// cannot take its height from its content (see `surface::glass`), and both
/// cards hold fixed text at a fixed width.
const IDENTITY_HEIGHT: f32 = 206.0;
const ADD_PARTY_HEIGHT: f32 = 232.0;
/// Padding inside the list card, around the rows.
const LIST_PADDING: f32 = 8.0;
/// Padding inside a party row.
const ROW_PADDING: f32 = 14.0;
/// Side of a party's tile.
const TILE: f32 = 44.0;
/// Width of the state column of a row (dot, state, detail).
const STATE_WIDTH: f32 = 182.0;
/// Size of the button at the end of a row; rows without one keep the room.
const ROW_BUTTON: (f32, f32) = (70.0, 28.0);
/// Height of a room line in the row of the connected party.
const ROOM_LINE: f32 = 28.0;
/// Side of a picture in a room line, and how far the next one is shifted.
const ROOM_AVATAR: f32 = 20.0;
const ROOM_AVATAR_STEP: f32 = 15.0;
/// Width of the room name and of the pictures in a room line.
const ROOM_NAME_WIDTH: f32 = 84.0;
const ROOM_PICTURES_WIDTH: f32 = 88.0;
/// Side of the user's picture on the identity card.
const PROFILE_AVATAR: f32 = 52.0;

pub(crate) struct Home;

impl Component for Home {
    type Props = ShellModel;

    fn create(_ctx: &mut Ctx) -> Self {
        Self
    }

    fn render(&self, ctx: &mut Ctx) -> impl Into<Element> {
        let tokens = tokens(ctx);
        let model = ctx.props::<ShellModel>().clone();
        let parties = model.home_parties();
        let online = parties
            .iter()
            .filter(|party| party.link.is_online())
            .count();

        let mut list = Column::new()
            .width(FILL)
            .height(FILL)
            .padding(LIST_PADDING)
            .spacing(4.0);
        for party in &parties {
            list = list.child(party_row(&tokens, &model, party));
        }

        let cards = Row::new()
            .width(FILL)
            .spacing(PANEL_GAP)
            .align_items(Alignment::Stretch)
            .child(glass(&tokens.panel, list).id("home-parties").flex(1.0))
            .child(
                Column::new()
                    .width(SIDE_WIDTH)
                    .spacing(PANEL_GAP)
                    .child(identity_card(
                        &tokens,
                        &model,
                        &model.me(),
                        &model.identity(),
                    ))
                    .child(add_party_card(&tokens, &model)),
            );

        let page = Column::new()
            .id("home")
            .flex(1.0)
            .max_width(PAGE_WIDTH)
            .padding_top(TITLE_TOP)
            .child(
                Column::new()
                    .padding_left(4.0)
                    .spacing(6.0)
                    .child(text(TextRole::Display, tokens.text, "Your parties").id("home-title"))
                    .child(
                        text(
                            TextRole::Lead,
                            tokens.text_secondary,
                            &format!("{online} of {} online", parties.len()),
                        )
                        .id("home-online"),
                    ),
            )
            .child(Spacer::new().height(28.0))
            .child(cards);

        ScrollVertical::new(
            Row::new()
                .width(FILL)
                .padding_custom(
                    Padding::new()
                        .right(PANEL_GAP)
                        .bottom(PANEL_GAP)
                        .left(PANEL_GAP),
                )
                .justify(Justify::Center)
                .child(page),
        )
        .scrollbar(scrollbar(&tokens))
        .width(FILL)
        .height(FILL)
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

/// A pill button at the end of a party row.
fn row_button(surface: Surface, label: &str, glyph: Icon, color: Color, label_color: Color) -> Row {
    surfaced!(
        Row::new()
            .size(ROW_BUTTON.0, ROW_BUTTON.1)
            .spacing(6.0)
            .align_items(Alignment::Center)
            .justify(Justify::Center)
            .cursor(CursorIcon::Pointer),
        Surface {
            radius: ROW_BUTTON.1 / 2.0,
            ..surface
        }
    )
    .child(text(TextRole::Label, label_color, label))
    .child(icon(glyph, 13.0, color))
}

/// One party: tile, name and address, state, and what can be done about it.
/// The party the client is connected to is a lit block that also lists its
/// voice rooms.
fn party_row(tokens: &Tokens, model: &ShellModel, party: &PartyCard) -> Column {
    let connected = party.link.is_connected();
    let offline = matches!(party.link, PartyLink::Offline { .. });
    let open = Command::ShowView(View::Party(party.id));

    let tile_surface = if connected {
        tokens.tile_party
    } else {
        tokens.card
    };
    let tile_icon = if connected {
        tokens.accent
    } else {
        tokens.text_secondary
    };
    let tile = surfaced!(
        Row::new()
            .size(TILE, TILE)
            .align_items(Alignment::Center)
            .justify(Justify::Center),
        tile_surface
    )
    .child(icon(party_icon(party.icon), 20.0, tile_icon));

    let name_color = if offline {
        tokens.text_secondary
    } else {
        tokens.text
    };
    let mut name_line = Row::new()
        .spacing(10.0)
        .align_items(Alignment::Center)
        .child(text(TextRole::Title, name_color, &party.name));
    if party.locked {
        name_line = name_line.child(
            Row::new()
                .spacing(5.0)
                .align_items(Alignment::Center)
                .child(icon(Icon::Lock, 12.0, tokens.text_muted))
                .child(text(TextRole::Caption, tokens.text_muted, "Private")),
        );
    }

    let button: Element = match party.link {
        PartyLink::Connected { .. } => row_button(
            tokens.control_lit,
            "Open",
            Icon::ArrowRight,
            tokens.accent_text,
            tokens.text,
        )
        .id(format!("party-open-{}", party.id.0))
        .on_click(model.command(open.clone()))
        .into(),
        PartyLink::Offline { .. } => {
            let hover = tokens.control_hover.fill;
            row_button(
                tokens.control,
                "Retry",
                Icon::Retry,
                tokens.text_muted,
                tokens.text,
            )
            .id(format!("party-retry-{}", party.id.0))
            .hovered(move |style| style.background(hover))
            .on_click(model.command(Command::RetryParty(party.id)))
            .into()
        }
        PartyLink::Reachable { .. } | PartyLink::Reconnecting { .. } => {
            Spacer::new().width(ROW_BUTTON.0).into()
        }
    };

    let header = Row::new()
        .width(FILL)
        .spacing(12.0)
        .align_items(Alignment::Center)
        .child(tile)
        .child(
            Column::new()
                .flex(1.0)
                .spacing(1.0)
                .child(name_line)
                .child(text(TextRole::Detail, tokens.text_muted, &party.address)),
        )
        .child(link_state(tokens, &party.link))
        .child(button);

    let mut row = Column::new()
        .id(format!("party-{}", party.id.0))
        .width(FILL)
        .padding(ROW_PADDING)
        .spacing(8.0)
        .child(header);
    if connected && !party.rooms.is_empty() {
        let mut rooms = Column::new()
            .width(FILL)
            // Under the name: past the tile and its gap, one pixel in.
            .padding_left(TILE + 13.0);
        for room in &party.rooms {
            rooms = rooms.child(room_line(tokens, room));
        }
        row = row.child(rooms);
    }

    if connected {
        surfaced!(row, tokens.block_lit)
            .class("connected")
            .cursor(CursorIcon::Pointer)
            .on_click(model.command(open))
    } else if party.link.is_online() {
        let hover = tokens.row_hover.fill;
        row.rounded(tokens.block_lit.radius)
            .cursor(CursorIcon::Pointer)
            .hovered(move |style| style.background(hover))
            .on_click(model.command(open))
    } else {
        row
    }
}

/// The state column of a party row: a dot, the state and one detail.
fn link_state(tokens: &Tokens, link: &PartyLink) -> Row {
    let (dot, state, state_color, detail) = match link {
        PartyLink::Connected {
            online,
            capacity,
            latency_ms,
        }
        | PartyLink::Reachable {
            online,
            capacity,
            latency_ms,
        } => (
            tokens.online,
            format!("{online} of {capacity} online"),
            tokens.text,
            format!("{latency_ms} ms"),
        ),
        PartyLink::Offline { last_seen } => (
            tokens.text_faint,
            "Offline".to_owned(),
            tokens.text_secondary,
            format!("Last seen {last_seen}"),
        ),
        PartyLink::Reconnecting { attempt } => (
            tokens.idle,
            "Reconnecting".to_owned(),
            tokens.text,
            format!("Attempt {attempt}"),
        ),
    };
    Row::new()
        .width(STATE_WIDTH)
        .spacing(8.0)
        .child(
            // The dot sits on the first line, not between the two.
            Row::new()
                .height(17.0)
                .align_items(Alignment::Center)
                .child(Rect::new(8.0, 8.0).rounded(4.0).background(dot)),
        )
        .child(
            Column::new()
                .spacing(1.0)
                .child(text(TextRole::Label, state_color, &state))
                .child(text(TextRole::Caption, tokens.text_muted, &detail)),
        )
}

/// A voice room of the connected party: name, the people's pictures, count.
fn room_line(tokens: &Tokens, room: &RoomPreview) -> Row {
    let people = if room.people == 1 {
        "1 person".to_owned()
    } else {
        format!("{} people", room.people)
    };
    Row::new()
        .width(FILL)
        .height(ROOM_LINE)
        .align_items(Alignment::Center)
        .child(icon(Icon::Speaker, 16.0, tokens.accent))
        .child(Spacer::new().width(5.0))
        .child(Row::new().width(ROOM_NAME_WIDTH).child(text(
            TextRole::Detail,
            tokens.text_secondary,
            &room.name,
        )))
        .child(Row::new().width(ROOM_PICTURES_WIDTH).child(pictures(
            tokens,
            &room.avatars,
            room.people,
        )))
        .child(text(TextRole::Caption, tokens.text_muted, &people))
}

/// The people of a room as overlapping pictures, with "+N" for the ones the
/// row has no picture of.
fn pictures(tokens: &Tokens, avatars: &[Avatar], people: u32) -> Stack {
    let more = people.saturating_sub(avatars.len() as u32);
    let slots = avatars.len() + usize::from(more > 0);
    let width = if slots == 0 {
        0.0
    } else {
        ROOM_AVATAR + ROOM_AVATAR_STEP * (slots - 1) as f32
    };
    let mut stack = Stack::new().size(width, ROOM_AVATAR).overflow_visible();
    for (index, person) in avatars.iter().enumerate() {
        let x = ROOM_AVATAR_STEP * index as f32;
        stack = stack.child(
            Stack::new()
                .absolute(x, 0.0, ROOM_AVATAR, ROOM_AVATAR)
                .child(avatar(tokens, person, AvatarLook::plain(ROOM_AVATAR))),
        );
    }
    if more > 0 {
        let x = ROOM_AVATAR_STEP * avatars.len() as f32;
        stack = stack.child(
            Row::new()
                .absolute(x, 0.0, ROOM_AVATAR, ROOM_AVATAR)
                .align_items(Alignment::Center)
                .justify(Justify::Center)
                .rounded(ROOM_AVATAR / 2.0)
                .background(tokens.presence_ring)
                .border_inside(1.0, tokens.monogram_border)
                .child(text(
                    TextRole::MicroStrong,
                    tokens.text_secondary,
                    &format!("+{more}"),
                )),
        );
    }
    stack
}

/// The user: picture, name and presence, and the key they sign in with.
fn identity_card(
    tokens: &Tokens,
    model: &ShellModel,
    me: &SelfState,
    identity: &IdentityCard,
) -> Stack {
    let content = Column::new()
        .width(FILL)
        .padding(CARD_PADDING)
        .child(
            Row::new()
                .spacing(12.0)
                .align_items(Alignment::Center)
                .child(avatar(
                    tokens,
                    &me.avatar,
                    AvatarLook::plain(PROFILE_AVATAR).presence(me.presence),
                ))
                .child(
                    Column::new()
                        .spacing(3.0)
                        .child(text(TextRole::HeadingPage, tokens.text, &me.name))
                        .child(text(
                            TextRole::Detail,
                            tokens.text_secondary,
                            me.presence.label(),
                        )),
                ),
        )
        .child(Spacer::new().height(16.0))
        .child(Rect::new(FILL, 1.0).background(tokens.hairline))
        .child(Spacer::new().height(20.0))
        .child(text(
            TextRole::Caption,
            tokens.text_muted,
            "Key fingerprint",
        ))
        .child(Spacer::new().height(8.0))
        .child(
            Row::new()
                .width(FILL)
                .height(20.0)
                .align_items(Alignment::Center)
                .child(
                    text(TextRole::CodeBody, tokens.text, &identity.fingerprint)
                        .id("home-fingerprint"),
                )
                .child(Spacer::new().flex(1.0))
                .child(
                    icon_button(
                        Icon::Copy,
                        14.0,
                        tokens.text_secondary,
                        (28.0, 28.0),
                        RADIUS_CHIP,
                        tokens.row_hover.fill,
                    )
                    .id("home-copy-fingerprint")
                    .on_click(model.command(Command::CopyFingerprint)),
                ),
        )
        .child(Spacer::new().height(16.0))
        .child(
            Row::new()
                .spacing(9.0)
                .align_items(Alignment::Center)
                .child(icon(Icon::Key, 14.0, tokens.text_muted))
                .child(
                    text(
                        TextRole::Detail,
                        tokens.text_secondary,
                        identity.key.label(),
                    )
                    .id("home-key"),
                ),
        );
    glass(&tokens.panel, content)
        .id("home-identity")
        .width(FILL)
        .height(IDENTITY_HEIGHT)
}

/// A button of the "Add a party" card.
fn card_button(surface: Surface, label: &str, label_color: Color) -> Row {
    surfaced!(
        Row::new()
            .flex(1.0)
            .height(36.0)
            .align_items(Alignment::Center)
            .justify(Justify::Center)
            .cursor(CursorIcon::Pointer),
        Surface {
            radius: RADIUS_CONTROL,
            ..surface
        }
    )
    .child(text(TextRole::LabelStrong, label_color, label))
}

/// "Add a party": the join code field and the two ways in.
fn add_party_card(tokens: &Tokens, model: &ShellModel) -> Stack {
    let field = surfaced!(
        Row::new()
            .id("home-join-code")
            .width(FILL)
            .height(40.0)
            .padding_horizontal(13.0)
            .spacing(11.0)
            .align_items(Alignment::Center),
        tokens.field
    )
    .child(icon(Icon::Ticket, 14.0, tokens.text_muted))
    .child(
        text_input(
            TextRole::CodeBody,
            tokens.text,
            tokens.text_faint,
            model.join_code_signal(),
        )
        .placeholder("K7QMD-4XR2P")
        .flex(1.0),
    );

    let hover = tokens.control_hover.fill;
    let content = Column::new()
        .width(FILL)
        .padding(CARD_PADDING)
        .child(text(TextRole::HeadingPage, tokens.text, "Add a party"))
        .child(Spacer::new().height(4.0))
        .child(
            paragraph(
                TextRole::Detail,
                tokens.text_muted,
                "Join codes are single-use. Ask someone in the party to create one for you.",
            )
            .width(FILL),
        )
        .child(Spacer::new().height(18.0))
        .child(text(TextRole::Control, tokens.text_secondary, "Join code"))
        .child(Spacer::new().height(8.0))
        .child(field)
        .child(Spacer::new().height(16.0))
        .child(
            Row::new()
                .width(FILL)
                .spacing(8.0)
                .child(
                    card_button(tokens.send, "Join with a code", tokens.on_accent)
                        .id("home-join")
                        .on_click(model.command(Command::JoinWithCode)),
                )
                .child(
                    card_button(tokens.control, "Add by address", tokens.text)
                        .id("home-add-by-address")
                        .hovered(move |style| style.background(hover))
                        .on_click(model.command(Command::AddPartyByAddress)),
                ),
        );
    glass(&tokens.panel, content)
        .id("home-add")
        .width(FILL)
        .height(ADD_PARTY_HEIGHT)
}
