//! The block at the bottom of the left panel: the mini profile (picture
//! with presence, name, the room the user is in, latency), the call
//! controls (Mic, Sound, Leave) and the Stream button.
//!
//! Two menus open upwards from it and are never open together: the
//! presence menu (from the profile line) and the Stream menu (from the
//! Stream button). They live in [`super::voice_menus`].

use lurq::{
    app::{component::Component, ctx::Ctx},
    components::{Column, Placement, Popup, Row},
    core::ElementRef,
    layout::{Alignment, layout_kind::Justify},
    node::{CursorIcon, Element, color::Color},
};

use super::{
    avatar::{AvatarLook, avatar},
    icon::{Icon, icon},
    surface::{FILL, surfaced},
    tokens,
    voice_menus::{presence_menu, stream_menu},
};
use crate::{
    commands::Command,
    model::{SelfState, StreamSource},
    theme::{Surface, TextRole, Tokens, typography::text},
    view_model::ShellModel,
};

/// Height of a button of the block.
const BUTTON: f32 = 34.0;
/// Padding inside the block.
const PADDING: f32 = 10.0;
/// Space between the block (or the Stream button) and a menu above it.
const MENU_GAP: f32 = 6.0;
/// Opacity of a control that has nothing to act on (no room joined).
const INERT: f32 = 0.4;

pub(crate) struct VoiceBlock {
    /// The profile line: trigger and anchor of the presence menu.
    profile: ElementRef,
    /// The Stream button: trigger and anchor of the Stream menu.
    stream: ElementRef,
}

impl Component for VoiceBlock {
    type Props = ShellModel;

    fn create(_ctx: &mut Ctx) -> Self {
        Self {
            profile: ElementRef::new(),
            stream: ElementRef::new(),
        }
    }

    fn render(&self, ctx: &mut Ctx) -> impl Into<Element> {
        let tokens = tokens(ctx);
        let model = ctx.props::<ShellModel>().clone();
        let me = model.me();

        surfaced!(
            Column::new()
                .id("voice-block")
                .width(FILL)
                .padding(PADDING)
                .spacing(10.0),
            tokens.block
        )
        .child(self.profile_line(&tokens, &model, &me))
        .child(call_controls(&tokens, &model, &me))
        .child(self.stream_button(&tokens, &model, &me))
        // The profile line sits `PADDING` inside the block; the menu is as
        // wide as the block and starts `MENU_GAP` above it.
        .child(
            Popup::new(
                self.profile.clone(),
                presence_menu(&tokens, &model, me.presence),
            )
            .open_signal(model.presence_menu_signal())
            .placement(Placement::TopStart)
            .offset(-PADDING, PADDING + MENU_GAP),
        )
        .child(
            Popup::new(
                self.stream.clone(),
                stream_menu(&tokens, &model, me.streams),
            )
            .open_signal(model.stream_menu_signal())
            .placement(Placement::Top)
            .match_anchor_width(true)
            .offset(0.0, MENU_GAP),
        )
    }
}

impl VoiceBlock {
    fn profile_line(&self, tokens: &Tokens, model: &ShellModel, me: &SelfState) -> Row {
        let place = match &me.room {
            Some(room) => format!("In {}", room.name),
            None => me.presence.label().to_owned(),
        };
        let mut line = Row::new()
            .width(FILL)
            .spacing(8.0)
            .align_items(Alignment::Center)
            .child(
                Row::new()
                    .id("profile-line")
                    .flex(1.0)
                    .spacing(8.0)
                    .align_items(Alignment::Center)
                    .cursor(CursorIcon::Pointer)
                    // Focusable, so lurq takes it for a menu trigger: a press on
                    // it while the other menu is open switches menus at once.
                    .focusable(true)
                    .ref_element(self.profile.clone())
                    .on_click(model.command(Command::TogglePresenceMenu))
                    .child(avatar(
                        tokens,
                        &me.avatar,
                        AvatarLook::plain(34.0).presence(me.presence),
                    ))
                    .child(
                        Column::new()
                            .child(text(TextRole::HeadingSmall, tokens.text, &me.name))
                            .child(text(TextRole::Caption, tokens.text_muted, &place)),
                    ),
            );
        if let Some(latency) = me.latency_ms {
            line = line.child(
                Row::new()
                    .spacing(6.0)
                    .align_items(Alignment::Center)
                    .child(icon(Icon::Signal, 16.0, tokens.online))
                    .child(text(
                        TextRole::Control,
                        tokens.text_secondary,
                        &format!("{latency} ms"),
                    )),
            );
        }
        line
    }

