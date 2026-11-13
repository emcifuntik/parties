//! The Voice and audio page of Settings: the microphone (device, level,
//! sensitivity, how it opens, what cleans it up) and the output (device,
//! volume).
//!
//! The two sliders edit their values in place, as text inputs do; everything
//! else leaves as a [`Command`]. The device fields show the device in use:
//! the list to pick another from is not built yet.

use lurq::{
    components::{Column, Rect, Row, Slider, Spacer, Stack},
    core::Signal,
    layout::{Alignment, layout_kind::Justify},
    node::{CursorIcon, Element, color::Color},
};

use super::{
    icon::{Icon, icon},
    settings::{card, switch},
    surface::{FILL, surfaced},
};
use crate::{
    commands::Command,
    model::{InputMode, VoiceSettings},
    theme::{Surface, TextRole, Tokens, tokens::RADIUS_CONTROL, typography::text},
    view_model::ShellModel,
};

/// Space between the two columns of a card.
const COLUMN_GAP: f32 = 48.0;
/// Height of a field label, and from it to the control under it.
const LABEL: f32 = 16.0;
const LABEL_GAP: f32 = 10.0;
/// Height of a device field.
const FIELD: f32 = 42.0;
/// The level meter: its bars and its height.
const METER_BARS: usize = 40;
const METER_HEIGHT: f32 = 44.0;
/// Height of a slider's row; the thumb is as tall.
const SLIDER: f32 = 16.0;
/// Height of an input mode card.
const MODE_CARD: f32 = 70.0;
/// Height of a row with a switch: two lines of text.
const SWITCH_ROW: f32 = 36.0;
/// Heights of the two cards' bodies: both columns of "Input" are as tall.
const INPUT_BODY: f32 = 320.0;
const OUTPUT_BODY: f32 = LABEL + LABEL_GAP + FIELD;

/// The cards of the page, top to bottom.
pub(crate) fn voice_cards(tokens: &Tokens, model: &ShellModel) -> [Stack; 2] {
    let voice = model.voice();
    [
        input_card(tokens, model, &voice),
        output_card(tokens, model, &voice),
    ]
}

/// A neutral button with an icon: in the head of a card, or at the end of a
/// row.
pub(crate) fn icon_text_button(tokens: &Tokens, glyph: Icon, label: &str) -> Row {
    let hover = tokens.control_hover.fill;
    surfaced!(
        Row::new()
            .height(34.0)
            .padding_horizontal(12.0)
            .spacing(7.0)
            .align_items(Alignment::Center)
            .cursor(CursorIcon::Pointer)
            .hovered(move |style| style.background(hover)),
        tokens.control
    )
    .child(icon(glyph, 13.0, tokens.text))
    .child(text(TextRole::Label, tokens.text, label))
}

pub(crate) fn field_label(tokens: &Tokens, label: &str) -> Row {
    Row::new()
        .height(LABEL)
        .align_items(Alignment::Center)
        .child(text(TextRole::Control, tokens.text_secondary, label))
}

/// A label on the left and a read-out on the right, on one line.
pub(crate) fn labelled(tokens: &Tokens, label: &str, read_out: impl Into<Element>) -> Row {
    Row::new()
        .width(FILL)
        .height(LABEL)
        .align_items(Alignment::Center)
        .child(text(TextRole::Control, tokens.text_secondary, label))
        .child(Spacer::new().flex(1.0))
        .child(read_out)
}

/// The value in use, drawn as the closed field of a list: a device here, a
/// sound or a language on other pages.
pub(crate) fn device_field(tokens: &Tokens, id: &'static str, glyph: Icon, device: &str) -> Row {
    surfaced!(
        Row::new()
            .id(id)
            .width(FILL)
            .height(FIELD)
            .padding_horizontal(14.0)
            .spacing(10.0)
            .align_items(Alignment::Center),
        tokens.field
    )
    .child(icon(glyph, 16.0, tokens.text_muted))
    .child(text(TextRole::Body, tokens.text, device))
    .child(Spacer::new().flex(1.0))
    .child(icon(Icon::CaretDown, 13.0, tokens.text_muted))
}

/// A slider over `value` from `min` to `max`.
pub(crate) fn slider(
    tokens: &Tokens,
    id: &'static str,
    value: Signal<i32>,
    (min, max): (i32, i32),
) -> Row {
    let (track, fill, thumb) = (tokens.switch_track, tokens.accent, tokens.slider_thumb);
    Row::new()
        .id(id)
        .width(FILL)
        .height(SLIDER)
        .align_items(Alignment::Center)
        .child(
            Slider::new(value)
                .range(min, max)
                .track(move |part| part.height(3.0).rounded(1.5).background(track))
                .fill(move |part| part.height(3.0).rounded(1.5).background(fill))
                .thumb(move |part| {
                    part.size(SLIDER, SLIDER)
                        .rounded(SLIDER / 2.0)
                        .background(thumb)
                })
                .width(FILL)
                .height(SLIDER),
        )
}

