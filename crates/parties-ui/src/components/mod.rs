//! The views of the main window, split the way the design is split.
//!
//! - [`shell`]: the root; backdrop, window chrome and the view in it.
//! - [`title_bar`]: brand, switcher, update notice, window buttons.
//! - [`home`]: the Home view; the user's parties, identity, "Add a party".
//! - [`settings`]: the Settings view; its navigation, the Appearance page
//!   and what its pages are built from. The other pages: [`settings_account`]
//!   (Profile and Keys), [`settings_voice`], [`settings_video`],
//!   [`settings_hotkeys`], [`settings_app`].
//! - [`channel_list`]: party header, voice rooms, text channels.
//! - [`voice_block`] and [`voice_menus`]: the profile and voice block with
//!   its Stream and presence menus.
//! - [`chat_panel`], [`message_list`], [`composer`]: the chat.
//! - [`member_list`]: the members panel.
//! - [`surface`], [`avatar`], [`icon`]: the pieces they are built from.
//!
//! Views hold no state of their own beyond element references: everything
//! they show comes from [`ShellModel`](crate::view_model::ShellModel) and
//! everything the user does leaves as a
//! [`Command`](crate::commands::Command).

pub mod avatar;
pub mod channel_list;
pub mod chat_panel;
pub mod composer;
pub mod home;
pub mod icon;
pub mod member_list;
pub mod message_list;
pub mod settings;
pub mod settings_account;
pub mod settings_app;
pub mod settings_hotkeys;
pub mod settings_video;
pub mod settings_voice;
pub mod shell;
pub mod surface;
pub mod title_bar;
pub mod voice_block;
pub mod voice_menus;

use lurq::{
    app::{ctx::Ctx, events::MouseEvent},
    components::Row,
    layout::{Alignment, layout_kind::Justify},
    node::{CursorIcon, color::Color},
};

use self::icon::{Icon, icon};
use crate::theme::{LightId, Tokens};

/// The tokens the shell provided for the current light. Reading them
/// subscribes the component, so it renders again when the light changes.
pub(crate) fn tokens(ctx: &mut Ctx) -> Tokens {
    ctx.use_context::<Tokens>()
        .unwrap_or_else(|| Tokens::for_light(LightId::default()))
}

/// Keeps a press on a control inside the title bar from starting a window
/// drag: the title bar listens for presses on everything it contains.
pub(crate) fn swallow_press(event: MouseEvent) {
    event.prevent_default();
    event.stop_immediate_propagation();
}

/// A square or rectangular button that shows one icon, with a hover fill.
pub(crate) fn icon_button(
    glyph: Icon,
    icon_size: f32,
    color: Color,
    (width, height): (f32, f32),
    radius: f32,
    hover: Color,
) -> Row {
    Row::new()
        .size(width, height)
        .align_items(Alignment::Center)
        .justify(Justify::Center)
        .rounded(radius)
        .cursor(CursorIcon::Pointer)
        .hovered(move |style| style.background(hover))
        .child(icon(glyph, icon_size, color))
}
