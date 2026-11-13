//! The App and notifications page of Settings: notifications, updates,
//! start-up and window, language, diagnostics.
//!
//! Its cards stand in two columns. Every card is a list of settings: a name
//! and a line about it on the left, the control on the right, a hairline
//! between two settings. The two lists (sound, language) show the value in
//! use: picking another is not built yet.

use lurq::{
    components::{Column, Rect, Row, Spacer, Stack},
    layout::{Alignment, layout_kind::Justify},
    node::{CursorIcon, Element},
};

use super::{
    icon::Icon,
    settings::{card, switch},
    settings_voice::{device_field, icon_text_button, radio_mark},
    surface::{FILL, surfaced},
};
use crate::{
    commands::Command,
    model::{AppSettings, AppSwitch, UpdateChannel},
    theme::{
        Surface, TextRole, Tokens, metrics::PANEL_GAP, tokens::RADIUS_CONTROL, typography::text,
    },
    view_model::ShellModel,
};

/// Height of a setting: two lines of text.
const ROW: f32 = 36.0;
/// Height of a setting whose control is a list or a pair of choices, and
/// the size of such a control.
const TALL_ROW: f32 = 44.0;
const LIST_WIDTH: f32 = 200.0;
const CHOICE_WIDTH: f32 = 104.0;
/// Height of the line under the settings of "Updates".
const FOOTNOTE: f32 = 16.0;
/// Space above and below the hairline between two settings.
const DIVIDER_GAP: (f32, f32) = (15.0, 16.0);

/// One setting of a card and its height.
type Setting = (Element, f32);

/// The cards of the page, in two columns.
pub(crate) fn app_cards(tokens: &Tokens, model: &ShellModel) -> Row {
    let app = model.app();
    // lurq clips a container's children to it; the shadow of a card reaches
    // past the column the card stands in.
    Row::new()
        .id("settings-app")
        .width(FILL)
        .spacing(PANEL_GAP)
        .overflow_visible()
        .child(
            Column::new()
                .flex(1.0)
                .spacing(PANEL_GAP)
                .overflow_visible()
                .child(notifications(tokens, model, &app))
                .child(updates(tokens, model, &app)),
        )
        .child(
            Column::new()
                .flex(1.0)
                .spacing(PANEL_GAP)
                .overflow_visible()
                .child(start_up(tokens, model, &app))
                .child(language(tokens, &app))
                .child(diagnostics(tokens, model, &app)),
        )
}

/// A card that is a list of settings with a hairline between them.
fn settings_card(tokens: &Tokens, id: &'static str, title: &str, settings: Vec<Setting>) -> Stack {
    let mut list = Column::new().width(FILL);
    let mut height = 0.0;
    for (index, (setting, setting_height)) in settings.into_iter().enumerate() {
        if index > 0 {
            list = list
                .child(Spacer::new().height(DIVIDER_GAP.0))
                .child(Rect::new(FILL, 1.0).background(tokens.hairline))
                .child(Spacer::new().height(DIVIDER_GAP.1));
            height += DIVIDER_GAP.0 + 1.0 + DIVIDER_GAP.1;
        }
        list = list.child(setting);
        height += setting_height;
    }
    card(tokens, id, title, None, None, height, list)
}

/// A setting: its name and a line about it, and `control` at the end.
/// `hint_role` is the role of the line: a caption, or code for a version.
fn setting(
    tokens: &Tokens,
    (title, hint): (&str, &str),
    hint_role: TextRole,
    control: impl Into<Element>,
    height: f32,
) -> Row {
    Row::new()
        .width(FILL)
        .height(height)
        .align_items(Alignment::Center)
        .child(
            Column::new()
                .flex(1.0)
                .spacing(3.0)
                .child(text(TextRole::Label, tokens.text, title))
                .child(text(hint_role, tokens.text_muted, hint)),
        )
        .child(control)
}

/// A setting that is on or off.
fn switch_setting(
    tokens: &Tokens,
    model: &ShellModel,
    app: &AppSettings,
    which: AppSwitch,
) -> Setting {
    let on = app.is_on(which);
    let row = setting(
        tokens,
        (which.title(), which.hint()),
        TextRole::Caption,
        switch(tokens, on),
        ROW,
    )
    .id(format!("app-{}", which.slug()))
    .cursor(CursorIcon::Pointer)
    .on_click(model.command(Command::SetAppSwitch(which, !on)));
    let row = if on { row.class("on") } else { row };
    (row.into(), ROW)
}

/// A setting whose value comes from a list.
fn list_setting(
    tokens: &Tokens,
    id: &'static str,
    labels: (&str, &str),
    glyph: Icon,
    value: &str,
) -> Setting {
    let list = device_field(tokens, id, glyph, value)
        .width(LIST_WIDTH)
        .height(TALL_ROW);
    (
        setting(tokens, labels, TextRole::Caption, list, TALL_ROW).into(),
        TALL_ROW,
    )
}

