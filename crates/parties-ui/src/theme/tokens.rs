//! Every colour, radius and surface of the Glass design, in one place.
//!
//! Components never write a colour or a radius themselves: they take a
//! [`Tokens`] value (built for the current light) and use its fields. The
//! values are the ones `design.pen` uses; alpha is written as the byte the
//! design source has (`0x99` is 60%).

use lurq::node::color::Color;

use super::{
    color::{Rgb, over, rgba},
    lights::LightId,
};

/// Corner radius of a glass panel.
pub const RADIUS_PANEL: f32 = 20.0;
/// Corner radius of a block inside a panel, and of the Stream menu.
pub const RADIUS_BLOCK: f32 = 14.0;
/// Corner radius of a control (button, field, row).
pub const RADIUS_CONTROL: f32 = 12.0;
/// Corner radius of a chip, a small tile or a menu row.
pub const RADIUS_CHIP: f32 = 8.0;
/// Corner radius of a floating menu.
pub const RADIUS_MENU: f32 = 16.0;

/// A flat surface: fill, 1 px inside border, corner radius.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Surface {
    pub fill: Color,
    pub border: Color,
    pub radius: f32,
}

impl Surface {
    const fn new(fill: u32, border: u32, radius: f32) -> Self {
        Self {
            fill: rgba(fill),
            border: rgba(border),
            radius,
        }
    }

    const fn tinted(tint: Rgb, fill: u8, border: u8, radius: f32) -> Self {
        Self {
            fill: tint.alpha(fill),
            border: tint.alpha(border),
            radius,
        }
    }
}

/// A drop shadow below a floating surface.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Shadow {
    pub color: Color,
    pub offset_y: f32,
    pub blur: f32,
}

/// A glass surface: a translucent fill under a faint vertical sheen, a border
/// that fades from top to bottom, a drop shadow, and a blur of what is behind.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Glass {
    /// Fill with the sheen flattened into it, at the top edge.
    pub fill_top: Color,
    /// Fill with the sheen flattened into it, at the bottom edge.
    pub fill_bottom: Color,
    pub border_top: Color,
    pub border_bottom: Color,
    pub radius: f32,
    pub shadow: Shadow,
    /// The backdrop blur radius the design asks for. lurq 0.60 has no backdrop
    /// filter, so nothing reads this yet; it is kept so the surface is
    /// complete when the toolkit can paint it.
    pub backdrop_blur: f32,
}

impl Glass {
    fn new(fill: u32, sheen: (u32, u32), border: (u32, u32), radius: f32, shadow: Shadow) -> Self {
        Self {
            fill_top: over(rgba(sheen.0), rgba(fill)),
            fill_bottom: over(rgba(sheen.1), rgba(fill)),
            border_top: rgba(border.0),
            border_bottom: rgba(border.1),
            radius,
            shadow,
            backdrop_blur: 32.0,
        }
    }
}

/// One radial wash of the backdrop, in fractions of the window: an ellipse of
/// `size` (full width and height) around `center`, fading through `stops`
/// (colour, position from the centre to the edge).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Wash {
    pub center: (f32, f32),
    pub size: (f32, f32),
    pub stops: [(Color, f32); 3],
}

/// The window backdrop: a base colour and two washes.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Backdrop {
    pub base: Color,
    pub wash_top_left: Wash,
    pub wash_bottom_right: Wash,
}

