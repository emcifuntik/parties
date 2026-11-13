//! The user's own pages of Settings: Profile (picture, display name, the
//! names in each party) and Keys (the fingerprint and the keys that sign the
//! user in).
//!
//! The flows these pages start (pick and crop a picture, rename in a party,
//! add a passkey or a hardware key, show the recovery phrase) are not built
//! yet: their buttons send their commands and nothing opens.

use lurq::{
    app::events::KeyboardEvent,
    components::{Column, Rect, Row, Spacer, Stack},
    layout::Alignment,
    node::{CursorIcon, Element},
};

use super::{
    avatar::{AvatarLook, avatar},
    icon::{Icon, icon},
    icon_button,
    settings::{CARD_PADDING, card},
    settings_voice::{caption, field_label, icon_text_button, switch_row},
    surface::{FILL, glass, surfaced},
};
use crate::{
    commands::Command,
    model::{
        IdentityCard, KeyEntry, KeyKind, KeyStorage, Keys, PartyIcon, PartyName, Profile, SelfState,
    },
    theme::{
        Surface, TextRole, Tokens,
        tokens::{RADIUS_CHIP, RADIUS_CONTROL},
        typography::{text, text_input},
    },
    view_model::ShellModel,
};

/// Space between the two columns of a card.
const COLUMN_GAP: f32 = 48.0;
/// Height of a field label, and from it to the field under it.
const LABEL: f32 = 16.0;
const LABEL_GAP: f32 = 9.0;
/// Height of a field and of the button next to one.
const FIELD: f32 = 44.0;
/// Height of a button inside a card, and of one next to a card's title.
const BUTTON: f32 = 36.0;
const HEAD_BUTTON: f32 = 34.0;
/// Side of the user's picture on the Profile page, and the card around it.
const PICTURE: f32 = 72.0;
const PICTURE_CARD: f32 = PICTURE + 2.0 * CARD_PADDING;
/// Height of a row of "Names in your parties" and of "Your keys".
const NAME_ROW: f32 = 48.0;
const KEY_ROW: f32 = 60.0;
/// Height of the notice under the hardware key switch, and the space above.
const NOTICE: f32 = 38.0;
const NOTICE_GAP: f32 = 16.0;

const fn party_icon(icon: PartyIcon) -> Icon {
    match icon {
        PartyIcon::Moon => Icon::PartyMoon,
        PartyIcon::Film => Icon::PartyFilm,
        PartyIcon::Shield => Icon::PartyShield,
        PartyIcon::Server => Icon::PartyServer,
    }
}

/// A plain button inside a card.
fn button(tokens: &Tokens, label: &str) -> Row {
    let hover = tokens.control_hover.fill;
    surfaced!(
        Row::new()
            .height(BUTTON)
            .padding_horizontal(12.0)
            .align_items(Alignment::Center)
            .cursor(CursorIcon::Pointer)
            .hovered(move |style| style.background(hover)),
        tokens.control
    )
    .child(text(TextRole::Label, tokens.text, label))
}

/// A list inside a card: rows of `row_height` with a hairline between them.
fn boxed_list(tokens: &Tokens, id: &'static str, rows: Vec<Row>) -> Column {
    let mut list = surfaced!(Column::new().id(id).width(FILL), tokens.card);
    for (index, row) in rows.into_iter().enumerate() {
        if index > 0 {
            list = list.child(Rect::new(FILL, 1.0).background(tokens.hairline));
        }
        list = list.child(row);
    }
    list
}

/// The height of a [`boxed_list`] of `rows` rows.
fn boxed_list_height(rows: usize, row_height: f32) -> f32 {
    rows as f32 * row_height + rows.saturating_sub(1) as f32
}

// --- Profile ---

/// The cards of the Profile page, top to bottom.
pub(crate) fn profile_cards(tokens: &Tokens, model: &ShellModel) -> [Stack; 2] {
    let profile = model.profile();
    [
        picture_card(tokens, model, &model.me(), &profile),
        name_card(tokens, model, &profile),
    ]
}

