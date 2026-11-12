//! The window the shell runs in: frameless, with the title bar drawn by the
//! shell itself, rendered with wgpu.

use lurq::app::{App, Tree, wgpu_render::WgpuRenderEngine, winit_shell::WinitWindow};

use crate::{
    Shell, ShellModel, theme,
    theme::metrics::{WINDOW_HEIGHT, WINDOW_MIN_HEIGHT, WINDOW_MIN_WIDTH, WINDOW_WIDTH},
};

/// The window title the taskbar shows.
pub const TITLE: &str = "Parties";

/// A lurq app with the Parties fonts and typography roles installed.
pub fn app() -> App {
    let mut app = App::new();
    theme::install(&mut app);
    app
}

/// An empty tree that renders with wgpu (Direct3D 12 on Windows).
pub fn tree() -> Tree {
    let mut tree = Tree::new();
    tree.set_render_engine_factory(|| Box::new(WgpuRenderEngine::new()));
    tree
}

/// Applies the main window's settings: title, the design's size, the
/// smallest size the three-panel layout supports, and no system frame (the
/// shell draws the title bar and asks the system for rounded corners).
pub fn frameless(window: WinitWindow) -> WinitWindow {
    window
        .with_title(TITLE)
        .with_size(WINDOW_WIDTH, WINDOW_HEIGHT)
        .with_min_size(WINDOW_MIN_WIDTH, WINDOW_MIN_HEIGHT)
        .with_decorations(false)
        .with_rounded_corners(true)
}

/// Opens the main window on `model` and runs until it is closed.
pub fn run(model: ShellModel) {
    let mut app = app();
    let mut tree = tree();
    tree.mount_root::<Shell>(&mut app, model);
    frameless(WinitWindow::new(app, tree)).run();
}
