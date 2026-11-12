//! Sizes of the window frame, in logical pixels. Sizes that belong to one
//! component (a row height, an icon size) stay next to that component.

/// The size the design is drawn at.
pub const WINDOW_WIDTH: u32 = 1440;
pub const WINDOW_HEIGHT: u32 = 900;
/// The smallest window the three-panel layout is meant for
/// (docs/design/README.md, "Smaller windows": below this the layout changes).
pub const WINDOW_MIN_WIDTH: u32 = 1020;
pub const WINDOW_MIN_HEIGHT: u32 = 640;

/// Height of the custom title bar.
pub const TITLE_BAR_HEIGHT: f32 = 52.0;
/// Space between the window edge and the panels, and between panels.
pub const PANEL_GAP: f32 = 12.0;
/// Width of the left panel and of the members panel.
pub const SIDE_PANEL_WIDTH: f32 = 264.0;
/// Padding inside a side panel.
pub const PANEL_PADDING: f32 = 8.0;
/// Width of a block that spans a side panel.
pub const SIDE_BLOCK_WIDTH: f32 = SIDE_PANEL_WIDTH - 2.0 * PANEL_PADDING;
/// Thickness of the invisible resize zones along the window edge.
pub const RESIZE_ZONE: f32 = 6.0;
