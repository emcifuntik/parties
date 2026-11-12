//! The Glass theme: lights, colour tokens and typography roles.
//!
//! - [`lights`] is the table of the eight lights (plain data).
//! - [`tokens`] resolves a light into every colour, radius and surface the
//!   components use.
//! - [`typography`] defines the text roles and registers them with lurq.
//!
//! The light is switched with one call: build `Tokens::for_light(id)` and
//! provide it to the tree, which is what the shell does whenever the light in
//! the view model changes.

pub mod color;
pub mod lights;
pub mod metrics;
pub mod tokens;
pub mod typography;

pub use lights::{LIGHTS, Light, LightId};
pub use tokens::{Glass, Surface, Tokens};
pub use typography::TextRole;

/// Prepares a lurq [`App`](lurq::app::App) for the Parties UI: registers the
/// font families and the typography roles. Colours do not live in lurq's
/// theme; they travel as [`Tokens`] through the component context.
pub fn install(app: &mut lurq::app::App) {
    typography::install(app);
}
