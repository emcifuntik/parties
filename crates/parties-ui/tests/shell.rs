//! Headless tests of the shell: the tree is built and laid out without a
//! window, and driven with the same pointer input a person gives it.

use lurq::app::{App, Tree, events::MouseButton};
use parties_ui::{
    Command, Shell, ShellModel,
    fixtures::{self, CHANNEL_CLIPS, CHANNEL_OFF_TOPIC, Pictures, USER_TUXICK},
    model::{Presence, StreamSource, VoiceBlockMenu},
    theme::{self, LightId, Tokens},
};

const WIDTH: u32 = 1440;
const HEIGHT: u32 = 900;

struct Harness {
    app: App,
    tree: Tree,
    model: ShellModel,
}

impl Harness {
    fn new() -> Self {
        let mut app = App::new();
        theme::install(&mut app);
        let model = ShellModel::new(fixtures::glass_shell(&Pictures::none()));
        let mut tree = Tree::new();
        tree.resize(WIDTH, HEIGHT);
        tree.mount_root::<Shell>(&mut app, model.clone());
        let mut harness = Self { app, tree, model };
        harness.pass();
        harness
    }

    fn pass(&mut self) {
        self.tree.pass_headless(&mut self.app);
    }

    /// The bounds of the element with `id`: x, y, width, height.
    fn bounds(&mut self, id: &str) -> (f32, f32, f32, f32) {
        let bounds = self
            .tree
            .get_element_by_id_mut(id)
            .unwrap_or_else(|| panic!("no element with id {id}"))
            .bounds()
            .unwrap_or_else(|| panic!("element {id} is not laid out"));
        (bounds.x, bounds.y, bounds.width, bounds.height)
    }

    fn exists(&mut self, id: &str) -> bool {
        self.tree.get_element_by_id(id).is_some()
    }

    fn has_class(&mut self, id: &str, class: &str) -> bool {
        self.tree
            .get_element_by_id(id)
            .unwrap_or_else(|| panic!("no element with id {id}"))
            .has_class(class)
    }

    fn text(&mut self, id: &str) -> String {
        self.tree
            .get_element_by_id(id)
            .unwrap_or_else(|| panic!("no element with id {id}"))
            .text_content()
            .unwrap_or_default()
            .to_owned()
    }

    /// A left click in the middle of the element, through hit testing.
    fn click(&mut self, id: &str) {
        let (x, y, width, height) = self.bounds(id);
        let (x, y) = (x + width / 2.0, y + height / 2.0);
        self.tree.mouse_move(x, y);
        self.tree.mouse_down(x, y, MouseButton::Left);
        self.tree.mouse_up(x, y, MouseButton::Left);
        self.pass();
    }
}

#[test]
fn the_tree_builds_from_the_fixture_state() {
    let mut shell = Harness::new();

    // The three panels sit where the design puts them at 1440 x 900.
    assert_eq!(shell.bounds("left-panel"), (12.0, 52.0, 264.0, 836.0));
    assert_eq!(shell.bounds("chat-panel"), (288.0, 52.0, 864.0, 836.0));
    assert_eq!(shell.bounds("members-panel"), (1164.0, 52.0, 264.0, 836.0));

    // The voice block spans the left panel and ends 8 px above its edge.
    let (x, y, width, height) = shell.bounds("voice-block");
    assert_eq!((x, width), (20.0, 248.0));
    assert_eq!(y + height, 880.0);

    for id in [
        "title-bar",
        "switcher",
        "party-header",
        "room-1",
        "channel-2",
        "chat-header",
        "messages",
        "composer",
        "member-search",
        "member-1",
        "members-show-more",
    ] {
        assert!(shell.exists(id), "{id} is missing");
    }
    assert_eq!(shell.text("chat-title"), "clips-and-links");
    assert!(shell.has_class("channel-2", "selected-channel"));
}

#[test]
fn selecting_a_channel_changes_the_selected_row() {
    let mut shell = Harness::new();
    assert!(shell.has_class("channel-2", "selected-channel"));
    assert!(!shell.has_class("channel-3", "selected-channel"));

    shell.click("channel-3");

    assert_eq!(shell.model.snapshot().selected_channel, CHANNEL_OFF_TOPIC);
    assert!(shell.has_class("channel-3", "selected-channel"));
    assert!(!shell.has_class("channel-2", "selected-channel"));
    assert_eq!(shell.text("chat-title"), "off-topic");

    shell.click("channel-2");
    assert_eq!(shell.model.snapshot().selected_channel, CHANNEL_CLIPS);
    assert_eq!(shell.text("chat-title"), "clips-and-links");
}

