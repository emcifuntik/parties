//! Typography: the one place where font families, sizes and weights are
//! chosen.
//!
//! Components pick a semantic [`TextRole`] and never set a family, size,
//! weight, line height or letter spacing themselves. Each role is registered
//! in lurq's theme as a named typography style, so a text node only carries
//! the role name.
//!
//! The design uses three typefaces: Bricolage Grotesque (titles), Hanken
//! Grotesk (text) and Geist Mono (codes). Their files are not in the
//! repository yet, so the three family names below are aliases of the
//! platform's generic families. To drop the real fonts in, load the files
//! with `App::load_font` and point the aliases at them in [`install`].

use lurq::{
    app::{App, theme::TypographyStyle},
    components::{Text, TextInput},
    core::Signal,
    layout::text_style::{FontWeight, TextStyle},
    node::color::Color,
};

/// Family alias of titles (Bricolage Grotesque in the design).
pub const FAMILY_TITLE: &str = "parties-title";
/// Family alias of running text and controls (Hanken Grotesk in the design).
pub const FAMILY_TEXT: &str = "parties-text";
/// Family alias of codes and fingerprints (Geist Mono in the design).
pub const FAMILY_CODE: &str = "parties-code";

/// The generic families the aliases point at until the real fonts are bundled.
const FALLBACK_SANS: &str = "sans-serif";
const FALLBACK_MONO: &str = "monospace";

/// A semantic text role. The names follow the type scale of `design.pen`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum TextRole {
    /// The title of a view that has no panel around it: "Your parties".
    Display,
    /// The title of a settings page.
    HeadingLarge,
    /// The title of the page in view: the channel name in the chat header.
    HeadingPage,
    /// The brand name in the title bar.
    Heading,
    /// A small title: the user's own name in the voice block.
    HeadingSmall,
    /// The name of the party at the top of the left panel.
    Title,
    /// The line under a display title.
    Lead,
    /// Running message text and the composer draft.
    Body,
    /// The author of a message.
    BodyStrong,
    /// Rows, menu items, names in lists.
    Label,
    /// A row that asks for attention (an unread channel), names in a system line.
    LabelStrong,
    /// Secondary 13 px text: a topic, a voice member, a placeholder.
    Detail,
    /// Button labels, group headers, counters.
    Control,
    /// State lines and sizes under a name.
    Caption,
    /// Small emphasised numbers and names: reaction counts, monograms.
    CaptionStrong,
    /// Timestamps.
    Micro,
    /// The "New messages" marker.
    MicroStrong,
    /// Codes and technical read-outs.
    Code,
    /// A code that is the content of its line: a fingerprint, a join code.
    CodeBody,
}

#[derive(Clone, Copy)]
enum Family {
    Title,
    Text,
    Code,
}

struct RoleSpec {
    name: &'static str,
    family: Family,
    size: f32,
    weight: u16,
    /// Height of one line, in logical pixels.
    line: f32,
    letter_spacing: f32,
}

const fn spec(
    name: &'static str,
    family: Family,
    size: f32,
    weight: u16,
    line: f32,
    letter_spacing: f32,
) -> RoleSpec {
    RoleSpec {
        name,
        family,
        size,
        weight,
        line,
        letter_spacing,
    }
}

impl TextRole {
    pub const ALL: [TextRole; 19] = [
        TextRole::Display,
        TextRole::HeadingLarge,
        TextRole::HeadingPage,
        TextRole::Heading,
        TextRole::HeadingSmall,
        TextRole::Title,
        TextRole::Lead,
        TextRole::Body,
        TextRole::BodyStrong,
        TextRole::Label,
        TextRole::LabelStrong,
        TextRole::Detail,
        TextRole::Control,
        TextRole::Caption,
        TextRole::CaptionStrong,
        TextRole::Micro,
        TextRole::MicroStrong,
        TextRole::Code,
        TextRole::CodeBody,
    ];

