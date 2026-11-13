//! Headless tests of the shell: the tree is built and laid out without a
//! window, and driven with the same pointer input a person gives it.

use lurq::app::{App, Tree, events::MouseButton};
use parties_ui::{
    Command, Shell, ShellModel,
    fixtures::{
        self, CHANNEL_CLIPS, CHANNEL_OFF_TOPIC, NIGHT_SHIFT, PARTY_OLD_GUILD, Pictures, USER_TUXICK,
    },
    model::{
        AppSwitch, CodecChoice, HotkeyAction, InputMode, PartyLink, Presence, SettingsPage,
        ShellState, StreamSource, UpdateChannel, View, VoiceBlockMenu,
    },
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
    /// The window on a party, as the chat frame of the design draws it.
    fn new() -> Self {
        Self::with(fixtures::glass_shell(&Pictures::none()))
    }

    /// The window on Home, as the Home frame of the design draws it.
    fn home() -> Self {
        Self::with(fixtures::glass_home(&Pictures::none()))
    }

    /// The window on a page of Settings.
    fn settings(page: SettingsPage) -> Self {
        Self::with(fixtures::glass_settings(&Pictures::none(), page))
    }

    fn with(state: ShellState) -> Self {
        let mut app = App::new();
        theme::install(&mut app);
        let model = ShellModel::new(state);
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

// --- Home ---

#[test]
fn home_sits_where_the_design_draws_it() {
    let mut shell = Harness::home();

    // The page is 1136 px wide and centred; the cards start 170 px under the
    // title bar.
    assert_eq!(shell.bounds("home-parties"), (152.0, 222.0, 744.0, 450.0));
    assert_eq!(shell.bounds("home-identity"), (908.0, 222.0, 380.0, 206.0));
    assert_eq!(shell.bounds("home-add"), (908.0, 440.0, 380.0, 232.0));
    let (x, _, _, _) = shell.bounds("home-title");
    assert_eq!(x, 156.0);

    assert_eq!(shell.text("home-online"), "2 of 4 online");
    assert_eq!(shell.text("home-fingerprint"), "5E:7A:91:C2:4D:3F:08:B1");
    assert_eq!(shell.text("home-key"), "Signed in with a hardware key");
    // The party the client is connected to is the lit row, with its button.
    assert!(shell.has_class("party-1", "connected"));
    assert!(shell.exists("party-open-1"));
    assert!(!shell.has_class("party-2", "connected"));
    // Only an offline party can be retried.
    assert!(shell.exists("party-retry-3"));
    assert!(!shell.exists("party-retry-4"));
    // The party's panels are not built while Home is in view.
    assert!(!shell.exists("left-panel"));
}

#[test]
fn the_switcher_and_the_open_button_move_between_home_and_a_party() {
    let mut shell = Harness::new();
    assert!(shell.exists("left-panel"));
    assert!(!shell.exists("home"));

    shell.click("switch-home");
    assert_eq!(shell.model.snapshot().switcher.view, View::Home);
    assert!(shell.exists("home"));
    assert!(!shell.exists("left-panel"));

    shell.click("party-open-1");
    assert_eq!(
        shell.model.snapshot().switcher.view,
        View::Party(NIGHT_SHIFT)
    );
    assert!(shell.exists("left-panel"));
    assert!(!shell.exists("home"));
}

#[test]
fn a_party_that_answers_opens_from_its_row() {
    let mut shell = Harness::home();
    shell.click("party-2");
    assert!(matches!(
        shell.model.snapshot().switcher.view,
        View::Party(party) if party != NIGHT_SHIFT
    ));
}

#[test]
fn retrying_an_offline_party_shows_it_reconnecting() {
    let mut shell = Harness::home();

    shell.click("party-retry-3");

    let state = shell.model.snapshot();
    let card = state
        .home
        .parties
        .iter()
        .find(|card| card.id == PARTY_OLD_GUILD)
        .expect("the fixture has the offline party");
    assert_eq!(card.link, PartyLink::Reconnecting { attempt: 1 });
    assert!(!shell.exists("party-retry-3"));
    // Still not online: the count under the title does not move.
    assert_eq!(shell.text("home-online"), "2 of 4 online");
}

#[test]
fn the_update_notice_leaves_with_the_restart() {
    let mut shell = Harness::home();
    // The chip ends 7 px before the window buttons.
    let (x, _, width, height) = shell.bounds("update-chip");
    let (buttons_x, _, _, _) = shell.bounds("window-minimise");
    assert_eq!(x + width + 7.0, buttons_x);
    assert_eq!(height, 36.0);

    shell.click("update-restart");
    assert_eq!(shell.model.snapshot().update, None);
    assert!(!shell.exists("update-chip"));

    // The party window has no update waiting, so no chip.
    let mut party = Harness::new();
    assert!(!party.exists("update-chip"));
}

#[test]
fn joining_with_a_code_takes_the_code_from_the_field() {
    let mut shell = Harness::home();
    shell.model.join_code_signal().set("K7QMD-4XR2P".to_owned());
    shell.pass();
    assert_eq!(shell.model.snapshot().home.join_code, "K7QMD-4XR2P");

    shell.click("home-join");
    assert_eq!(shell.model.snapshot().home.join_code, "");
}

#[test]
fn home_gives_up_width_from_the_list_in_a_small_window() {
    use parties_ui::theme::metrics::{WINDOW_MIN_HEIGHT, WINDOW_MIN_WIDTH};

    let mut shell = Harness::home();
    shell.tree.resize(WINDOW_MIN_WIDTH, WINDOW_MIN_HEIGHT);
    shell.pass();

    let width = WINDOW_MIN_WIDTH as f32;
    // 12 px from each window edge; the side column keeps its 380 px.
    let (x, _, list_width, _) = shell.bounds("home-parties");
    assert_eq!((x, list_width), (12.0, width - 24.0 - 380.0 - 12.0));
    let (x, _, side_width, _) = shell.bounds("home-identity");
    assert_eq!((x, side_width), (width - 12.0 - 380.0, 380.0));
}

// --- Settings ---

#[test]
fn settings_has_its_navigation_and_the_voice_block() {
    let mut shell = Harness::settings(SettingsPage::Appearance);

    // The navigation is the left panel of this view, voice block included.
    assert_eq!(shell.bounds("settings-nav"), (12.0, 52.0, 264.0, 836.0));
    let (x, y, width, height) = shell.bounds("voice-block");
    assert_eq!((x, width), (20.0, 248.0));
    assert_eq!(y + height, 880.0);
    assert!(!shell.exists("left-panel"));

    // Every page has a row; the one in view is marked.
    for page in SettingsPage::ALL {
        let id = format!("settings-{}", page.slug());
        assert!(shell.exists(&id), "{id} is missing");
        assert_eq!(
            shell.has_class(&id, "current"),
            page == SettingsPage::Appearance
        );
    }
    assert_eq!(shell.text("settings-title"), "Appearance");

    // The cards start under the page header and span to 12 px from the edge.
    assert_eq!(
        shell.bounds("settings-lights"),
        (288.0, 160.0, 1140.0, 432.0)
    );
    let (x, y, width, _) = shell.bounds("settings-glass");
    assert_eq!((x, y, width), (288.0, 604.0, 1140.0));
}

#[test]
fn a_navigation_row_opens_its_page() {
    let mut shell = Harness::settings(SettingsPage::Appearance);
    assert!(shell.exists("settings-lights"));

    shell.click("settings-keys");
    assert_eq!(shell.model.snapshot().settings_page, SettingsPage::Keys);
    assert_eq!(shell.text("settings-title"), "Keys");
    assert!(shell.has_class("settings-keys", "current"));
    assert!(!shell.has_class("settings-appearance", "current"));
    // The cards are those of the page in view.
    assert!(!shell.exists("settings-lights"));
    assert!(shell.exists("settings-identity"));

    shell.click("settings-appearance");
    assert!(shell.exists("settings-lights"));
}

#[test]
fn the_switcher_opens_settings_on_the_page_it_was_left_on() {
    let mut shell = Harness::new();
    shell
        .model
        .dispatch(Command::ShowSettingsPage(SettingsPage::Appearance));

    shell.click("switch-settings");
    assert_eq!(shell.model.snapshot().switcher.view, View::Settings);
    assert_eq!(shell.text("settings-title"), "Appearance");

    // The party stays lit in the switcher and a click on it goes back.
    shell.click("switch-party-1");
    assert!(shell.exists("left-panel"));
    assert!(!shell.exists("settings-nav"));
}

#[test]
fn picking_a_light_on_the_appearance_page_changes_the_window() {
    let mut shell = Harness::settings(SettingsPage::Appearance);
    assert!(shell.has_class("light-night", "current"));

    shell.click("light-moss");

    assert_eq!(shell.model.snapshot().light, LightId::Moss);
    assert!(shell.has_class("light-moss", "current"));
    assert!(!shell.has_class("light-night", "current"));
    let accent = shell
        .tree
        .get_element_by_id("brand-mark")
        .expect("the title bar has the brand mark")
        .color();
    assert_eq!(accent, Some(Tokens::for_light(LightId::Moss).accent));
}

#[test]
fn reduce_transparency_turns_the_blur_off_and_on_again() {
    let mut shell = Harness::settings(SettingsPage::Appearance);
    assert!(shell.model.snapshot().backdrop_blur);
    assert!(!shell.has_class("settings-reduce-transparency", "on"));

    shell.click("settings-reduce-transparency");
    assert!(!shell.model.snapshot().backdrop_blur);
    assert!(shell.has_class("settings-reduce-transparency", "on"));

    shell.click("settings-reduce-transparency");
    assert!(shell.model.snapshot().backdrop_blur);
    assert!(!shell.has_class("settings-reduce-transparency", "on"));
}

#[test]
fn settings_keeps_its_navigation_in_a_small_window() {
    use parties_ui::theme::metrics::{WINDOW_MIN_HEIGHT, WINDOW_MIN_WIDTH};

    let mut shell = Harness::settings(SettingsPage::Appearance);
    shell.tree.resize(WINDOW_MIN_WIDTH, WINDOW_MIN_HEIGHT);
    shell.pass();

    let (width, height) = (WINDOW_MIN_WIDTH as f32, WINDOW_MIN_HEIGHT as f32);
    assert_eq!(
        shell.bounds("settings-nav"),
        (12.0, 52.0, 264.0, height - 64.0)
    );
    // The page takes the rest of the width; it scrolls, so a card keeps its
    // height.
    assert_eq!(
        shell.bounds("settings-lights"),
        (288.0, 160.0, width - 300.0, 432.0)
    );
}

// --- Settings: Voice and audio ---

#[test]
fn the_voice_page_sits_where_the_design_draws_it() {
    let mut shell = Harness::settings(SettingsPage::VoiceAndAudio);
    assert_eq!(shell.text("settings-title"), "Voice and audio");

    // "Input" is as tall as its content; "Output" follows 12 px under it.
    assert_eq!(
        shell.bounds("settings-input"),
        (288.0, 160.0, 1140.0, 442.0)
    );
    assert_eq!(
        shell.bounds("settings-output"),
        (288.0, 614.0, 1140.0, 190.0)
    );
    // Two columns of 518 px with 48 px between them.
    assert_eq!(
        shell.bounds("voice-input-device"),
        (316.0, 280.0, 518.0, 42.0)
    );
    assert_eq!(shell.bounds("voice-meter"), (316.0, 392.0, 518.0, 44.0));
    assert_eq!(
        shell.bounds("voice-sensitivity"),
        (316.0, 532.0, 518.0, 16.0)
    );
    let (x, y, _, height) = shell.bounds("voice-mode-activity");
    assert_eq!((x, y, height), (882.0, 280.0, 70.0));
    let (x, _, width, _) = shell.bounds("voice-mode-push-to-talk");
    assert_eq!(x + width, 1400.0);

    assert_eq!(shell.text("voice-sensitivity-value"), "-36 dB");
    assert_eq!(shell.text("voice-output-volume-value"), "80%");
    assert!(shell.exists("voice-detected"));
}

#[test]
fn the_input_mode_cards_choose_how_the_microphone_opens() {
    let mut shell = Harness::settings(SettingsPage::VoiceAndAudio);
    assert!(shell.has_class("voice-mode-activity", "current"));

    shell.click("voice-mode-push-to-talk");

    assert_eq!(
        shell.model.snapshot().voice.input_mode,
        InputMode::PushToTalk
    );
    assert!(shell.has_class("voice-mode-push-to-talk", "current"));
    assert!(!shell.has_class("voice-mode-activity", "current"));
}

#[test]
fn a_switch_row_turns_its_setting_on_and_off() {
    let mut shell = Harness::settings(SettingsPage::VoiceAndAudio);
    assert!(shell.has_class("voice-noise-suppression", "on"));
    assert!(!shell.has_class("voice-automatic-gain", "on"));

    shell.click("voice-automatic-gain");
    shell.click("voice-noise-suppression");

    let voice = shell.model.snapshot().voice;
    assert!(voice.automatic_gain);
    assert!(!voice.noise_suppression);
    assert!(voice.echo_cancellation, "the row between them is untouched");
    assert!(shell.has_class("voice-automatic-gain", "on"));
    assert!(!shell.has_class("voice-noise-suppression", "on"));
}

#[test]
fn the_sensitivity_gates_the_meter_and_the_voice_mark() {
    let mut shell = Harness::settings(SettingsPage::VoiceAndAudio);

    // Above the level of the fixture's microphone (-24 dB): nothing is sent.
    shell.model.sensitivity_signal().set(-12);
    shell.pass();

    assert_eq!(shell.model.snapshot().voice.sensitivity_db, -12);
    assert_eq!(shell.text("voice-sensitivity-value"), "-12 dB");
    assert!(!shell.exists("voice-detected"));

    shell.model.sensitivity_signal().set(-48);
    shell.pass();
    assert!(shell.exists("voice-detected"));
}

#[test]
fn a_press_on_a_slider_moves_its_value() {
    let mut shell = Harness::settings(SettingsPage::VoiceAndAudio);

    // The middle of the sensitivity slider is half way from -60 dB to 0.
    shell.click("voice-sensitivity");
    let sensitivity = shell.model.snapshot().voice.sensitivity_db;
    assert!(
        (-32..=-28).contains(&sensitivity),
        "a press in the middle gave {sensitivity} dB"
    );
    assert_eq!(
        shell.text("voice-sensitivity-value"),
        format!("{sensitivity} dB")
    );

    shell.click("voice-output-volume");
    let volume = shell.model.snapshot().voice.output_volume;
    assert!(
        (48..=52).contains(&volume),
        "a press in the middle gave {volume}%"
    );
    assert_eq!(
        shell.text("voice-output-volume-value"),
        format!("{volume}%")
    );
}

// --- Settings: App and notifications ---

#[test]
fn the_app_page_stands_in_two_columns_of_cards() {
    let mut shell = Harness::settings(SettingsPage::AppAndNotifications);
    assert_eq!(shell.text("settings-title"), "App and notifications");

    // Two columns of 564 px with 12 px between them; a card is as tall as
    // its settings.
    assert_eq!(
        shell.bounds("settings-notifications"),
        (288.0, 160.0, 564.0, 343.0)
    );
    let (x, y, width, _) = shell.bounds("settings-updates");
    assert_eq!((x, y, width), (288.0, 515.0, 564.0));
    assert_eq!(
        shell.bounds("settings-start-up"),
        (864.0, 160.0, 564.0, 267.0)
    );
    let (x, y, width, height) = shell.bounds("settings-language");
    assert_eq!((x, y, width, height), (864.0, 439.0, 564.0, 139.0));
    let (x, y, _, _) = shell.bounds("settings-diagnostics");
    assert_eq!((x, y), (864.0, 590.0));

    // A list is 200 x 44 at the end of its setting.
    assert_eq!(shell.bounds("app-sound"), (624.0, 431.0, 200.0, 44.0));
    assert_eq!(shell.text("app-build"), "Parties 0.8.0 \u{b7} protocol 2.0");
    assert_eq!(
        shell.text("app-last-checked"),
        "Last checked today at 14:02"
    );
}

#[test]
fn every_app_switch_follows_a_click_on_its_row() {
    let mut shell = Harness::settings(SettingsPage::AppAndNotifications);
    for switch in [
        AppSwitch::NotifyMentions,
        AppSwitch::NotifyRoomJoins,
        AppSwitch::NotifyStreams,
        AppSwitch::InstallUpdates,
        AppSwitch::StartWithSystem,
        AppSwitch::StartMinimised,
        AppSwitch::CloseToTray,
        AppSwitch::DiagnosticsOverlay,
    ] {
        let id = format!("app-{}", switch.slug());
        let was = shell.model.snapshot().app.is_on(switch);
        assert_eq!(shell.has_class(&id, "on"), was, "{id} before the click");

        shell.click(&id);

        assert_eq!(shell.model.snapshot().app.is_on(switch), !was, "{id}");
        assert_eq!(shell.has_class(&id, "on"), !was, "{id} after the click");
    }
}

#[test]
fn the_update_channel_and_the_check_button_reach_the_state() {
    let mut shell = Harness::settings(SettingsPage::AppAndNotifications);
    assert!(shell.has_class("app-channel-stable", "current"));

    shell.click("app-channel-beta");
    assert_eq!(
        shell.model.snapshot().app.update_channel,
        UpdateChannel::Beta
    );
    assert!(shell.has_class("app-channel-beta", "current"));
    assert!(!shell.has_class("app-channel-stable", "current"));

    shell.click("app-check-now");
    assert_eq!(shell.text("app-last-checked"), "Last checked just now");
}

// --- Settings: Video and sharing ---

#[test]
fn the_video_page_sits_where_the_design_draws_it() {
    let mut shell = Harness::settings(SettingsPage::VideoAndSharing);
    assert_eq!(shell.text("settings-title"), "Video and sharing");

    // The preview is as tall as the three lists and the line next to it.
    assert_eq!(
        shell.bounds("settings-camera"),
        (288.0, 160.0, 1140.0, 378.0)
    );
    assert_eq!(shell.bounds("video-preview"), (316.0, 227.0, 518.0, 283.0));
    assert_eq!(shell.bounds("video-camera"), (882.0, 252.0, 518.0, 44.0));
    let (x, y, width, _) = shell.bounds("settings-sharing");
    assert_eq!((x, y, width), (288.0, 550.0, 1140.0));
    // The four codecs share the left column; the frame rate list is 160 px.
    let (x, _, _, height) = shell.bounds("video-codec-automatic");
    assert_eq!((x, height), (316.0, 44.0));
    let (x, _, width, _) = shell.bounds("video-codec-h264");
    assert_eq!(x + width, 834.0);
    let (x, _, width, _) = shell.bounds("video-share-frame-rate");
    assert_eq!((x, width), (1240.0, 160.0));
    assert_eq!(shell.text("video-bitrate-value"), "8 Mbps");
}

#[test]
fn the_preview_button_starts_and_stops_the_camera_preview() {
    let mut shell = Harness::settings(SettingsPage::VideoAndSharing);
    assert_eq!(shell.text("video-preview-state"), "Preview is off");

    shell.click("video-preview-toggle");
    assert!(shell.model.snapshot().video.camera_preview);
    assert_eq!(shell.text("video-preview-state"), "Preview is on");
    assert!(shell.has_class("video-preview", "on"));

    shell.click("video-preview-toggle");
    assert!(!shell.model.snapshot().video.camera_preview);
    assert!(!shell.has_class("video-preview", "on"));
}

#[test]
fn the_sharing_defaults_follow_their_controls() {
    let mut shell = Harness::settings(SettingsPage::VideoAndSharing);
    assert!(shell.has_class("video-codec-automatic", "current"));
    assert!(shell.has_class("video-share-audio", "on"));

    shell.click("video-codec-av1");
    shell.click("video-share-audio");
    shell.model.share_bitrate_signal().set(14);
    shell.pass();

    let video = shell.model.snapshot().video;
    assert_eq!(video.codec, CodecChoice::Av1);
    assert!(!video.share_audio);
    assert_eq!(video.share_bitrate_mbps, 14);
    assert!(shell.has_class("video-codec-av1", "current"));
    assert!(!shell.has_class("video-codec-automatic", "current"));
    assert!(!shell.has_class("video-share-audio", "on"));
    assert_eq!(shell.text("video-bitrate-value"), "14 Mbps");
}

// --- Settings: Hotkeys ---

#[test]
fn the_hotkeys_page_lists_every_action_with_its_state() {
    let mut shell = Harness::settings(SettingsPage::Hotkeys);
    assert_eq!(shell.text("settings-title"), "Hotkeys");

    // Seven shortcuts, 200 px fields at the right edge of the card.
    for action in HotkeyAction::ALL {
        let id = format!("hotkey-{}", action.slug());
        let (x, _, width, height) = shell.bounds(&id);
        assert_eq!((x, width, height), (1200.0, 200.0, 36.0), "{id}");
    }
    assert_eq!(shell.text("hotkey-push-to-talk-keys"), "Mouse 4");
    assert_eq!(shell.text("hotkey-push-to-mute-keys"), "Add shortcut");
    // The camera shortcut is being captured, as the frame draws it.
    assert!(shell.has_class("hotkey-toggle-camera", "capturing"));
    assert_eq!(shell.text("hotkey-toggle-camera-keys"), "Press keys");
    // The mute shortcut is taken by another application: a notice under it.
    let (x, y, width, height) = shell.bounds("hotkey-toggle-mute-conflict");
    let (_, row_y, _, row_height) = shell.bounds("hotkey-toggle-mute");
    assert_eq!((x, width, height), (316.0, 1084.0, 38.0));
    assert_eq!(y, row_y + row_height + 9.0);
    assert!(!shell.exists("hotkey-toggle-deafen-conflict"));
}

#[test]
fn a_click_on_a_shortcut_captures_it_and_a_second_click_stops() {
    let mut shell = Harness::settings(SettingsPage::Hotkeys);

    // Capturing moves to the shortcut that was clicked: one at a time.
    shell.click("hotkey-toggle-deafen");
    assert_eq!(
        shell.model.snapshot().hotkeys.capturing,
        Some(HotkeyAction::ToggleDeafen)
    );
    assert!(shell.has_class("hotkey-toggle-deafen", "capturing"));
    assert!(!shell.has_class("hotkey-toggle-camera", "capturing"));

    shell.click("hotkey-toggle-deafen");
    assert_eq!(shell.model.snapshot().hotkeys.capturing, None);
    assert_eq!(shell.text("hotkey-toggle-deafen-keys"), "Ctrl + Shift + D");

    // "Change" in the notice starts a capture for its shortcut.
    shell.click("hotkey-toggle-mute-change");
    assert_eq!(
        shell.model.snapshot().hotkeys.capturing,
        Some(HotkeyAction::ToggleMute)
    );
}

#[test]
fn a_new_shortcut_and_a_reset_reach_the_page() {
    let mut shell = Harness::settings(SettingsPage::Hotkeys);

    // The core reports the keys the user pressed.
    shell.model.dispatch(Command::SetHotkey(
        HotkeyAction::ToggleCamera,
        Some("Ctrl + Shift + C".to_owned()),
    ));
    shell.pass();
    assert_eq!(shell.text("hotkey-toggle-camera-keys"), "Ctrl + Shift + C");
    assert!(!shell.has_class("hotkey-toggle-camera", "capturing"));

    shell.click("hotkeys-reset");
    assert_eq!(shell.text("hotkey-toggle-camera-keys"), "Add shortcut");
    // The defaults know of no conflict: the notice is gone.
    assert!(!shell.exists("hotkey-toggle-mute-conflict"));
}

// --- Settings: Profile and Keys ---

#[test]
fn the_profile_page_shows_the_picture_and_the_names() {
    let mut shell = Harness::settings(SettingsPage::Profile);
    assert_eq!(shell.text("settings-title"), "Profile");

    assert_eq!(
        shell.bounds("settings-picture"),
        (288.0, 160.0, 1140.0, 128.0)
    );
    let (x, y, width, _) = shell.bounds("settings-name");
    assert_eq!((x, y, width), (288.0, 300.0, 1140.0));
    let (x, _, width, height) = shell.bounds("profile-display-name");
    assert_eq!((x, width, height), (316.0, 518.0, 44.0));
    let (x, _, width, height) = shell.bounds("profile-party-names");
    assert_eq!((x, width, height), (882.0, 518.0, 97.0));

    // A nickname is shown where a party has one, the display name elsewhere.
    assert_eq!(shell.text("profile-party-1-name"), "tuxick@night");
    assert_eq!(shell.text("profile-party-2-name"), "tuxick");
    // The fixture of the tests has no pictures: nothing to remove.
    assert!(shell.exists("profile-no-picture"));
    assert!(!shell.exists("profile-remove-picture"));
}

#[test]
fn committing_the_display_name_renames_the_user_everywhere() {
    let mut shell = Harness::settings(SettingsPage::Profile);

    shell.model.display_name_signal().set("  Tux ".to_owned());
    shell.model.dispatch(Command::CommitDisplayName);
    shell.pass();

    let state = shell.model.snapshot();
    assert_eq!(state.me.name, "Tux");
    assert_eq!(state.profile.display_name, "Tux");
    let listed = state
        .members
        .groups
        .iter()
        .flat_map(|group| &group.members)
        .find(|member| member.user == USER_TUXICK)
        .expect("the fixture lists the user among the members");
    assert_eq!(listed.name, "Tux");
    // The party without a nickname follows; the nickname stays.
    assert_eq!(shell.text("profile-party-2-name"), "Tux");
    assert_eq!(shell.text("profile-party-1-name"), "tuxick@night");

    // An empty field is not a name.
    shell.model.display_name_signal().set("   ".to_owned());
    shell.model.dispatch(Command::CommitDisplayName);
    shell.pass();
    let state = shell.model.snapshot();
    assert_eq!(state.me.name, "Tux");
    assert_eq!(state.profile.display_name, "Tux");
}

#[test]
fn removing_the_picture_leaves_the_initials() {
    let mut state = fixtures::glass_settings(&Pictures::none(), SettingsPage::Profile);
    state.profile.picture = Some("256 \u{d7} 256, JPEG, 41 KB".to_owned());
    let mut shell = Harness::with(state);
    assert_eq!(
        shell.text("profile-picture-file"),
        "256 \u{d7} 256, JPEG, 41 KB"
    );

    shell.click("profile-remove-picture");

    assert_eq!(shell.model.snapshot().profile.picture, None);
    assert!(shell.exists("profile-no-picture"));
    assert!(!shell.exists("profile-remove-picture"));
}

#[test]
fn the_keys_page_lists_the_keys_and_keeps_the_last_one() {
    let mut shell = Harness::settings(SettingsPage::Keys);
    assert_eq!(shell.text("settings-title"), "Keys");
    assert_eq!(shell.text("keys-fingerprint"), "5E:7A:91:C2:4D:3F:08:B1");
    assert_eq!(shell.text("keys-storage"), "Signed in with a hardware key");

    let (x, y, width, _) = shell.bounds("settings-identity");
    assert_eq!((x, y, width), (288.0, 160.0, 1140.0));
    // Three keys of 60 px with a hairline between them.
    let (x, _, width, height) = shell.bounds("keys-list");
    assert_eq!((x, width, height), (316.0, 518.0, 182.0));

    shell.click("key-2-remove");
    assert!(!shell.exists("key-2"));
    shell.click("key-1-remove");
    assert!(!shell.exists("key-1"));
    // The only key left stays, however often its button is pressed.
    shell.click("key-3-remove");
    assert!(shell.exists("key-3"));
    assert_eq!(shell.model.snapshot().keys.keys.len(), 1);
}

#[test]
fn the_hardware_key_rule_is_a_switch() {
    let mut shell = Harness::settings(SettingsPage::Keys);
    assert!(!shell.has_class("keys-always-hardware-key", "on"));

    shell.click("keys-always-hardware-key");
    assert!(shell.model.snapshot().keys.always_ask_hardware_key);
    assert!(shell.has_class("keys-always-hardware-key", "on"));
}

#[test]
fn every_settings_page_has_cards() {
    for page in SettingsPage::ALL {
        let mut shell = Harness::settings(page);
        let cards = shell
            .tree
            .get_element_by_id("settings-page")
            .expect("Settings has its page")
            .children()
            .len();
        assert!(cards >= 2, "{page:?} has a header and at least one card");
    }
}
