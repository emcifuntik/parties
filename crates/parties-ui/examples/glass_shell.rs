//! Opens the main window of the Glass design on fixture data.
//!
//! ```text
//! cargo run -p parties-ui --example glass_shell --target-dir target/parties-ui
//! cargo run -p parties-ui --example glass_shell --target-dir target/parties-ui -- --capture target/ui-shots/glass_shell.png
//! ```
//!
//! Options:
//!
//! - `--capture <path>`: save the composed window to a PNG and exit.
//! - `--menu stream|presence`: start with that menu open above the voice block.
//! - `--light <name>`: Night, Ember, Smoke, Ocean, Dusk, Rose, Honey or Moss.
//! - `--profile`: print the timings of every presented frame, and a summary
//!   at exit (with the GPU time of the frames, where the renderer measures
//!   it). An idle window prints nothing: it does not redraw.
//! - `--size <width>x<height>`: the window's size in logical pixels
//!   (1440x900 when omitted).
//! - `--seconds <n>`: close the window after `n` seconds.
//! - `--exercise`: send a command every 250 ms (mute, channel, light), to
//!   measure frames that change something.
//! - `--sweep`: move the pointer in a loop over the message list, the
//!   members, the channels and a menu that is open, to measure frames that
//!   hover changes ask for. The loop polls while it runs, so use it to
//!   measure frames, not idleness.
//! - `--no-blur`: draw the glass without its backdrop blur (the menus are
//!   then opaque), to measure what the blur costs.
//! - `--renderer wgpu|dx12`: wgpu, which the application uses and is the
//!   default, or lurq's own Direct3D 12 renderer (Windows only).
//!
//! The capture is in physical pixels: on a display scaled to 150% the
//! 1440 x 900 window gives a 2160 x 1350 picture. To capture at 100% on such
//! a display, start the example with the environment variable
//! `__COMPAT_LAYER=DpiUnaware` (Windows only).