    /// The type scale. Where the design sets no line height it gets the
    /// font's own line box, which the design tool rounds to whole pixels
    /// (Hanken Grotesk: 14, 16, 17, 18 and 20 px for 11 to 15 px text), so
    /// the line heights here are those boxes. Message text asks for 1.45.
    #[rustfmt::skip]
    const fn spec(self) -> RoleSpec {
        match self {
            Self::Display       => spec("ui-display",        Family::Title, 30.0, 700, 36.0, 0.0),
            Self::HeadingLarge  => spec("ui-heading-large",  Family::Title, 24.0, 600, 30.0, 0.0),
            Self::HeadingPage   => spec("ui-heading-page",   Family::Title, 17.0, 600, 20.0, 0.0),
            Self::Heading       => spec("ui-heading",        Family::Title, 15.0, 700, 18.0, 0.0),
            Self::HeadingSmall  => spec("ui-heading-small",  Family::Title, 14.0, 600, 17.0, 0.0),
            Self::Title         => spec("ui-title",          Family::Text,  15.0, 600, 20.0, 0.0),
            Self::Lead          => spec("ui-lead",           Family::Text,  15.0, 400, 20.0, 0.0),
            Self::Body          => spec("ui-body",           Family::Text,  14.0, 400, 20.0, 0.0),
            Self::BodyStrong    => spec("ui-body-strong",    Family::Text,  14.0, 600, 18.0, 0.0),
            Self::Label         => spec("ui-label",          Family::Text,  13.0, 500, 17.0, 0.1),
            Self::LabelStrong   => spec("ui-label-strong",   Family::Text,  13.0, 600, 17.0, 0.1),
            Self::Detail        => spec("ui-detail",         Family::Text,  13.0, 400, 17.0, 0.1),
            Self::Control       => spec("ui-control",        Family::Text,  12.0, 500, 16.0, 0.1),
            Self::Caption       => spec("ui-caption",        Family::Text,  12.0, 400, 16.0, 0.1),
            Self::CaptionStrong => spec("ui-caption-strong", Family::Text,  12.0, 600, 16.0, 0.0),
            Self::Micro         => spec("ui-micro",          Family::Text,  11.0, 400, 14.0, 0.2),
            Self::MicroStrong   => spec("ui-micro-strong",   Family::Text,  11.0, 600, 14.0, 0.2),
            Self::Code          => spec("ui-code",           Family::Code,  12.0, 400, 16.0, 0.0),
            Self::CodeBody      => spec("ui-code-body",      Family::Code,  13.0, 400, 17.0, 0.0),
        }
    }

    /// The name of the role in lurq's theme.
    pub const fn name(self) -> &'static str {
        self.spec().name
    }

    /// Font size in logical pixels.
    pub const fn size(self) -> f32 {
        self.spec().size
    }

    /// The text style of the role. Text inputs take a style, not a role name.
    pub fn style(self) -> TextStyle {
        let spec = self.spec();
        TextStyle {
            font_family: match spec.family {
                Family::Title => FAMILY_TITLE.into(),
                Family::Text => FAMILY_TEXT.into(),
                Family::Code => FAMILY_CODE.into(),
            },
            font_size: spec.size,
            line_height: spec.line / spec.size,
            weight: FontWeight::Numeric(spec.weight),
            letter_spacing: spec.letter_spacing,
            ..TextStyle::default()
        }
    }

    fn typography_style(self) -> TypographyStyle {
        TypographyStyle::extra(self.name())
    }
}

/// Registers the family aliases and every role in `app`'s theme. Call once
/// before the first tree is built.
pub fn install(app: &mut App) {
    app.register_font(FAMILY_TITLE, FALLBACK_SANS);
    app.register_font(FAMILY_TEXT, FALLBACK_SANS);
    app.register_font(FAMILY_CODE, FALLBACK_MONO);
    let theme = app.theme();
    for role in TextRole::ALL {
        theme.set_typography_style(role.typography_style(), role.style());
    }
    theme.set_default_text_style(TextRole::Body.style());
}

/// A single line of text in `role`.
pub fn text(role: TextRole, color: Color, content: &str) -> Text {
    Text::new(content)
        .variant(role.typography_style())
        .color(color)
        .nowrap()
}

/// Text in `role` that wraps to the width it is given.
pub fn paragraph(role: TextRole, color: Color, content: &str) -> Text {
    Text::new(content)
        .variant(role.typography_style())
        .color(color)
}

/// A single-line text input whose text and placeholder use `role`.
pub fn text_input(
    role: TextRole,
    color: Color,
    placeholder: Color,
    value: Signal<String>,
) -> TextInput {
    let mut style = role.style();
    style.color = color;
    let mut placeholder_style = role.style();
    placeholder_style.color = placeholder;
    TextInput::styled(value, style)
        .placeholder_style(placeholder_style)
        .single_line()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn role_names_are_unique() {
        let mut names: Vec<_> = TextRole::ALL.iter().map(|role| role.name()).collect();
        names.sort_unstable();
        names.dedup();
        assert_eq!(names.len(), TextRole::ALL.len());
    }

    #[test]
    fn install_registers_every_role() {
        let mut app = App::new();
        install(&mut app);
        for role in TextRole::ALL {
            let style = app.theme().typography_style(role.typography_style());
            assert_eq!(style.font_size, role.size());
        }
    }

    #[test]
    fn the_three_families_stay_separate() {
        assert_eq!(&*TextRole::Heading.style().font_family, FAMILY_TITLE);
        assert_eq!(&*TextRole::Body.style().font_family, FAMILY_TEXT);
        assert_eq!(&*TextRole::Code.style().font_family, FAMILY_CODE);
    }
}
