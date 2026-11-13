//! The Video and sharing page of Settings: the camera (a preview, the device
//! and its format) and the defaults of a screen share (codec, encoder,
//! format, bitrate, audio).
//!
//! The lists show the value in use: picking another is not built yet. The
//! preview box has no picture until the video pipeline exists; it shows
//! whether the preview is on.

use lurq::{
    components::{Column, Row, Spacer, Stack},
    layout::{Alignment, layout_kind::Justify},
    node::{CursorIcon, Element},
};

use super::{
    icon::{Icon, icon},
    settings::card,
    settings_voice::{
        caption, field_label, icon_text_button, labelled, radio_mark, slider, switch_row,
    },
    surface::{FILL, surfaced},
};
use crate::{
    commands::Command,
    model::{CodecChoice, VideoSettings},
    theme::{
        Surface, TextRole, Tokens,
        tokens::{RADIUS_BLOCK, RADIUS_CONTROL},
        typography::text,
    },
    view_model::ShellModel,
};

/// Space between the two columns of a card.
const COLUMN_GAP: f32 = 48.0;
/// Height of a field label, and from it to the field under it.
const LABEL: f32 = 16.0;
const LABEL_GAP: f32 = 9.0;
/// Height of a list, a choice or a read-out field.
const FIELD: f32 = 44.0;
/// From a field to the next label, and from a field to the line about it.
const FIELD_GAP: f32 = 20.0;
const HINT_GAP: f32 = 8.0;
/// Height of the camera preview: three labelled lists and the line under
/// them are as tall.
const PREVIEW_HEIGHT: f32 = 3.0 * (LABEL + LABEL_GAP + FIELD) + 3.0 * FIELD_GAP + LABEL;
/// Width of the frame rate list next to the resolution list.
const FRAME_RATE_WIDTH: f32 = 160.0;
/// Height of the body of "Screen sharing defaults": its right column.
const SHARING_BODY: f32 =
    LABEL + LABEL_GAP + FIELD + 24.0 + LABEL + 11.0 + 16.0 + 10.0 + LABEL + 24.0 + 36.0;
/// The bitrates the slider reaches, in Mbps.
const BITRATE_RANGE: (i32, i32) = (1, 20);

/// The cards of the page, top to bottom.
pub(crate) fn video_cards(tokens: &Tokens, model: &ShellModel) -> [Stack; 2] {
    let video = model.video();
    [
        camera_card(tokens, model, &video),
        sharing_card(tokens, model, &video),
    ]
}

/// A value in use, as the closed field of a list, or as a read-out when
/// there is nothing to choose (`caret` off).
fn field(
    tokens: &Tokens,
    id: &'static str,
    glyph: Option<Icon>,
    role: TextRole,
    value: &str,
    caret: bool,
) -> Row {
    let mut field = surfaced!(
        Row::new()
            .id(id)
            .width(FILL)
            .height(FIELD)
            .padding_horizontal(13.0)
            .spacing(10.0)
            .align_items(Alignment::Center),
        tokens.field
    );
    if let Some(glyph) = glyph {
        field = field.child(icon(glyph, 16.0, tokens.text_muted));
    }
    field = field
        .child(text(role, tokens.text, value))
        .child(Spacer::new().flex(1.0));
    if caret {
        field = field.child(icon(Icon::CaretDown, 13.0, tokens.text_muted));
    }
    field
}

/// A label and the field under it.
fn labelled_field(tokens: &Tokens, label: &str, field: Row) -> Column {
    Column::new()
        .width(FILL)
        .child(field_label(tokens, label))
        .child(Spacer::new().height(LABEL_GAP))
        .child(field)
}

/// "Camera": the preview and the device.
fn camera_card(tokens: &Tokens, model: &ShellModel, video: &VideoSettings) -> Stack {
    let (glyph, glyph_color, state, action) = if video.camera_preview {
        (Icon::Camera, tokens.accent, "Preview is on", "Stop preview")
    } else {
        (
            Icon::CameraOff,
            tokens.text_muted,
            "Preview is off",
            "Start preview",
        )
    };
    let preview = surfaced!(
        Column::new()
            .id("video-preview")
            .flex(1.0)
            .height(PREVIEW_HEIGHT)
            .align_items(Alignment::Center)
            .justify(Justify::Center),
        Surface {
            radius: RADIUS_BLOCK,
            ..tokens.well
        }
    )
    .child(icon(glyph, 24.0, glyph_color))
    .child(Spacer::new().height(12.0))
    .child(text(TextRole::Caption, tokens.text_muted, state).id("video-preview-state"))
    .child(Spacer::new().height(16.0))
    .child(
        icon_text_button(tokens, Icon::Play, action)
            .id("video-preview-toggle")
            .height(36.0)
            .on_click(model.command(Command::SetCameraPreview(!video.camera_preview))),
    );
    let preview = if video.camera_preview {
        preview.class("on")
    } else {
        preview
    };

    let device = Column::new()
        .flex(1.0)
        .child(labelled_field(
            tokens,
            "Camera",
            field(
                tokens,
                "video-camera",
                Some(Icon::Camera),
                TextRole::Body,
                &video.camera,
                true,
            ),
        ))
        .child(Spacer::new().height(FIELD_GAP))
        .child(labelled_field(
            tokens,
            "Resolution",
            field(
                tokens,
                "video-camera-resolution",
                None,
                TextRole::Body,
                &video.camera_resolution,
                true,
            ),
        ))
        .child(Spacer::new().height(FIELD_GAP))
        .child(labelled_field(
            tokens,
            "Frame rate",
            field(
                tokens,
                "video-camera-frame-rate",
                None,
                TextRole::Body,
                &video.camera_frame_rate,
                true,
            ),
        ))
        .child(Spacer::new().height(FIELD_GAP))
        .child(caption(
            tokens.text_muted,
            "Your camera is sent to the room only while it is on. Changes apply right away, even mid-call.",
        ));

    card(
        tokens,
        "settings-camera",
        "Camera",
        None,
        None,
        PREVIEW_HEIGHT,
        Row::new()
            .width(FILL)
            .spacing(COLUMN_GAP)
            .child(preview)
            .child(device),
    )
}

