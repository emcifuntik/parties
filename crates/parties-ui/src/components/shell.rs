//! The root of the main window: backdrop, custom window chrome, and the
//! view the switcher is on (Home, Settings, or a party with its three glass
//! panels).

use lurq::{
    app::{component::Component, ctx::Ctx},
    components::{
        ChromeBorderPolicy, ChromeTitleBar, Column, Rect, ResizeHandlePlacement,
        ResizeHandlePolicy, Row, Stack, WindowChrome, WindowChromeMode, WindowChromeProps,
    },
    node::{Element, Gradient, GradientStop, color::Color},
};

use super::{
    channel_list::ChannelList,
    chat_panel::ChatPanel,
    home::Home,
    member_list::MemberList,
    settings::Settings,
    surface::{FILL, glass},
    title_bar::TitleBar,
    voice_block::VoiceBlock,
};
use crate::{
    model::View,
    theme::{
        Tokens,
        metrics::{PANEL_GAP, PANEL_PADDING, RESIZE_ZONE, SIDE_PANEL_WIDTH, TITLE_BAR_HEIGHT},
        tokens::{Backdrop, Wash},
    },
    view_model::ShellModel,
};

const TRANSPARENT: Color = Color::new(0, 0, 0, 0);

/// The main window. Mount it as the root with the window's [`ShellModel`].
pub struct Shell;

impl Component for Shell {
    type Props = ShellModel;

    fn create(_ctx: &mut Ctx) -> Self {
        Self
    }

    fn render(&self, ctx: &mut Ctx) -> impl Into<Element> {
        let model = ctx.props::<ShellModel>().clone();
        // The one place the light becomes colours: every component below
        // reads these tokens from the context.
        let tokens = Tokens::for_light(model.light());
        let tokens = if model.backdrop_blur() {
            tokens
        } else {
            tokens.without_backdrop_blur()
        };
        ctx.provide_if_changed(tokens);

        let window = ctx.window();
        let backdrop = backdrop(
            &tokens.backdrop,
            window.logical_width(),
            window.logical_height(),
        );

        let body: Element = match model.switcher().view {
            View::Home => ctx.mount::<Home>(model.clone()),
            View::Settings => ctx.mount::<Settings>(model.clone()),
            View::Party(_) => party_view(ctx, &tokens, &model).into(),
        };

        // lurq's chrome supplies the drag region, the double-click to
        // maximise and the resize zones. Its own frame is made transparent
        // so the backdrop shows under the title bar too.
        let chrome = WindowChrome::new()
            .props(
                WindowChromeProps::new()
                    .mode(WindowChromeMode::AlwaysCustom)
                    .windows_height(TITLE_BAR_HEIGHT)
                    .macos_height(TITLE_BAR_HEIGHT)
                    .resize_handles(ResizeHandlePolicy::Enabled { size: RESIZE_ZONE })
                    .resize_placement(ResizeHandlePlacement::Overlay)
                    .border(ChromeBorderPolicy::Hidden),
            )
            .frame_background(TRANSPARENT)
            .title_bar(
                ChromeTitleBar::new()
                    .without_controls()
                    .height(TITLE_BAR_HEIGHT)
                    .background(TRANSPARENT)
                    .border_bottom(None)
                    .center(ctx.mount::<TitleBar>(model.clone())),
            )
            .content(body)
            .mount(ctx);

        backdrop.child(chrome)
    }
}

/// A party: the channels and the voice block, the chat, and the members.
fn party_view(ctx: &mut Ctx, tokens: &Tokens, model: &ShellModel) -> Row {
    let left = glass(
        &tokens.panel,
        Column::new()
            .width(FILL)
            .height(FILL)
            .padding(PANEL_PADDING)
            .spacing(PANEL_PADDING)
            .child(ctx.mount::<ChannelList>(model.clone()))
            .child(ctx.mount::<VoiceBlock>(model.clone())),
    )
    .id("left-panel")
    .width(SIDE_PANEL_WIDTH)
    .height(FILL);

    let chat = glass(&tokens.panel_reading, ctx.mount::<ChatPanel>(model.clone()))
        .id("chat-panel")
        .flex(1.0)
        .height(FILL);

    let mut body = Row::new()
        .width(FILL)
        .height(FILL)
        .padding_left(PANEL_GAP)
        .padding_right(PANEL_GAP)
        .padding_bottom(PANEL_GAP)
        .spacing(PANEL_GAP)
        .child(left)
        .child(chat);
    if model.members_panel_open() {
        body = body.child(
            glass(&tokens.panel, ctx.mount::<MemberList>(model.clone()))
                .id("members-panel")
                .width(SIDE_PANEL_WIDTH)
                .height(FILL),
        );
    }
    body
}

/// The window backdrop: the base colour and the two radial washes.
fn backdrop(backdrop: &Backdrop, width: f32, height: f32) -> Stack {
    Stack::new()
        .id("backdrop")
        .width(FILL)
        .height(FILL)
        .background(backdrop.base)
        .clip()
        .child(wash(&backdrop.wash_top_left, width, height))
        .child(wash(&backdrop.wash_bottom_right, width, height))
}

/// One wash: an ellipse given in fractions of the window.
///
/// lurq's radial gradient reaches its last stop at the farthest corner of
/// the element, which for a centred gradient is `sqrt(2)` times the
/// half-size. The element is the bounding box of the design's ellipse, so
/// the stop positions are divided by `sqrt(2)` to end on the ellipse, and
/// the corners beyond it stay clear.
fn wash(wash: &Wash, width: f32, height: f32) -> Rect {
    let (ellipse_width, ellipse_height) = (wash.size.0 * width, wash.size.1 * height);
    let x = wash.center.0 * width - ellipse_width / 2.0;
    let y = wash.center.1 * height - ellipse_height / 2.0;
    let to_corner = std::f32::consts::FRAC_1_SQRT_2;
    let mut stops: Vec<GradientStop> = wash
        .stops
        .iter()
        .map(|(color, position)| GradientStop::at(*color, position * to_corner))
        .collect();
    if let Some((edge, _)) = wash.stops.last() {
        stops.push(GradientStop::at(*edge, 1.0));
    }
    Rect::new(ellipse_width, ellipse_height)
        .absolute(x, y, ellipse_width, ellipse_height)
        .background_gradient(Gradient::radial(stops))
        .pointer_events_none()
}