/// The picture, what is known about its file, and the two things to do
/// with it.
fn picture_card(tokens: &Tokens, model: &ShellModel, me: &SelfState, profile: &Profile) -> Stack {
    let mut about =
        Column::new()
            .spacing(4.0)
            .child(text(TextRole::HeadingPage, tokens.text, "Picture"));
    let mut buttons = Row::new().spacing(8.0).child(
        icon_text_button(tokens, Icon::Image, "Change picture")
            .id("profile-change-picture")
            .height(BUTTON)
            .on_click(model.command(Command::ChangePicture)),
    );
    match &profile.picture {
        Some(file) => {
            about = about
                .child(text(
                    TextRole::Detail,
                    tokens.text_muted,
                    "Stored on this device. A party asks for it only when it has changed.",
                ))
                .child(text(TextRole::Code, tokens.text_muted, file).id("profile-picture-file"));
            buttons = buttons.child(
                button(tokens, "Remove")
                    .id("profile-remove-picture")
                    .on_click(model.command(Command::RemovePicture)),
            );
        }
        None => {
            about = about.child(
                text(
                    TextRole::Detail,
                    tokens.text_muted,
                    "No picture. Parties show your initials.",
                )
                .id("profile-no-picture"),
            );
        }
    }
    glass(
        &tokens.panel_reading,
        Row::new()
            .width(FILL)
            .padding(CARD_PADDING)
            .spacing(20.0)
            .align_items(Alignment::Center)
            .child(avatar(tokens, &me.avatar, AvatarLook::plain(PICTURE)))
            .child(about)
            .child(Spacer::new().flex(1.0))
            .child(buttons),
    )
    .id("settings-picture")
    .width(FILL)
    .height(PICTURE_CARD)
}

/// One party and the name the user goes by in it.
fn party_name_row(tokens: &Tokens, model: &ShellModel, profile: &Profile, name: &PartyName) -> Row {
    let (tile, tile_icon) = if name.connected {
        (tokens.tile_party, tokens.accent)
    } else {
        (tokens.card, tokens.text_secondary)
    };
    let (kind, value) = match &name.nickname {
        Some(nickname) => ("Nickname", nickname.as_str()),
        None => ("Display name", profile.display_name.as_str()),
    };
    Row::new()
        .id(format!("profile-party-{}", name.party.0))
        .width(FILL)
        .height(NAME_ROW)
        .padding_left(12.0)
        .padding_right(8.0)
        .spacing(12.0)
        .align_items(Alignment::Center)
        .child(
            surfaced!(
                Row::new()
                    .size(28.0, 28.0)
                    .align_items(Alignment::Center)
                    .justify(lurq::layout::layout_kind::Justify::Center),
                Surface {
                    radius: RADIUS_CHIP,
                    ..tile
                }
            )
            .child(icon(party_icon(name.icon), 14.0, tile_icon)),
        )
        .child(text(TextRole::Label, tokens.text, &name.party_name))
        .child(Spacer::new().flex(1.0))
        .child(text(TextRole::Caption, tokens.text_muted, kind))
        .child(
            text(TextRole::Label, tokens.text, value)
                .id(format!("profile-party-{}-name", name.party.0)),
        )
        .child(
            icon_button(
                Icon::Edit,
                14.0,
                tokens.text_secondary,
                (28.0, 28.0),
                RADIUS_CHIP,
                tokens.row_hover.fill,
            )
            .id(format!("profile-party-{}-edit", name.party.0))
            .on_click(model.command(Command::EditPartyName(name.party))),
        )
}