    fn stream_button(&self, tokens: &Tokens, model: &ShellModel, me: &SelfState) -> Row {
        let live: Vec<StreamSource> = StreamSource::ALL
            .into_iter()
            .filter(|source| me.streams.is_live(*source))
            .collect();
        let label = match live.as_slice() {
            [] => "Stream".to_owned(),
            [source] => format!("{} is live", source.label()),
            many => format!("{} streams live", many.len()),
        };
        let (surface, icon_color, label_color) = if live.is_empty() {
            (tokens.control, tokens.text, tokens.text)
        } else {
            (tokens.control_lit, tokens.accent, tokens.accent_text)
        };
        let button = button(tokens, surface, live.is_empty())
            .id("stream-button")
            .width(FILL)
            .spacing(6.0)
            .ref_element(self.stream.clone())
            .child(icon(Icon::Stream, 16.0, icon_color))
            .child(text(TextRole::Control, label_color, &label))
            .child(icon(Icon::CaretUp, 11.0, tokens.text_muted));
        if me.room.is_some() {
            // Focusable for the same reason as the profile line.
            button
                .focusable(true)
                .on_click(model.command(Command::ToggleStreamMenu))
        } else {
            button.opacity(INERT).cursor(CursorIcon::Default)
        }
    }
}

/// A labelled button of the block. `hover` adds the neutral hover fill;
/// tinted buttons keep their tint.
fn button(tokens: &Tokens, surface: Surface, hover: bool) -> Row {
    let row = surfaced!(
        Row::new()
            .height(BUTTON)
            .align_items(Alignment::Center)
            .justify(Justify::Center)
            .cursor(CursorIcon::Pointer),
        surface
    );
    if hover {
        let fill = tokens.control_hover.fill;
        row.hovered(move |style| style.background(fill))
    } else {
        row
    }
}

fn call_controls(tokens: &Tokens, model: &ShellModel, me: &SelfState) -> Row {
    let toggle =
        |id: &'static str, off: bool, glyphs: (Icon, Icon), label: &str, command: Command| {
            let (surface, glyph, icon_color, label_color): (Surface, Icon, Color, Color) = if off {
                (
                    tokens.control_danger,
                    glyphs.1,
                    tokens.danger_icon,
                    tokens.danger_label,
                )
            } else {
                (tokens.control, glyphs.0, tokens.text, tokens.text)
            };
            let mut control = button(tokens, surface, !off)
                .id(id)
                .flex(1.0)
                .spacing(5.0)
                .on_click(model.command(command))
                .child(icon(glyph, 16.0, icon_color))
                .child(text(TextRole::Control, label_color, label));
            if off {
                control = control.class("off");
            }
            control
        };

    let leave = button(tokens, tokens.control_danger, false)
        .id("leave-button")
        .flex(1.0)
        .spacing(5.0)
        .child(icon(Icon::Leave, 16.0, tokens.danger_icon))
        .child(text(TextRole::Control, tokens.danger_label, "Leave"));
    let leave = if me.room.is_some() {
        leave.on_click(model.command(Command::LeaveRoom))
    } else {
        leave.opacity(INERT).cursor(CursorIcon::Default)
    };

    Row::new()
        .width(FILL)
        .spacing(6.0)
        .child(toggle(
            "mic-button",
            me.muted,
            (Icon::Mic, Icon::MicOff),
            "Mic",
            Command::ToggleMute,
        ))
        .child(toggle(
            "sound-button",
            me.deafened,
            (Icon::Headphones, Icon::SpeakerOff),
            "Sound",
            Command::ToggleDeafen,
        ))
        .child(leave)
}