/// The microphone level as bars. The bars under the sensitivity are dim:
/// sound that quiet is not sent.
fn meter(tokens: &Tokens, voice: &VoiceSettings) -> Row {
    let bars = METER_BARS as f32;
    let lit = (voice.input_level.clamp(0.0, 1.0) * bars).round() as usize;
    let gate = (voice.sensitivity_position() * bars).round() as usize;
    let mut row = Row::new()
        .id("voice-meter")
        .width(FILL)
        .height(METER_HEIGHT)
        .spacing(4.0);
    for bar in 0..METER_BARS {
        let color = if bar >= lit {
            tokens.meter_off
        } else if bar < gate {
            tokens.meter_below
        } else {
            tokens.meter_above
        };
        row = row.child(
            Rect::new(FILL, FILL)
                .flex(1.0)
                .rounded(2.0)
                .background(color),
        );
    }
    row
}

/// The mark of a choice among a few: a ring, filled when it is the one taken.
pub(crate) fn radio_mark(tokens: &Tokens, current: bool) -> Element {
    if current {
        Row::new()
            .size(16.0, 16.0)
            .align_items(Alignment::Center)
            .justify(Justify::Center)
            .rounded(8.0)
            .border_inside(2.0, tokens.accent)
            .child(Rect::new(8.0, 8.0).rounded(4.0).background(tokens.accent))
            .into()
    } else {
        Rect::new(16.0, 16.0)
            .rounded(8.0)
            .border_inside(1.5, tokens.text_muted)
            .into()
    }
}

/// One way of opening the microphone, as a card with a radio mark.
fn mode_card(tokens: &Tokens, model: &ShellModel, mode: InputMode, current: bool) -> Row {
    let surface = if current {
        Surface {
            radius: RADIUS_CONTROL,
            ..tokens.block_lit
        }
    } else {
        tokens.card
    };
    let mark = radio_mark(tokens, current);
    let slug = match mode {
        InputMode::VoiceActivity => "voice-mode-activity",
        InputMode::PushToTalk => "voice-mode-push-to-talk",
    };
    let card = surfaced!(
        Row::new()
            .id(slug)
            .flex(1.0)
            .height(MODE_CARD)
            .padding(16.0)
            .spacing(10.0)
            .cursor(CursorIcon::Pointer)
            .on_click(model.command(Command::SetInputMode(mode))),
        surface
    )
    .child(mark)
    .child(
        Column::new()
            .spacing(3.0)
            .child(text(TextRole::Label, tokens.text, mode.label()))
            .child(text(TextRole::Caption, tokens.text_muted, mode.hint())),
    );
    if current { card.class("current") } else { card }
}

/// A setting that is on or off: its name, what it does, and the switch.
pub(crate) fn switch_row(
    tokens: &Tokens,
    model: &ShellModel,
    id: &'static str,
    (title, hint): (&str, &str),
    on: bool,
    command: Command,
) -> Row {
    let row = Row::new()
        .id(id)
        .width(FILL)
        .height(SWITCH_ROW)
        .align_items(Alignment::Center)
        .cursor(CursorIcon::Pointer)
        .on_click(model.command(command))
        .child(
            Column::new()
                .flex(1.0)
                .spacing(3.0)
                .child(text(TextRole::Label, tokens.text, title))
                .child(text(TextRole::Caption, tokens.text_muted, hint)),
        )
        .child(switch(tokens, on));
    if on { row.class("on") } else { row }
}

fn divider(tokens: &Tokens) -> Column {
    Column::new()
        .width(FILL)
        .child(Spacer::new().height(19.0))
        .child(Rect::new(FILL, 1.0).background(tokens.hairline))
        .child(Spacer::new().height(20.0))
}

pub(crate) fn caption(color: Color, content: &str) -> Row {
    Row::new()
        .height(LABEL)
        .align_items(Alignment::Center)
        .child(text(TextRole::Caption, color, content))
}