#[test]
fn toggling_mute_changes_the_button_state() {
    let mut shell = Harness::new();
    assert!(!shell.has_class("mic-button", "off"));

    shell.click("mic-button");
    assert!(shell.model.snapshot().me.muted);
    assert!(shell.has_class("mic-button", "off"));
    assert!(!shell.has_class("sound-button", "off"));

    shell.click("sound-button");
    assert!(shell.model.snapshot().me.deafened);
    assert!(shell.has_class("sound-button", "off"));

    // Unmuting turns the sound back on too.
    shell.click("mic-button");
    assert!(!shell.has_class("mic-button", "off"));
    assert!(!shell.has_class("sound-button", "off"));
}

#[test]
fn choosing_idle_in_the_presence_menu_changes_the_presence() {
    let mut shell = Harness::new();
    assert!(!shell.exists("presence-menu"));

    shell.click("profile-line");
    assert_eq!(
        shell.model.snapshot().open_menu,
        Some(VoiceBlockMenu::Presence)
    );
    assert!(shell.has_class("presence-online", "current"));

    // The menu is as wide as the voice block and sits 6 px above it.
    let (block_x, block_y, block_width, _) = shell.bounds("voice-block");
    let (x, y, width, height) = shell.bounds("presence-menu");
    assert_eq!((x, width), (block_x, block_width));
    assert_eq!(y + height, block_y - 6.0);

    shell.click("presence-idle");

    let state = shell.model.snapshot();
    assert_eq!(state.me.presence, Presence::Idle);
    assert_eq!(state.open_menu, None);
    let own_row = state
        .members
        .groups
        .iter()
        .flat_map(|group| &group.members)
        .find(|member| member.user == USER_TUXICK)
        .expect("the fixture lists the user among the members");
    assert_eq!(own_row.presence, Presence::Idle);
    assert!(!shell.exists("presence-menu"));

    // Opening it again shows the new presence as the current one.
    shell.click("profile-line");
    assert!(shell.has_class("presence-idle", "current"));
    assert!(!shell.has_class("presence-online", "current"));
}

#[test]
fn the_stream_menu_opens_upwards_and_marks_what_is_live() {
    let mut shell = Harness::new();
    assert!(!shell.exists("stream-menu"));

    shell.click("stream-button");
    assert_eq!(
        shell.model.snapshot().open_menu,
        Some(VoiceBlockMenu::Stream)
    );
    let (button_x, button_y, button_width, _) = shell.bounds("stream-button");
    let (x, y, width, height) = shell.bounds("stream-menu");
    assert_eq!((x, width), (button_x, button_width));
    assert_eq!(y + height, button_y - 6.0);

    // Choosing a source starts it and leaves the menu open.
    shell.click("stream-screen");
    let state = shell.model.snapshot();
    assert!(state.me.streams.is_live(StreamSource::Screen));
    assert_eq!(state.open_menu, Some(VoiceBlockMenu::Stream));
    assert!(shell.has_class("stream-screen", "live"));
    assert!(!shell.has_class("stream-camera", "live"));

    // The button closes it again.
    shell.click("stream-button");
    assert_eq!(shell.model.snapshot().open_menu, None);
    assert!(!shell.exists("stream-menu"));
}

#[test]
fn the_two_menus_are_never_open_together() {
    let mut shell = Harness::new();

    shell.click("profile-line");
    assert!(shell.exists("presence-menu"));

    // One press on the other trigger switches menus. (The other way round
    // cannot be clicked: the open Stream menu covers the profile line.)
    shell.click("stream-button");
    assert_eq!(
        shell.model.snapshot().open_menu,
        Some(VoiceBlockMenu::Stream)
    );
    assert!(shell.exists("stream-menu"));
    assert!(!shell.exists("presence-menu"));

    shell.model.dispatch(Command::TogglePresenceMenu);
    shell.pass();
    assert_eq!(
        shell.model.snapshot().open_menu,
        Some(VoiceBlockMenu::Presence)
    );
    assert!(shell.exists("presence-menu"));
    assert!(!shell.exists("stream-menu"));
}

#[test]
fn a_press_outside_closes_the_open_menu() {
    let mut shell = Harness::new();
    shell.click("profile-line");
    assert!(shell.exists("presence-menu"));

    shell.click("chat-header");
    assert_eq!(shell.model.snapshot().open_menu, None);
    assert!(!shell.exists("presence-menu"));
}

#[test]
fn leaving_the_room_updates_the_voice_block() {
    let mut shell = Harness::new();
    assert_eq!(shell.tree.get_elements_by_class_name("lit-room").len(), 1);

    shell.click("leave-button");
    assert_eq!(shell.model.snapshot().me.room, None);
    assert!(shell.tree.get_elements_by_class_name("lit-room").is_empty());

    // Outside a room the Stream button does nothing.
    shell.click("stream-button");
    assert_eq!(shell.model.snapshot().open_menu, None);

    // A click on a room joins it.
    shell.click("room-2");
    assert_eq!(
        shell.model.snapshot().me.room.map(|room| room.name),
        Some("Late night".to_owned())
    );
    assert_eq!(shell.tree.get_elements_by_class_name("lit-room").len(), 1);
}

