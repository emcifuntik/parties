//! The eight Glass lights (docs/design/README.md, "Glass lights").
//!
//! A light is a base colour, two radial washes and one accent. Panels, text
//! and state colours are the same in every light. This module is plain data:
//! it does not depend on the toolkit.

use super::color::Rgb;

/// Which light the window is drawn in. A user setting.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum LightId {
    #[default]
    Night,
    Ember,
    Smoke,
    Ocean,
    Dusk,
    Rose,
    Honey,
    Moss,
}

/// The colours of one light.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Light {
    pub id: LightId,
    /// The name the Appearance page shows.
    pub name: &'static str,
    /// Window backdrop.
    pub base: Rgb,
    /// Centre of the wash from the top left.
    pub wash_top_left: Rgb,
    /// The same wash at 45% of its radius, where the design darkens it and
    /// drops it to 60% opacity. Taken from `design.pen`; the README table
    /// lists only the centre colour.
    pub wash_top_left_mid: Rgb,
    /// Centre of the wash from the bottom right.
    pub wash_bottom_right: Rgb,
    pub accent: Rgb,
}

const fn light(
    id: LightId,
    name: &'static str,
    base: u32,
    wash_top_left: u32,
    wash_top_left_mid: u32,
    wash_bottom_right: u32,
    accent: u32,
) -> Light {
    Light {
        id,
        name,
        base: Rgb::hex(base),
        wash_top_left: Rgb::hex(wash_top_left),
        wash_top_left_mid: Rgb::hex(wash_top_left_mid),
        wash_bottom_right: Rgb::hex(wash_bottom_right),
        accent: Rgb::hex(accent),
    }
}

/// Every light, in the order of the design's table.
#[rustfmt::skip]
pub const LIGHTS: [Light; 8] = [
    light(LightId::Night, "Night", 0x090B0F, 0x17607F, 0x0F435C, 0x0F4A46, 0x8FDCFF),
    light(LightId::Ember, "Ember", 0x0C0A0C, 0x7A2436, 0x5A1A2B, 0x4A2140, 0xFFB38F),
    light(LightId::Smoke, "Smoke", 0x0B0C0D, 0x444B57, 0x2E333C, 0x2C322D, 0xC8F169),
    light(LightId::Ocean, "Ocean", 0x090A10, 0x1F4596, 0x16316E, 0x123A5E, 0xA3BCFF),
    light(LightId::Dusk,  "Dusk",  0x0B0A10, 0x51308A, 0x3A2266, 0x2A2460, 0xC9B0FF),
    light(LightId::Rose,  "Rose",  0x0D0A0C, 0x86245F, 0x5F1A45, 0x47205A, 0xFFA3CF),
    light(LightId::Honey, "Honey", 0x0C0B09, 0x71501A, 0x523A12, 0x4E2C16, 0xFFD27A),
    light(LightId::Moss,  "Moss",  0x090C0A, 0x1E5E43, 0x154533, 0x3B4A1E, 0x86E8AE),
];

impl LightId {
    pub const ALL: [LightId; 8] = [
        LightId::Night,
        LightId::Ember,
        LightId::Smoke,
        LightId::Ocean,
        LightId::Dusk,
        LightId::Rose,
        LightId::Honey,
        LightId::Moss,
    ];

    /// The colours of this light.
    pub const fn light(self) -> &'static Light {
        // `LIGHTS` is in declaration order, so the discriminant is the index.
        &LIGHTS[self as usize]
    }

    pub const fn name(self) -> &'static str {
        self.light().name
    }

    /// The light after this one, wrapping around.
    pub const fn next(self) -> LightId {
        Self::ALL[(self as usize + 1) % Self::ALL.len()]
    }

    /// Finds a light by its name, ignoring ASCII case.
    pub fn from_name(name: &str) -> Option<LightId> {
        Self::ALL
            .into_iter()
            .find(|id| id.name().eq_ignore_ascii_case(name))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn table_is_in_declaration_order() {
        for id in LightId::ALL {
            assert_eq!(id.light().id, id);
        }
    }

    #[test]
    fn next_visits_every_light_and_wraps() {
        let mut id = LightId::Night;
        for expected in LightId::ALL.into_iter().skip(1) {
            id = id.next();
            assert_eq!(id, expected);
        }
        assert_eq!(id.next(), LightId::Night);
    }

    #[test]
    fn from_name_ignores_case() {
        assert_eq!(LightId::from_name("ember"), Some(LightId::Ember));
        assert_eq!(LightId::from_name("MOSS"), Some(LightId::Moss));
        assert_eq!(LightId::from_name("noon"), None);
    }
}