/// "Input": the microphone.
fn input_card(tokens: &Tokens, model: &ShellModel, voice: &VoiceSettings) -> Stack {
    let detected: Element = if voice.voice_detected() {
        Row::new()
            .id("voice-detected")
            .spacing(6.0)
            .align_items(Alignment::Center)
            .child(icon(Icon::Speaking, 14.0, tokens.accent))
            .child(text(
                TextRole::Control,
                tokens.accent_text,
                "Voice detected",
            ))
            .into()
    } else {
        Spacer::new().into()
    };
    let sensitivity = text(
        TextRole::Caption,
        tokens.text_muted,
        &format!("{} dB", voice.sensitivity_db),
    )
    .id("voice-sensitivity-value");

    let device_and_level = Column::new()
        .flex(1.0)
        .child(field_label(tokens, "Input device"))
        .child(Spacer::new().height(LABEL_GAP))
        .child(device_field(
            tokens,
            "voice-input-device",
            Icon::Mic,
            &voice.input_device,
        ))
        .child(Spacer::new().height(46.0))
        .child(labelled(tokens, "Input level", detected))
        .child(Spacer::new().height(8.0))
        .child(meter(tokens, voice))
        .child(Spacer::new().height(8.0))
        .child(
            Row::new()
                .width(FILL)
                .justify(Justify::SpaceBetween)
                .child(caption(
                    tokens.text_muted,
                    &format!("{} dB", VoiceSettings::METER_FLOOR_DB),
                ))
                .child(caption(tokens.text_muted, "0 dB")),
        )
        .child(Spacer::new().height(45.0))
        .child(labelled(tokens, "Sensitivity", sensitivity))
        .child(Spacer::new().height(11.0))
        .child(slider(
            tokens,
            "voice-sensitivity",
            model.sensitivity_signal(),
            (VoiceSettings::METER_FLOOR_DB, 0),
        ))
        .child(Spacer::new().height(10.0))
        .child(caption(
            tokens.text_muted,
            "Anything quieter than this is not sent.",
        ));

    let mut modes = Row::new().width(FILL).spacing(8.0);
    for mode in InputMode::ALL {
        modes = modes.child(mode_card(tokens, model, mode, mode == voice.input_mode));
    }
    let mode_and_cleanup = Column::new()
        .flex(1.0)
        .child(field_label(tokens, "Input mode"))
        .child(Spacer::new().height(LABEL_GAP))
        .child(modes)
        .child(Spacer::new().height(36.0))
        .child(switch_row(
            tokens,
            model,
            "voice-noise-suppression",
            (
                "Noise suppression",
                "Removes keyboard, fan and background noise.",
            ),
            voice.noise_suppression,
            Command::SetNoiseSuppression(!voice.noise_suppression),
        ))
        .child(divider(tokens))
        .child(switch_row(
            tokens,
            model,
            "voice-echo-cancellation",
            (
                "Echo cancellation",
                "Keeps your speakers from leaking back into the microphone.",
            ),
            voice.echo_cancellation,
            Command::SetEchoCancellation(!voice.echo_cancellation),
        ))
        .child(divider(tokens))
        .child(switch_row(
            tokens,
            model,
            "voice-automatic-gain",
            (
                "Automatic gain",
                "Evens out the volume of quiet and loud speech.",
            ),
            voice.automatic_gain,
            Command::SetAutomaticGain(!voice.automatic_gain),
        ));

    card(
        tokens,
        "settings-input",
        "Input",
        None,
        Some(
            icon_text_button(tokens, Icon::Play, "Test microphone")
                .id("voice-test-microphone")
                .on_click(model.command(Command::TestMicrophone))
                .into(),
        ),
        INPUT_BODY,
        Row::new()
            .width(FILL)
            .spacing(COLUMN_GAP)
            .child(device_and_level)
            .child(mode_and_cleanup),
    )
}

/// "Output": what the user hears through, and how loud.
fn output_card(tokens: &Tokens, model: &ShellModel, voice: &VoiceSettings) -> Stack {
    let volume = text(
        TextRole::Caption,
        tokens.text_muted,
        &format!("{}%", voice.output_volume),
    )
    .id("voice-output-volume-value");
    card(
        tokens,
        "settings-output",
        "Output",
        None,
        Some(
            icon_text_button(tokens, Icon::Play, "Test output")
                .id("voice-test-output")
                .on_click(model.command(Command::TestOutput))
                .into(),
        ),
        OUTPUT_BODY,
        Row::new()
            .width(FILL)
            .spacing(COLUMN_GAP)
            .child(
                Column::new()
                    .flex(1.0)
                    .child(field_label(tokens, "Output device"))
                    .child(Spacer::new().height(LABEL_GAP))
                    .child(device_field(
                        tokens,
                        "voice-output-device",
                        Icon::Speaker,
                        &voice.output_device,
                    )),
            )
            .child(
                Column::new()
                    .flex(1.0)
                    .child(labelled(tokens, "Output volume", volume))
                    // The slider is level with the middle of the device field.
                    .child(Spacer::new().height(LABEL_GAP + (FIELD - SLIDER) / 2.0))
                    .child(slider(
                        tokens,
                        "voice-output-volume",
                        model.output_volume_signal(),
                        (0, 100),
                    )),
            ),
    )
}