#[test]
fn the_light_is_switched_by_one_command() {
    let mut shell = Harness::new();
    let accent = |shell: &mut Harness| {
        shell
            .tree
            .get_element_by_id("brand-mark")
            .expect("the title bar has the brand mark")
            .color()
    };
    assert_eq!(
        accent(&mut shell),
        Some(Tokens::for_light(LightId::Night).accent)
    );

    shell.model.dispatch(Command::SetLight(LightId::Ember));
    shell.pass();
    assert_eq!(
        accent(&mut shell),
        Some(Tokens::for_light(LightId::Ember).accent)
    );

    // The brand mark steps to the next light.
    shell.click("brand-mark");
    assert_eq!(shell.model.snapshot().light, LightId::Smoke);
    assert_eq!(
        accent(&mut shell),
        Some(Tokens::for_light(LightId::Smoke).accent)
    );
}

#[test]
fn hiding_the_members_panel_gives_its_width_to_the_chat() {
    let mut shell = Harness::new();
    assert!(shell.has_class("chat-members", "on"));

    shell.click("chat-members");
    assert!(!shell.exists("members-panel"));
    assert_eq!(shell.bounds("chat-panel"), (288.0, 52.0, 1140.0, 836.0));
    assert!(!shell.has_class("chat-members", "on"));
}

#[test]
fn a_reaction_toggles_on_click() {
    let mut shell = Harness::new();
    let mine = |shell: &mut Harness| shell.tree.get_elements_by_class_name("mine").len();
    assert_eq!(mine(&mut shell), 1);

    let (x, y) = {
        let reactions = shell.tree.get_elements_by_class_name("reaction");
        assert_eq!(reactions.len(), 2);
        // The second chip is the one the user has not reacted with.
        let id = reactions[1].node_id();
        drop(reactions);
        let found = shell
            .tree
            .find_element_mut(|element| element.node_id() == id)
            .expect("the reaction chip is in the tree");
        found.bounds().center()
    };
    shell.tree.mouse_down(x, y, MouseButton::Left);
    shell.tree.mouse_up(x, y, MouseButton::Left);
    shell.pass();

    assert_eq!(mine(&mut shell), 2);
}

#[test]
fn message_text_can_be_selected_with_the_pointer() {
    let mut shell = Harness::new();
    assert_eq!(shell.tree.selected_text(), None);

    // Drag across the first message, from its first letter to past its end.
    let (x, y, width, height) = shell.bounds("message-text-1");
    let middle = y + height / 2.0;
    shell.tree.mouse_move(x + 1.0, middle);
    shell.tree.mouse_down(x + 1.0, middle, MouseButton::Left);
    shell.tree.mouse_move(x + width - 1.0, middle);
    shell
        .tree
        .mouse_up(x + width - 1.0, middle, MouseButton::Left);
    shell.pass();

    assert_eq!(
        shell.tree.selected_text().as_deref(),
        Some("Final scoreboard from last night. The 1v3 at the end still makes no sense.")
    );
}

#[test]
fn the_layout_holds_at_the_smallest_supported_window() {
    use parties_ui::theme::metrics::{WINDOW_MIN_HEIGHT, WINDOW_MIN_WIDTH};

    let mut shell = Harness::new();
    shell.tree.resize(WINDOW_MIN_WIDTH, WINDOW_MIN_HEIGHT);
    shell.pass();

    let (width, height) = (WINDOW_MIN_WIDTH as f32, WINDOW_MIN_HEIGHT as f32);
    // The side panels keep their width; the chat takes what is left.
    assert_eq!(
        shell.bounds("left-panel"),
        (12.0, 52.0, 264.0, height - 64.0)
    );
    assert_eq!(
        shell.bounds("chat-panel"),
        (288.0, 52.0, width - 576.0, height - 64.0)
    );
    assert_eq!(
        shell.bounds("members-panel"),
        (width - 276.0, 52.0, 264.0, height - 64.0)
    );
    // The composer stays inside the chat panel, 16 px above its bottom edge.
    let (_, y, _, composer_height) = shell.bounds("composer");
    assert_eq!(y + composer_height, height - 12.0 - 16.0);
    // The voice block still ends 8 px above the left panel's edge.
    let (_, y, _, block_height) = shell.bounds("voice-block");
    assert_eq!(y + block_height, height - 12.0 - 8.0);
}
