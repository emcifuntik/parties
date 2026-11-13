//! Surfaces: the glass panel and the flat surfaces drawn inside it.
//!
//! lurq paints one background, one solid border and box shadows per element,
//! and blurs what is behind an element. The design's glass needs two things
//! lurq does not have as such:
//!
//! - a fill under a sheen gradient: flattened into one gradient by the theme
//!   ([`Glass::fill_top`](crate::theme::Glass) and `fill_bottom`);
//! - a border that fades from top to bottom: composed here from two clipped
//!   rings and two gradient lines ([`glass_border`]).

use lurq::{
    components::{Column, Rect, Row, Stack},
    layout::{layout_kind::Justify, scrollbar::ScrollBarStyle},
    node::{BoxShadow, Element, Gradient, color::Color, dimension::Dimension},
};

use crate::theme::{Glass, Tokens};

/// Applies a flat [`Surface`](crate::theme::Surface) (fill, 1 px inside
/// border, radius) to any container builder. A macro because lurq's builders
/// share their methods by name, not through a trait.
macro_rules! surfaced {
    ($node:expr, $surface:expr) => {{
        let surface: $crate::theme::Surface = $surface;
        $node
            .background(surface.fill)
            .border_inside(1.0, surface.border)
            .rounded(surface.radius)
    }};
}
pub(crate) use surfaced;

/// Full size of the parent.
pub(crate) const FILL: Dimension = Dimension::Pct(100.0);

fn vertical(top: Color, bottom: Color) -> Gradient {
    // 180 degrees points down: the first stop is at the top edge.
    Gradient::linear(180.0, [top, bottom])
}

/// A glass surface around `content`: what is behind it shows through its
/// fill, blurred. The caller sizes the returned stack; `content` should fill
/// it.
pub(crate) fn glass(glass: &Glass, content: impl Into<Element>) -> Stack {
    let shadow = glass.shadow;
    Stack::new()
        .rounded(glass.radius)
        .backdrop_blur(glass.backdrop_blur)
        .background_gradient(vertical(glass.fill_top, glass.fill_bottom))
        .box_shadow(BoxShadow::new(
            0.0,
            shadow.offset_y,
            shadow.blur,
            shadow.color,
        ))
        .child(content)
        .child(glass_border(glass))
}

/// The fading border of a glass surface, as an overlay that takes no input.
///
/// A lurq border has one colour, and per-side borders do not follow the
/// corner radius. So the top and the bottom of the outline are each a full
/// ring in one colour, clipped to a band as tall as the radius, and the two
/// sides between the bands are 1 px lines with the gradient. The gradient
/// changes by a fraction of a percent across a band, so the joins do not show.
fn glass_border(glass: &Glass) -> Column {
    let radius = glass.radius;
    let band = |color: Color, ring_y: f32| {
        Stack::new().width(FILL).height(radius).clip().child(
            Rect::new(FILL, radius * 2.0)
                .absolute(0.0, ring_y, FILL, radius * 2.0)
                .rounded(radius)
                .border_inside(1.0, color),
        )
    };
    let side = || {
        Rect::new(1.0, FILL).background_gradient(vertical(glass.border_top, glass.border_bottom))
    };
    Column::new()
        .width(FILL)
        .height(FILL)
        .pointer_events_none()
        .child(band(glass.border_top, 0.0))
        .child(
            Row::new()
                .width(FILL)
                .flex(1.0)
                .justify(Justify::SpaceBetween)
                .child(side())
                .child(side()),
        )
        .child(band(glass.border_bottom, -radius))
}

/// The scrollbar of every scrolling area: a thin thumb over the content,
/// without a track.
pub(crate) fn scrollbar(tokens: &Tokens) -> ScrollBarStyle {
    ScrollBarStyle {
        thumb_color: tokens.scrollbar_thumb,
        track_color: Color::new(0, 0, 0, 0),
        ..ScrollBarStyle::thin()
    }
}