/// One codec, as a small card with a radio mark.
fn codec_choice(tokens: &Tokens, model: &ShellModel, codec: CodecChoice, current: bool) -> Row {
    let surface = if current {
        Surface {
            radius: RADIUS_CONTROL,
            ..tokens.block_lit
        }
    } else {
        tokens.card
    };
    let choice = surfaced!(
        Row::new()
            .id(format!("video-codec-{}", codec.slug()))
            .flex(1.0)
            .height(FIELD)
            .padding_horizontal(12.0)
            .spacing(10.0)
            .align_items(Alignment::Center)
            .cursor(CursorIcon::Pointer)
            .on_click(model.command(Command::SetPreferredCodec(codec))),
        surface
    )
    .child(radio_mark(tokens, current))
    .child(text(TextRole::Label, tokens.text, codec.label()));
    if current {
        choice.class("current")
    } else {
        choice
    }
}

/// "Screen sharing defaults": what a new share starts with.
fn sharing_card(tokens: &Tokens, model: &ShellModel, video: &VideoSettings) -> Stack {
    let mut codecs = Row::new().width(FILL).spacing(8.0);
    for codec in CodecChoice::ALL {
        codecs = codecs.child(codec_choice(tokens, model, codec, codec == video.codec));
    }
    let codec_and_encoder = Column::new()
        .flex(1.0)
        .child(field_label(tokens, "Preferred codec"))
        .child(Spacer::new().height(LABEL_GAP))
        .child(codecs)
        .child(Spacer::new().height(HINT_GAP))
        .child(caption(tokens.text_muted, video.codec.hint()))
        .child(Spacer::new().height(24.0))
        .child(labelled_field(
            tokens,
            "Encoder",
            field(
                tokens,
                "video-encoder",
                Some(Icon::Chip),
                TextRole::CodeBody,
                &video.encoder,
                false,
            ),
        ))
        .child(Spacer::new().height(HINT_GAP))
        .child(caption(
            tokens.text_muted,
            "Detected on this PC. Falls back to software when busy.",
        ));

    let bitrate: Element = text(
        TextRole::Caption,
        tokens.text_muted,
        &format!("{} Mbps", video.share_bitrate_mbps),
    )
    .id("video-bitrate-value")
    .into();
    let format_and_rate = Column::new()
        .flex(1.0)
        .child(
            Row::new()
                .width(FILL)
                .spacing(12.0)
                .child(
                    labelled_field(
                        tokens,
                        "Resolution",
                        field(
                            tokens,
                            "video-share-resolution",
                            None,
                            TextRole::Body,
                            &video.share_resolution,
                            true,
                        ),
                    )
                    .flex(1.0),
                )
                .child(
                    labelled_field(
                        tokens,
                        "Frame rate",
                        field(
                            tokens,
                            "video-share-frame-rate",
                            None,
                            TextRole::Body,
                            &video.share_frame_rate,
                            true,
                        ),
                    )
                    .width(FRAME_RATE_WIDTH),
                ),
        )
        .child(Spacer::new().height(24.0))
        .child(labelled(tokens, "Bitrate", bitrate))
        .child(Spacer::new().height(11.0))
        .child(slider(
            tokens,
            "video-bitrate",
            model.share_bitrate_signal(),
            BITRATE_RANGE,
        ))
        .child(Spacer::new().height(10.0))
        .child(caption(
            tokens.text_muted,
            "Average. Lowered automatically on a weak connection.",
        ))
        .child(Spacer::new().height(24.0))
        .child(switch_row(
            tokens,
            model,
            "video-share-audio",
            (
                "Share audio by default",
                "Send the sound of the shared window or screen.",
            ),
            video.share_audio,
            Command::SetShareAudio(!video.share_audio),
        ));

    card(
        tokens,
        "settings-sharing",
        "Screen sharing defaults",
        None,
        None,
        SHARING_BODY,
        Row::new()
            .width(FILL)
            .spacing(COLUMN_GAP)
            .child(codec_and_encoder)
            .child(format_and_rate),
    )
}