/// "Name": the display name, and the names in each party.
fn name_card(tokens: &Tokens, model: &ShellModel, profile: &Profile) -> Stack {
    let commit = model.clone();
    let input = surfaced!(
        Row::new()
            .id("profile-display-name")
            .width(FILL)
            .height(FIELD)
            .padding_horizontal(12.0)
            .align_items(Alignment::Center),
        tokens.field
    )
    .child(
        text_input(
            TextRole::Body,
            tokens.text,
            tokens.text_muted,
            model.display_name_signal(),
        )
        .id("profile-display-name-input")
        .placeholder("Your name")
        .caret_color(tokens.accent)
        .selection_color(tokens.accent_line)
        .flex(1.0)
        .on_key_down(move |event: KeyboardEvent| {
            if event.key == "Enter" {
                event.prevent_default();
                commit.dispatch(Command::CommitDisplayName);
            }
        }),
    );
    let display_name = Column::new()
        .flex(1.0)
        .child(field_label(tokens, "Display name"))
        .child(Spacer::new().height(LABEL_GAP))
        .child(input)
        .child(Spacer::new().height(8.0))
        .child(caption(
            tokens.text_muted,
            "Tip: double-click your own name anywhere in the app to rename yourself.",
        ));

    let rows: Vec<Row> = profile
        .party_names
        .iter()
        .map(|name| party_name_row(tokens, model, profile, name))
        .collect();
    let list_height = boxed_list_height(rows.len(), NAME_ROW);
    let names = Column::new()
        .flex(1.0)
        .child(field_label(tokens, "Names in your parties"))
        .child(Spacer::new().height(LABEL_GAP))
        .child(boxed_list(tokens, "profile-party-names", rows));

    let body = (LABEL + LABEL_GAP + FIELD + 8.0 + LABEL).max(LABEL + LABEL_GAP + list_height);
    card(
        tokens,
        "settings-name",
        "Name",
        None,
        None,
        body,
        Row::new()
            .width(FILL)
            .spacing(COLUMN_GAP)
            .child(display_name)
            .child(names),
    )
}

// --- Keys ---

/// The cards of the Keys page, top to bottom.
pub(crate) fn keys_cards(tokens: &Tokens, model: &ShellModel) -> [Stack; 2] {
    [
        identity_card(tokens, model, &model.identity()),
        keys_card(tokens, model, &model.keys()),
    ]
}

const fn storage_icon(storage: KeyStorage) -> Icon {
    match storage {
        KeyStorage::Device => Icon::Chip,
        KeyStorage::HardwareKey => Icon::Key,
        KeyStorage::Passkey => Icon::Fingerprint,
    }
}

/// "Your identity": the fingerprint, and where the key is kept.
fn identity_card(tokens: &Tokens, model: &ShellModel, identity: &IdentityCard) -> Stack {
    let hover = tokens.control_hover.fill;
    let fingerprint = Column::new()
        .flex(1.0)
        .child(field_label(tokens, "Fingerprint"))
        .child(Spacer::new().height(LABEL_GAP))
        .child(
            Row::new()
                .width(FILL)
                .spacing(8.0)
                .child(
                    surfaced!(
                        Row::new()
                            .flex(1.0)
                            .height(FIELD)
                            .padding_horizontal(13.0)
                            .align_items(Alignment::Center),
                        tokens.field
                    )
                    .child(
                        text(TextRole::CodeBody, tokens.text, &identity.fingerprint)
                            .id("keys-fingerprint"),
                    ),
                )
                .child(surfaced!(
                    icon_button(
                        Icon::Copy,
                        16.0,
                        tokens.text,
                        (FIELD, FIELD),
                        RADIUS_CONTROL,
                        hover,
                    )
                    .id("keys-copy-fingerprint")
                    .on_click(model.command(Command::CopyFingerprint)),
                    tokens.control
                )),
        );
    let storage = Column::new()
        .flex(1.0)
        .child(field_label(tokens, "Storage"))
        .child(Spacer::new().height(LABEL_GAP))
        .child(
            Row::new()
                .width(FILL)
                .height(FIELD)
                .spacing(10.0)
                .align_items(Alignment::Center)
                .child(icon(storage_icon(identity.key), 16.0, tokens.text_muted))
                .child(text(TextRole::Body, tokens.text, identity.key.label()).id("keys-storage"))
                .child(Spacer::new().flex(1.0))
                .child(
                    button(tokens, "Show recovery phrase")
                        .id("keys-show-recovery-phrase")
                        .on_click(model.command(Command::ShowRecoveryPhrase)),
                ),
        );
    card(
        tokens,
        "settings-identity",
        "Your identity",
        Some(
            "Each party knows you by this fingerprint. Compare it with a friend to check it is really you.",
        ),
        None,
        LABEL + LABEL_GAP + FIELD,
        Row::new()
            .width(FILL)
            .spacing(COLUMN_GAP)
            .child(fingerprint)
            .child(storage),
    )
}