/// The resolved tokens of one light.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Tokens {
    pub light: LightId,
    pub backdrop: Backdrop,

    // Accent.
    pub accent: Color,
    /// Text and icons tinted with the accent, on a dark surface.
    pub accent_text: Color,
    /// Labels and icons on an accent fill.
    pub on_accent: Color,
    /// The "New messages" rule.
    pub accent_line: Color,

    // Text.
    pub text: Color,
    /// Running message text.
    pub text_body: Color,
    pub text_secondary: Color,
    pub text_muted: Color,
    pub text_faint: Color,
    /// The state line of an offline member.
    pub text_offline: Color,

    // State.
    pub online: Color,
    pub idle: Color,
    pub do_not_disturb: Color,
    pub danger_label: Color,
    pub danger_icon: Color,
    /// The ring that separates a presence dot from the avatar. The design
    /// uses one fixed dark colour close to a panel over the backdrop.
    pub presence_ring: Color,

    // Lines.
    pub hairline: Color,
    pub divider: Color,
    /// The thumb of a scrollbar; its track is not drawn.
    pub scrollbar_thumb: Color,

    // Glass.
    pub panel: Glass,
    /// The chat panel: more opaque, for reading.
    pub panel_reading: Glass,
    pub menu: Glass,
    pub menu_stream: Glass,

    // Surfaces inside a panel.
    pub block: Surface,
    pub block_lit: Surface,
    pub row_selected: Surface,
    pub row_hover: Surface,
    pub menu_row_current: Surface,
    pub menu_row_live: Surface,
    pub control: Surface,
    pub control_hover: Surface,
    pub control_danger: Surface,
    pub control_lit: Surface,
    pub field: Surface,
    pub composer: Surface,
    pub switcher: Surface,
    pub pill_active: Surface,
    pub tile_party: Surface,
    pub tile: Surface,
    pub tile_on: Surface,
    pub chip: Surface,
    pub chip_mine: Surface,
    pub card: Surface,
    pub image_border: Color,
    pub send: Surface,

    // Monogram avatars (a member without a picture).
    pub monogram_top: Color,
    pub monogram_bottom: Color,
    pub monogram_border: Color,
}

const CLEAR: u32 = 0x0000_0000;
const DANGER: Rgb = Rgb::hex(0xFF5A52);