fn notifications(tokens: &Tokens, model: &ShellModel, app: &AppSettings) -> Stack {
    settings_card(
        tokens,
        "settings-notifications",
        "Notifications",
        vec![
            switch_setting(tokens, model, app, AppSwitch::NotifyMentions),
            switch_setting(tokens, model, app, AppSwitch::NotifyRoomJoins),
            switch_setting(tokens, model, app, AppSwitch::NotifyStreams),
            list_setting(
                tokens,
                "app-sound",
                ("Sound", "Played with notifications."),
                Icon::Speaker,
                &app.notification_sound,
            ),
        ],
    )
}

/// One update channel, as a small card with a radio mark.
fn channel_choice(
    tokens: &Tokens,
    model: &ShellModel,
    channel: UpdateChannel,
    current: bool,
) -> Row {
    let surface = if current {
        Surface {
            radius: RADIUS_CONTROL,
            ..tokens.block_lit
        }
    } else {
        tokens.card
    };
    let slug = match channel {
        UpdateChannel::Stable => "app-channel-stable",
        UpdateChannel::Beta => "app-channel-beta",
    };
    let choice = surfaced!(
        Row::new()
            .id(slug)
            .size(CHOICE_WIDTH, TALL_ROW)
            .padding_horizontal(12.0)
            .spacing(10.0)
            .align_items(Alignment::Center)
            .cursor(CursorIcon::Pointer)
            .on_click(model.command(Command::SetUpdateChannel(channel))),
        surface
    )
    .child(radio_mark(tokens, current))
    .child(text(TextRole::Label, tokens.text, channel.label()));
    if current {
        choice.class("current")
    } else {
        choice
    }
}

fn updates(tokens: &Tokens, model: &ShellModel, app: &AppSettings) -> Stack {
    let mut channels = Row::new().spacing(8.0);
    for channel in UpdateChannel::ALL {
        channels = channels.child(channel_choice(
            tokens,
            model,
            channel,
            channel == app.update_channel,
        ));
    }
    let footnote = Row::new()
        .width(FILL)
        .height(FOOTNOTE)
        .align_items(Alignment::Center)
        .justify(Justify::SpaceBetween)
        .child(
            text(
                TextRole::Caption,
                tokens.text_muted,
                &format!("Last checked {}", app.last_checked),
            )
            .id("app-last-checked"),
        )
        .child(
            Row::new()
                .id("app-whats-new")
                .cursor(CursorIcon::Pointer)
                .on_click(model.command(Command::ShowWhatsNew))
                .child(text(
                    TextRole::Control,
                    tokens.accent_text,
                    &format!("What's new in {}", app.version),
                )),
        );
    settings_card(
        tokens,
        "settings-updates",
        "Updates",
        vec![
            (
                setting(
                    tokens,
                    ("Channel", "Beta gets features earlier."),
                    TextRole::Caption,
                    channels,
                    TALL_ROW,
                )
                .into(),
                TALL_ROW,
            ),
            (
                setting(
                    tokens,
                    (
                        "Current version",
                        &format!("{}, {}", app.version, app.update_state),
                    ),
                    TextRole::Code,
                    icon_text_button(tokens, Icon::Refresh, "Check now")
                        .id("app-check-now")
                        .height(ROW)
                        .on_click(model.command(Command::CheckForUpdates)),
                    ROW,
                )
                .into(),
                ROW,
            ),
            switch_setting(tokens, model, app, AppSwitch::InstallUpdates),
            (footnote.into(), FOOTNOTE),
        ],
    )
}

fn start_up(tokens: &Tokens, model: &ShellModel, app: &AppSettings) -> Stack {
    settings_card(
        tokens,
        "settings-start-up",
        "Start-up and window",
        vec![
            switch_setting(tokens, model, app, AppSwitch::StartWithSystem),
            switch_setting(tokens, model, app, AppSwitch::StartMinimised),
            switch_setting(tokens, model, app, AppSwitch::CloseToTray),
        ],
    )
}

fn language(tokens: &Tokens, app: &AppSettings) -> Stack {
    settings_card(
        tokens,
        "settings-language",
        "Language",
        vec![list_setting(
            tokens,
            "app-language",
            ("App language", "Applies right away."),
            Icon::Globe,
            &app.language,
        )],
    )
}

fn diagnostics(tokens: &Tokens, model: &ShellModel, app: &AppSettings) -> Stack {
    settings_card(
        tokens,
        "settings-diagnostics",
        "Diagnostics",
        vec![
            switch_setting(tokens, model, app, AppSwitch::DiagnosticsOverlay),
            (
                setting(
                    tokens,
                    ("Logs", "Attach them to a bug report."),
                    TextRole::Caption,
                    icon_text_button(tokens, Icon::Folder, "Open logs folder")
                        .id("app-open-logs")
                        .on_click(model.command(Command::OpenLogsFolder)),
                    ROW,
                )
                .into(),
                ROW,
            ),
            (
                setting(
                    tokens,
                    ("Build", "Include it when you report a problem."),
                    TextRole::Caption,
                    text(TextRole::Code, tokens.text_secondary, &app.build).id("app-build"),
                    ROW,
                )
                .into(),
                ROW,
            ),
        ],
    )
}