/// One key: what it is, when it was added, and the button that removes it.
fn key_row(tokens: &Tokens, model: &ShellModel, key: &KeyEntry) -> Row {
    let glyph = match key.kind {
        KeyKind::Passkey => Icon::Fingerprint,
        KeyKind::HardwareKey => Icon::Key,
    };
    Row::new()
        .id(format!("key-{}", key.id))
        .width(FILL)
        .height(KEY_ROW)
        .padding_horizontal(12.0)
        .spacing(12.0)
        .align_items(Alignment::Center)
        .child(
            surfaced!(
                Row::new()
                    .size(32.0, 32.0)
                    .align_items(Alignment::Center)
                    .justify(lurq::layout::layout_kind::Justify::Center),
                Surface {
                    radius: RADIUS_CHIP,
                    ..tokens.card
                }
            )
            .child(icon(glyph, 16.0, tokens.text_secondary)),
        )
        .child(
            Column::new()
                .flex(1.0)
                .spacing(3.0)
                .child(text(TextRole::Label, tokens.text, &key.name))
                .child(text(TextRole::Caption, tokens.text_muted, &key.detail)),
        )
        .child(
            button(tokens, "Remove")
                .id(format!("key-{}-remove", key.id))
                .on_click(model.command(Command::RemoveKey(key.id))),
        )
}

/// "Your keys": the keys, the ways to add one, and the hardware key rule.
fn keys_card(tokens: &Tokens, model: &ShellModel, keys: &Keys) -> Stack {
    let add: Element = Row::new()
        .spacing(8.0)
        .child(
            // As tall as the line of the card's title, which clips them.
            Row::new()
                .id("keys-add-passkey")
                .height(HEAD_BUTTON)
                .padding_horizontal(12.0)
                .spacing(8.0)
                .align_items(Alignment::Center)
                .rounded(RADIUS_CONTROL)
                .background(tokens.accent)
                .cursor(CursorIcon::Pointer)
                .on_click(model.command(Command::AddPasskey))
                .child(icon(Icon::Plus, 14.0, tokens.on_accent))
                .child(text(
                    TextRole::LabelStrong,
                    tokens.on_accent,
                    "Add a passkey",
                )),
        )
        .child(
            icon_text_button(tokens, Icon::Plus, "Add a hardware key")
                .id("keys-add-hardware-key")
                .height(HEAD_BUTTON)
                .on_click(model.command(Command::AddHardwareKey)),
        )
        .into();

    let rows: Vec<Row> = keys
        .keys
        .iter()
        .map(|key| key_row(tokens, model, key))
        .collect();
    let list_height = boxed_list_height(rows.len(), KEY_ROW);
    let rule = Column::new()
        .flex(1.0)
        .child(switch_row(
            tokens,
            model,
            "keys-always-hardware-key",
            (
                "Always ask for a hardware key on this device",
                "Passkeys and the key on this PC are then never used to sign in.",
            ),
            keys.always_ask_hardware_key,
            Command::SetAlwaysAskHardwareKey(!keys.always_ask_hardware_key),
        ))
        .child(Spacer::new().height(NOTICE_GAP))
        .child(
            surfaced!(
                Row::new()
                    .width(FILL)
                    .height(NOTICE)
                    .padding_horizontal(12.0)
                    .spacing(10.0)
                    .align_items(Alignment::Center),
                tokens.card
            )
            .child(icon(Icon::Info, 14.0, tokens.text_secondary))
            .child(text(
                TextRole::Caption,
                tokens.text_secondary,
                "Keep at least two keys or a recovery phrase. Losing your only key locks you out.",
            )),
        );

    card(
        tokens,
        "settings-keys",
        "Your keys",
        Some(
            "Any of these keys signs you in. A new key is linked to each party the next time you connect.",
        ),
        Some(add),
        list_height.max(36.0 + NOTICE_GAP + NOTICE),
        Row::new()
            .width(FILL)
            .spacing(COLUMN_GAP)
            .child(
                Column::new()
                    .flex(1.0)
                    .child(boxed_list(tokens, "keys-list", rows)),
            )
            .child(rule),
    )
}
