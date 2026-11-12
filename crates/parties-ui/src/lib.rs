//! Views and view models of the Parties client, built with the lurq toolkit.
//!
//! This crate draws; it does not decide. The split:
//!
//! - [`model`]: what the main window shows, as plain data ([`ShellState`]).
//! - [`commands`]: what the user can ask for ([`Command`]).
//! - [`view_model`]: [`ShellModel`], the state as reactive signals. Views read
//!   it; the owner of the state writes it with `apply`; controls call
//!   `dispatch`.
//! - [`components`]: the views, rooted at [`Shell`].
//! - [`theme`]: lights, colour tokens and typography roles.
//! - [`fixtures`]: the design's chat window as data, and an in-memory
//!   stand-in for the client core, until the real one exists.
//! - [`window`] (feature `window`): the frameless window the shell runs in.
//!
//! ```no_run
//! use parties_ui::{ShellModel, fixtures};
//!
//! let model = ShellModel::new(fixtures::glass_shell(&fixtures::Pictures::none()));
//! # #[cfg(feature = "window")]
//! parties_ui::window::run(model);
//! ```

pub mod commands;
pub mod components;
pub mod fixtures;
pub mod model;
pub mod theme;
pub mod view_model;
#[cfg(feature = "window")]
pub mod window;

pub use commands::Command;
pub use components::shell::Shell;
pub use model::ShellState;
pub use view_model::ShellModel;