use std::{
    path::PathBuf,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

use lurq::{
    app::{
        Tree,
        component::Component,
        ctx::{Ctx, Interval, Timeout},
        profiler::{RecordingOptions, SampleData},
        winit_shell::WinitWindow,
    },
    node::Element,
};
use parties_ui::{
    Command, Shell, ShellModel,
    fixtures::{self, CHANNEL_CLIPS, CHANNEL_GENERAL, Pictures},
    model::VoiceBlockMenu,
    theme::LightId,
    window,
};

/// The earliest moment of a capture: the window has been shown and laid out.
const CAPTURE_AFTER: Duration = Duration::from_millis(700);
/// How long the frames that show the decoded pictures get before the capture.
const CAPTURE_SETTLE: Duration = Duration::from_millis(400);
/// How often the harness checks where the capture stands.
const CAPTURE_POLL: Duration = Duration::from_millis(100);
/// How often `--exercise` sends a command.
const EXERCISE_EVERY: Duration = Duration::from_millis(250);
/// How fast `--sweep` moves the pointer, in logical pixels a second: a quick
/// flick, which crosses a list row in about one frame.
const SWEEP_SPEED: f32 = 2400.0;
/// How often `--sweep` reports a pointer position, as a 250 Hz mouse does.
const SWEEP_EVERY: Duration = Duration::from_millis(4);

#[derive(Clone, PartialEq)]
struct HarnessProps {
    model: ShellModel,
    capture: Option<PathBuf>,
    close_after: Option<Duration>,
    exercise: bool,
}

/// Where a `--capture` run stands.
#[derive(Clone, Copy)]
enum CaptureStage {
    /// Waiting for the first frames and for the pictures to decode.
    Settling,
    /// Every picture is decoded; waiting for the frames that show them.
    Decoded(Instant),
    /// The capture was requested; waiting for the file.
    Requested,
}

/// Wraps the shell with what a run without a human needs: a capture of the
/// frame, a time limit, and commands sent on a timer.
struct Harness {
    _timeouts: Vec<Timeout>,
    _intervals: Vec<Interval>,
}

impl Component for Harness {
    type Props = HarnessProps;

    fn create(ctx: &mut Ctx) -> Self {
        let props = ctx.props::<HarnessProps>().clone();
        let mut timeouts = Vec::new();
        let mut intervals = Vec::new();

        if let Some(path) = props.capture {
            // Pictures decode on worker threads. Capture once none is
            // pending and the frames that show them have been drawn, then
            // close when the file is there.
            let window = ctx.window();
            let started = Instant::now();
            let stage = Mutex::new(CaptureStage::Settling);
            let poll = ctx.create_interval(CAPTURE_POLL, move || {
                let mut stage = stage
                    .lock()
                    .unwrap_or_else(std::sync::PoisonError::into_inner);
                match *stage {
                    CaptureStage::Settling => {
                        let decoded = lurq::images::wait_for_decoded_images(Duration::ZERO);
                        if decoded && started.elapsed() >= CAPTURE_AFTER {
                            *stage = CaptureStage::Decoded(Instant::now());
                        }
                    }
                    CaptureStage::Decoded(at) => {
                        if at.elapsed() >= CAPTURE_SETTLE {
                            window.screenshot(path.clone());
                            *stage = CaptureStage::Requested;
                        }
                    }
                    CaptureStage::Requested => {
                        if std::fs::metadata(&path).is_ok_and(|file| file.len() > 0) {
                            window.close();
                        }
                    }
                }
            });
            poll.start();
            intervals.push(poll);
        }

        if let Some(after) = props.close_after {
            let window = ctx.window();
            let close = ctx.create_timeout(after, move || window.close());
            close.start();
            timeouts.push(close);
        }

        if props.exercise {
            let model = props.model.clone();
            let step = Mutex::new(0_u32);
            let exercise = ctx.create_interval(EXERCISE_EVERY, move || {
                let mut step = step
                    .lock()
                    .unwrap_or_else(std::sync::PoisonError::into_inner);
                model.dispatch(match *step % 4 {
                    0 => Command::ToggleMute,
                    1 => Command::SelectChannel(CHANNEL_GENERAL),
                    2 => Command::SelectChannel(CHANNEL_CLIPS),
                    _ => Command::CycleLight,
                });
                *step += 1;
            });
            exercise.start();
            intervals.push(exercise);
        }

        Self {
            _timeouts: timeouts,
            _intervals: intervals,
        }
    }

    fn render(&self, ctx: &mut Ctx) -> impl Into<Element> {
        let model = ctx.props::<HarnessProps>().model.clone();
        ctx.mount::<Shell>(model)
    }
}

/// One straight run of the pointer, in logical pixels, named by the part of
/// the window it crosses.
struct Run {
    over: &'static str,
    from: (f32, f32),
    to: (f32, f32),
}

impl Run {
    fn length(&self) -> f32 {
        (self.to.0 - self.from.0).hypot(self.to.1 - self.from.1)
    }
}

/// The pointer's loop of `--sweep`: where it is at any moment after the
/// window opened.
struct Sweep {
    started: Instant,
    last_move: Option<Instant>,
    /// The part of the window the pointer was last over, to name each once.
    over: Option<&'static str>,
}

impl Sweep {
    fn new() -> Self {
        Self {
            started: Instant::now(),
            last_move: None,
            over: None,
        }
    }

    /// The loop as it lies in the window now. `None` until the window is
    /// laid out.
    fn runs(tree: &mut Tree) -> Option<Vec<Run>> {
        let mut bounds = |id: &str| tree.get_element_by_id_mut(id)?.bounds();
        let messages = bounds("messages")?;
        let members = bounds("members-panel");
        let left = bounds("left-panel")?;
        let (top, bottom) = (messages.y + 8.0, messages.y + messages.height - 8.0);
        // Down the pictures, up the attachments (through the file card's
        // download button, 384 px in), down the right edge.
        let columns = [
            (60.0, top, bottom),
            (384.0, bottom, top),
            (messages.width - 60.0, top, bottom),
        ];
        let mut runs: Vec<Run> = columns
            .into_iter()
            .map(|(column, from, to)| Run {
                over: "the message list",
                from: (messages.x + column, from),
                to: (messages.x + column, to),
            })
            .collect();
        // Down the middle of each side panel: every row, and a menu that is
        // open above the voice block.
        for (over, panel) in [("the members", members), ("the channels", Some(left))] {
            let Some(panel) = panel else { continue };
            let x = panel.x + panel.width / 2.0;
            runs.push(Run {
                over,
                from: (x, panel.y + 8.0),
                to: (x, panel.y + panel.height - 8.0),
            });
        }
        Some(runs)
    }

    /// Moves the pointer to where the loop is now.
    fn tick(&mut self, tree: &mut Tree) {
        let now = Instant::now();
        if self
            .last_move
            .is_some_and(|last| now.duration_since(last) < SWEEP_EVERY)
        {
            return;
        }
        let Some(runs) = Self::runs(tree) else { return };
        self.last_move = Some(now);
        // The pointer jumps from the end of a run to the start of the next.
        let total: f32 = runs.iter().map(Run::length).sum();
        if total <= 0.0 {
            return;
        }
        let mut along = (self.started.elapsed().as_secs_f32() * SWEEP_SPEED) % total;
        for run in runs {
            let length = run.length();
            if along > length {
                along -= length;
                continue;
            }
            if self.over != Some(run.over) {
                self.over = Some(run.over);
                println!(
                    "sweep at {:>7.1} ms: the pointer is over {}",
                    millis(self.started.elapsed()),
                    run.over
                );
            }
            let t = if length > 0.0 { along / length } else { 0.0 };
            // The tree takes the pointer in physical pixels, as the window
            // system gives it.
            let scale = tree.scale_factor();
            tree.mouse_move(
                (run.from.0 + (run.to.0 - run.from.0) * t) * scale,
                (run.from.1 + (run.to.1 - run.from.1) * t) * scale,
            );
            return;
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Renderer {
    Wgpu,
    Dx12,
}

/// Makes `tree` draw with lurq's own Direct3D 12 renderer.
#[cfg(windows)]
fn use_dx12(tree: &mut Tree) {
    tree.set_render_engine_factory(|| Box::new(lurq::app::dx12_render::Dx12RenderEngine::new()));
}

/// Only Windows has that renderer; `parse_options` refuses it elsewhere.
#[cfg(not(windows))]
fn use_dx12(_tree: &mut Tree) {}

struct Options {
    capture: Option<PathBuf>,
    menu: Option<VoiceBlockMenu>,
    light: LightId,
    profile: bool,
    close_after: Option<Duration>,
    exercise: bool,
    sweep: bool,
    backdrop_blur: bool,
    renderer: Renderer,
    size: Option<(u32, u32)>,
}

fn usage(problem: &str) -> ! {
    eprintln!("{problem}");
    eprintln!(
        "usage: glass_shell [--capture <path>] [--menu stream|presence] [--light <name>] [--profile] [--seconds \
         <n>] [--exercise] [--sweep] [--no-blur] [--renderer wgpu|dx12] [--size <width>x<height>]"
    );
    std::process::exit(2);
}

fn parse_options() -> Options {
    let mut options = Options {
        capture: None,
        menu: None,
        light: LightId::Night,
        profile: false,
        close_after: None,
        exercise: false,
        sweep: false,
        backdrop_blur: true,
        renderer: Renderer::Wgpu,
        size: None,
    };
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let mut value = |name: &str| {
            args.next()
                .unwrap_or_else(|| usage(&format!("{name} needs a value")))
        };
        match arg.as_str() {
            "--capture" => options.capture = Some(PathBuf::from(value("--capture"))),
            "--menu" => {
                options.menu = Some(match value("--menu").as_str() {
                    "stream" => VoiceBlockMenu::Stream,
                    "presence" => VoiceBlockMenu::Presence,
                    other => usage(&format!("unknown menu {other}: use stream or presence")),
                });
            }
            "--light" => {
                let name = value("--light");
                options.light = LightId::from_name(&name)
                    .unwrap_or_else(|| usage(&format!("unknown light {name}")));
            }
            "--profile" => options.profile = true,
            "--seconds" => {
                let seconds: u64 = value("--seconds")
                    .parse()
                    .unwrap_or_else(|_| usage("--seconds takes whole seconds"));
                options.close_after = Some(Duration::from_secs(seconds));
            }
            "--exercise" => options.exercise = true,
            "--sweep" => options.sweep = true,
            "--no-blur" => options.backdrop_blur = false,
            "--renderer" => {
                options.renderer = match value("--renderer").as_str() {
                    "wgpu" => Renderer::Wgpu,
                    "dx12" if cfg!(windows) => Renderer::Dx12,
                    "dx12" => usage("the dx12 renderer is for Windows only"),
                    other => usage(&format!("unknown renderer {other}: use wgpu or dx12")),
                };
            }
            "--size" => {
                let size = value("--size");
                options.size = size
                    .split_once('x')
                    .and_then(|(width, height)| Some((width.parse().ok()?, height.parse().ok()?)))
                    .or_else(|| usage("--size takes <width>x<height>, for example 1020x640"));
            }
            other => usage(&format!("unknown option {other}")),
        }
    }
    options
}

/// What `--profile` collects from the frames the window presented.
///
/// A frame's time has two parts: the work (layout, building the draw list,
/// encoding and submitting it) and the wait (acquiring the swap chain image
/// and presenting, which block on the compositor and the display).
#[derive(Default)]
struct FrameStats {
    frames: u32,
    /// The first frame, which creates the GPU device and compiles shaders.
    first_work: Duration,
    /// Work of each frame after the first.
    work: Vec<Duration>,
    wait: Duration,
    draw_calls: usize,
}

fn millis(duration: Duration) -> f64 {
    duration.as_secs_f64() * 1000.0
}

/// Mean, median and worst of `times`, in milliseconds; zeros when empty.
fn spread(times: &mut [Duration]) -> (f64, f64, f64) {
    times.sort_unstable();
    let (Some(median), Some(worst)) = (times.get(times.len() / 2), times.last()) else {
        return (0.0, 0.0, 0.0);
    };
    let total: Duration = times.iter().sum();
    (
        millis(total) / times.len() as f64,
        millis(*median),
        millis(*worst),
    )
}

fn main() {
    let options = parse_options();

    // The design's pictures are files Pencil generated into the repository's
    // `images/` folder. Without them the fixture falls back to monograms.
    let images = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../images");
    let mut state = fixtures::glass_shell(&Pictures::from_dir(images));
    state.light = options.light;
    state.open_menu = options.menu;
    state.backdrop_blur = options.backdrop_blur;
    let model = ShellModel::new(state);

    if let Some(path) = &options.capture {
        // The harness waits for this file to appear.
        let _ = std::fs::remove_file(path);
    }

    let mut app = window::app();
    let mut tree = window::tree();
    if options.renderer == Renderer::Dx12 {
        use_dx12(&mut tree);
    }
    // A recording of lurq's profiler has the GPU time of the frames, which
    // arrives a frame or more after each of them.
    let recording = options.profile.then(|| {
        let profiling = tree.profiling_handle();
        let session = profiling
            .start_recording(RecordingOptions::default())
            .map(|started| started.id);
        (profiling, session)
    });
    tree.mount_root::<Harness>(
        &mut app,
        HarnessProps {
            model,
            capture: options.capture.clone(),
            close_after: options.close_after,
            exercise: options.exercise,
        },
    );

    let mut shell = window::frameless(WinitWindow::new(app, tree));
    if let Some((width, height)) = options.size {
        shell = shell.with_size(width, height);
    }
    let mut shell = shell
        // A run that ends by itself should not take the keyboard from whoever
        // is working.
        .with_start_without_focus(options.capture.is_some() || options.close_after.is_some());
    if options.sweep {
        let mut sweep = Sweep::new();
        shell = shell.on_tick(move |tree, _since_last| sweep.tick(tree));
    }

    let stats = Arc::new(Mutex::new(FrameStats::default()));
    if options.profile {
        let stats = stats.clone();
        let started = Instant::now();
        shell = shell.on_paint(move |tree, _since_last, report| {
            let profile = tree.last_profile();
            let wait = profile.render.acquire + profile.render.present;
            let work = profile.total.saturating_sub(wait);
            let reasons: Vec<&str> = report.reasons.names().collect();
            println!(
                "frame at {:>7.1} ms: work {:>6.2} ms (layout {:.2}{}, draw list {:.2}, glyphs {:.2}, encode {:.2}, submit {:.2}), wait {:>6.2} ms, {} draw calls, {} quads, {} glyphs, because {}",
                millis(started.elapsed()),
                millis(work),
                millis(profile.layout),
                if profile.layout_recalculated { "" } else { " cached" },
                millis(profile.quad_resolve),
                millis(profile.glyph_rasterize),
                millis(profile.render.encode),
                millis(profile.render.submit),
                millis(wait),
                profile.render.draw_calls,
                profile.quad_count,
                profile.glyph_count,
                if reasons.is_empty() {
                    "nothing".to_owned()
                } else {
                    reasons.join("+")
                },
            );
            let mut stats = stats.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
            if stats.frames == 0 {
                stats.first_work = work;
            } else {
                stats.work.push(work);
            }
            stats.frames += 1;
            stats.wait += wait;
            stats.draw_calls = stats.draw_calls.max(profile.render.draw_calls);
        });
    }

    shell.run();

    if options.profile {
        let mut stats = stats
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let (mean, median, worst) = spread(&mut stats.work);
        println!(
            "{} frames. First frame: {:.1} ms of work. After it: mean {mean:.2} ms, median {median:.2} ms, worst {worst:.2} ms of work per frame; mean wait for the display {:.1} ms; at most {} draw calls.",
            stats.frames,
            millis(stats.first_work),
            millis(stats.wait) / f64::from(stats.frames.max(1)),
            stats.draw_calls,
        );
    }

    if let Some((profiling, session)) = recording {
        match session.and_then(|id| profiling.end(id)) {
            Ok(report) => {
                // The first frame is left out here too.
                let mut gpu: Vec<Duration> = report
                    .samples
                    .iter()
                    .filter_map(|sample| match &sample.data {
                        SampleData::Pass(pass) if pass.rendered => Some(pass.gpu),
                        _ => None,
                    })
                    .skip(1)
                    .flatten()
                    .map(|timing| timing.total())
                    .collect();
                let availability = report.build.gpu_timestamps;
                if gpu.is_empty() {
                    println!(
                        "GPU time: not measured ({}).",
                        availability
                            .reason
                            .unwrap_or("no frame's result arrived before the window closed")
                    );
                } else {
                    let frames = gpu.len();
                    let (mean, median, worst) = spread(&mut gpu);
                    println!(
                        "GPU time of {frames} frames after the first ({}): mean {mean:.2} ms, median {median:.2} ms, worst {worst:.2} ms.",
                        availability.adapter_backend.unwrap_or("unknown backend"),
                    );
                }
            }
            Err(error) => println!("GPU time: the profiler gave no report ({error})."),
        }
    }
}
