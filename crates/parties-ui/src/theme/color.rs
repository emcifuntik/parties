//! Colour arithmetic for the theme. Everything works on straight-alpha sRGB
//! bytes, the way the design source writes its colours.

use lurq::node::color::Color;

/// An opaque sRGB colour as the design writes it (`0xRRGGBB`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rgb(pub u8, pub u8, pub u8);

impl Rgb {
    pub const WHITE: Rgb = Rgb(255, 255, 255);

    /// `0xRRGGBB`.
    pub const fn hex(value: u32) -> Self {
        Self((value >> 16) as u8, (value >> 8) as u8, value as u8)
    }

    pub const fn opaque(self) -> Color {
        Color::new(self.0, self.1, self.2, 255)
    }

    /// This colour with the alpha byte the design source gives (`0x99` is 60%).
    pub const fn alpha(self, alpha: u8) -> Color {
        Color::new(self.0, self.1, self.2, alpha)
    }

    /// Linear mix in sRGB bytes: `t = 0` is `self`, `t = 1` is `other`.
    pub fn mix(self, other: Rgb, t: f32) -> Rgb {
        let t = t.clamp(0.0, 1.0);
        let channel =
            |a: u8, b: u8| (f32::from(a) + (f32::from(b) - f32::from(a)) * t).round() as u8;
        Rgb(
            channel(self.0, other.0),
            channel(self.1, other.1),
            channel(self.2, other.2),
        )
    }
}

/// `0xRRGGBBAA`, the eight-digit form of the design source.
pub const fn rgba(value: u32) -> Color {
    Color::new(
        (value >> 24) as u8,
        (value >> 16) as u8,
        (value >> 8) as u8,
        value as u8,
    )
}

/// `top` painted over `bottom` (source-over), both translucent. The design
/// stacks a sheen gradient on a translucent fill; lurq paints one background
/// per element, so the theme flattens the two layers into one colour per
/// gradient stop.
pub fn over(top: Color, bottom: Color) -> Color {
    let top_alpha = f32::from(top.a()) / 255.0;
    let bottom_alpha = f32::from(bottom.a()) / 255.0;
    let alpha = top_alpha + bottom_alpha * (1.0 - top_alpha);
    if alpha <= 0.0 {
        return Color::new(0, 0, 0, 0);
    }
    let channel = |top_channel: u8, bottom_channel: u8| {
        let value = (f32::from(top_channel) * top_alpha
            + f32::from(bottom_channel) * bottom_alpha * (1.0 - top_alpha))
            / alpha;
        value.round().clamp(0.0, 255.0) as u8
    };
    Color::new(
        channel(top.r(), bottom.r()),
        channel(top.g(), bottom.g()),
        channel(top.b(), bottom.b()),
        (alpha * 255.0).round() as u8,
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_and_rgba_split_channels() {
        assert_eq!(Rgb::hex(0x8FDCFF), Rgb(0x8F, 0xDC, 0xFF));
        assert_eq!(rgba(0x0E121899), Color::new(0x0E, 0x12, 0x18, 0x99));
    }

    #[test]
    fn mix_matches_the_design_accent_tint() {
        // The design's tint of the Night accent (#8FDCFF) is #A9E4FF.
        assert_eq!(Rgb::hex(0x8FDCFF).mix(Rgb::WHITE, 0.23), Rgb::hex(0xA9E4FF));
    }

    #[test]
    fn over_keeps_an_opaque_bottom_opaque() {
        let flat = over(rgba(0xFFFFFF80), Rgb::hex(0x000000).opaque());
        assert_eq!(flat.a(), 255);
        assert_eq!(flat.r(), 128);
    }

    #[test]
    fn over_adds_coverage_of_two_translucent_layers() {
        // 7% white over a 60% fill covers 62.8% of what is behind.
        let flat = over(rgba(0xFFFFFF12), rgba(0x0E121899));
        assert_eq!(flat.a(), 160);
    }
}