impl Tokens {
    /// The tokens of `light`.
    pub fn for_light(light: LightId) -> Self {
        let colors = light.light();
        let accent = colors.accent;
        let panel_shadow = Shadow {
            color: rgba(0x0803_0573),
            offset_y: 16.0,
            blur: 40.0,
        };
        let panel = Glass::new(
            0x0E12_1899,
            (0xFFFF_FF12, 0xFFFF_FF08),
            (0xFFFF_FF2B, 0xFFFF_FF0F),
            RADIUS_PANEL,
            panel_shadow,
        );
        Self {
            light,
            backdrop: Backdrop {
                base: colors.base.opaque(),
                wash_top_left: Wash {
                    center: (0.24, -0.08),
                    size: (1.5, 1.7),
                    stops: [
                        (colors.wash_top_left.opaque(), 0.0),
                        (colors.wash_top_left_mid.alpha(0x99), 0.45),
                        (colors.wash_top_left_mid.alpha(0x00), 1.0),
                    ],
                },
                wash_bottom_right: Wash {
                    center: (0.96, 1.1),
                    size: (1.2, 1.3),
                    stops: [
                        (colors.wash_bottom_right.opaque(), 0.0),
                        (colors.wash_bottom_right.alpha(0x80), 0.5),
                        (colors.wash_bottom_right.alpha(0x00), 1.0),
                    ],
                },
            },

            accent: accent.opaque(),
            accent_text: accent.mix(Rgb::WHITE, 0.23).opaque(),
            on_accent: rgba(0x0A0A_0CFF),
            accent_line: accent.alpha(0x73),

            text: rgba(0xF4F4_F6FF),
            text_body: rgba(0xE6E6_E9FF),
            text_secondary: rgba(0xCDCD_D0FF),
            text_muted: rgba(0x9C9C_A1FF),
            text_faint: rgba(0x7777_7DFF),
            text_offline: rgba(0x8585_8BFF),

            online: rgba(0x59D4_99FF),
            idle: rgba(0xFFC5_33FF),
            do_not_disturb: DANGER.opaque(),
            danger_label: rgba(0xFF93_8DFF),
            danger_icon: rgba(0xFF7A_73FF),
            presence_ring: rgba(0x1217_1DFF),

            hairline: rgba(0xFFFF_FF14),
            divider: rgba(0xFFFF_FF24),
            scrollbar_thumb: rgba(0xFFFF_FF29),

            panel,
            panel_reading: Glass::new(
                0x0B0E_13D9,
                (0xFFFF_FF0F, 0xFFFF_FF05),
                (0xFFFF_FF2B, 0xFFFF_FF0F),
                RADIUS_PANEL,
                panel_shadow,
            ),
            // The design fills menus at 90% and 95% and blurs what is behind
            // them. lurq has no backdrop blur yet, so a translucent menu lets
            // the text under it show through sharp. Until blur exists the
            // menus are opaque; restore 0x1217_1FE6 and 0x1217_1FF2 then.
            menu: Glass::new(
                0x1217_1FFF,
                (0xFFFF_FF12, 0xFFFF_FF08),
                (0xFFFF_FF2B, 0xFFFF_FF0F),
                RADIUS_MENU,
                panel_shadow,
            ),
            menu_stream: Glass::new(
                0x1217_1FFF,
                (0xFFFF_FF14, 0xFFFF_FF08),
                (0xFFFF_FF3D, 0xFFFF_FF14),
                RADIUS_BLOCK,
                Shadow {
                    color: rgba(0x0407_0A99),
                    offset_y: 12.0,
                    blur: 32.0,
                },
            ),

            block: Surface::new(0xFFFF_FF0A, 0xFFFF_FF1A, RADIUS_BLOCK),
            block_lit: Surface::tinted(accent, 0x14, 0x33, RADIUS_BLOCK),
            row_selected: Surface::tinted(accent, 0x1F, 0x33, RADIUS_CONTROL),
            row_hover: Surface::new(0xFFFF_FF0D, CLEAR, RADIUS_CONTROL),
            menu_row_current: Surface::new(0xFFFF_FF14, CLEAR, RADIUS_CHIP),
            menu_row_live: Surface::tinted(accent, 0x1F, 0x00, RADIUS_CHIP),
            control: Surface::new(0xFFFF_FF14, 0xFFFF_FF1F, RADIUS_CONTROL),
            control_hover: Surface::new(0xFFFF_FF1F, 0xFFFF_FF29, RADIUS_CONTROL),
            control_danger: Surface::tinted(DANGER, 0x24, 0x4D, RADIUS_CONTROL),
            control_lit: Surface::tinted(accent, 0x26, 0x66, RADIUS_CONTROL),
            field: Surface::new(0xFFFF_FF0F, 0xFFFF_FF1A, RADIUS_CONTROL),
            composer: Surface::new(0xFFFF_FF0F, 0xFFFF_FF24, RADIUS_CONTROL),
            switcher: Surface::new(0xFFFF_FF0F, 0xFFFF_FF1A, RADIUS_CONTROL),
            pill_active: Surface::tinted(accent, 0x26, 0x66, RADIUS_CHIP),
            tile_party: Surface::tinted(accent, 0x26, 0x4D, RADIUS_CONTROL),
            tile: Surface::new(0xFFFF_FF14, CLEAR, RADIUS_CHIP),
            tile_on: Surface::new(0xFFFF_FF1A, 0xFFFF_FF14, RADIUS_CONTROL),
            chip: Surface::new(0xFFFF_FF0F, 0xFFFF_FF1A, RADIUS_CHIP),
            chip_mine: Surface::tinted(accent, 0x1F, 0x66, RADIUS_CHIP),
            card: Surface::new(0xFFFF_FF0F, 0xFFFF_FF1A, RADIUS_CONTROL),
            image_border: rgba(0xFFFF_FF1F),
            send: Surface {
                fill: accent.opaque(),
                border: accent.opaque(),
                radius: RADIUS_CHIP,
            },

            monogram_top: rgba(0xFFFF_FF33),
            monogram_bottom: rgba(0xFFFF_FF14),
            monogram_border: rgba(0xFFFF_FF29),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn night_tokens_match_the_brief() {
        let tokens = Tokens::for_light(LightId::Night);
        assert_eq!(tokens.accent, rgba(0x8FDC_FFFF));
        assert_eq!(tokens.accent_text, rgba(0xA9E4_FFFF));
        assert_eq!(tokens.backdrop.base, rgba(0x090B_0FFF));
        assert_eq!(tokens.block_lit.fill, rgba(0x8FDC_FF14));
        assert_eq!(tokens.row_selected.border, rgba(0x8FDC_FF33));
    }

    #[test]
    fn only_accent_and_backdrop_follow_the_light() {
        let night = Tokens::for_light(LightId::Night);
        let ember = Tokens::for_light(LightId::Ember);
        assert_ne!(night.accent, ember.accent);
        assert_ne!(night.backdrop, ember.backdrop);
        assert_eq!(night.text, ember.text);
        assert_eq!(night.panel.fill_top, ember.panel.fill_top);
        assert_eq!(night.control_danger, ember.control_danger);
    }
}
