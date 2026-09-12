#include <RmlUi/Core.h>
#include <client/ui_layout_audit.h>
#include <client/ios_ui_layout.h>
#include <client/ios_audio_routes.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace {

class NullRenderInterface final : public Rml::RenderInterface {
public:
	Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
	void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
	void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
	Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override { return {}; }
	Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return {}; }
	void ReleaseTexture(Rml::TextureHandle) override {}
	void EnableScissorRegion(bool) override {}
	void SetScissorRegion(Rml::Rectanglei) override {}
};

class TestSystemInterface final : public Rml::SystemInterface {
public:
	bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
		if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT)
			std::fprintf(stderr, "RmlUi: %s\n", message.c_str());
		// The test runner is non-interactive, so report assertions instead of
		// invoking RmlUi's debugger breakpoint path.
		return true;
	}
};

bool Check(bool condition, const char* message) {
	if (!condition)
		std::fprintf(stderr, "UI stylesheet test failed: %s\n", message);
	return condition;
}

bool CheckTypographyContract() {
	namespace fs = std::filesystem;
	const fs::path ui_directory = PARTIES_UI_SOURCE_DIR;
	const fs::path typography_path = ui_directory / "typography.rcss";
	std::set<std::string> font_families;
	bool success = true;

	for (const fs::directory_entry& entry : fs::directory_iterator(ui_directory)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".rcss")
			continue;
		std::ifstream stream(entry.path(), std::ios::binary);
		std::ostringstream buffer;
		buffer << stream.rdbuf();
		const std::string source = buffer.str();
		size_t offset = 0;
		while ((offset = source.find("font-family", offset)) != std::string::npos) {
			if (entry.path().filename() != typography_path.filename()) {
				std::fprintf(stderr, "Typography declaration outside typography.rcss: %s\n",
					entry.path().string().c_str());
				success = false;
			}
			const size_t colon = source.find(':', offset);
			const size_t semicolon = colon == std::string::npos ? std::string::npos : source.find(';', colon);
			if (colon != std::string::npos && semicolon != std::string::npos) {
				std::string family = source.substr(colon + 1, semicolon - colon - 1);
				const size_t first = family.find_first_not_of(" \t\r\n");
				const size_t last = family.find_last_not_of(" \t\r\n");
				if (first != std::string::npos)
					font_families.insert(family.substr(first, last - first + 1));
			}
			offset += 11;
		}
	}

	std::ifstream typography_stream(typography_path, std::ios::binary);
	std::ostringstream typography_buffer;
	typography_buffer << typography_stream.rdbuf();
	const std::string typography = typography_buffer.str();
	for (const char* role : {".ui-display", ".ui-heading-page", ".ui-heading-section", ".ui-heading",
		".ui-item-title", ".ui-body", ".ui-control", ".ui-label", ".ui-caption", ".ui-micro", ".ui-symbol"}) {
		if (typography.find(role) == std::string::npos) {
			std::fprintf(stderr, "Missing semantic typography role: %s\n", role);
			success = false;
		}
	}
	if (font_families.empty() || font_families.size() > 2) {
		std::fprintf(stderr, "Expected one or two UI font families, found %zu\n", font_families.size());
		success = false;
	}
	return success;
}

bool CheckIOSRoutePublication() {
    using namespace parties::client;
    struct Device { Rml::String name; int index; };
    std::ifstream stream(std::filesystem::path(PARTIES_UI_SOURCE_DIR) / "lobby.rml");
    std::ostringstream contents;
    contents << stream.rdbuf();
    const auto source = contents.str();
    bool success = true;
    const IOSAudioPort mic{"mic", "iPhone Microphone"};
    const IOSAudioPort speaker{"speaker", "iPhone Speaker", false, true};
    const IOSAudioPort headset{"headset", "AirPods Pro", true};
    for (bool input : {true, false}) {
        auto* context = Rml::CreateContext("ios-route-publication", {800, 500});
        auto model = context->CreateDataModel("audio");
        auto type = model.RegisterStruct<Device>();
        type.RegisterMember("name", &Device::name);
        type.RegisterMember("index", &Device::index);
        model.RegisterArray<std::vector<Device>>();
        std::vector<Device> devices;
        int selected = 0, actual = 0, unexpected_changes = 0;
        const char* device_name = input ? "capture_devices" : "playback_devices";
        const char* selected_name = input ? "selected_capture" : "selected_playback";
        model.Bind(device_name, &devices);
        model.Bind(selected_name, &selected);
        model.BindEventCallback(input ? "capture_selection_changed" : "playback_selection_changed",
            [&](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&) {
                const int index = event.GetParameter<int>("value", -1);
                if (index >= 0) {
                    selected = index;
                    if (index != actual) ++unexpected_changes;
                }
            });
        const auto id = source.find(input ? "id=\"settings-capture-select\"" : "id=\"settings-playback-select\"");
        const auto start = source.rfind("<select", id);
        const auto end = source.find("</select>", id) + 9;
        auto* document = context->LoadDocumentFromMemory("<rml><head></head><body data-model='audio'>" +
            source.substr(start, end - start) + "</body></rml>");
        if (!document) { Rml::RemoveContext("ios-route-publication"); return false; }
        document->Show();
        for (const auto& routes : {
                BuildIOSAudioRoutes({mic}, mic, speaker),
                BuildIOSAudioRoutes({mic, headset}, mic, speaker),
                BuildIOSAudioRoutes({mic, headset}, headset, headset),
                BuildIOSAudioRoutes({mic}, mic, speaker)}) {
            actual = input ? routes.selected_input : routes.selected_output;
            const auto& choices = input ? routes.inputs : routes.outputs;
            devices.clear();
            for (size_t i = 0; i < choices.size(); ++i)
                devices.push_back({choices[i].name, static_cast<int>(i)});
            selected = actual;
            model.GetModelHandle().DirtyVariable(device_name);
            model.GetModelHandle().DirtyVariable(selected_name);
            for (int frame = 0; frame < 3; ++frame) context->Update();
            success &= Check(selected == actual, "route refresh lost the actual audio selection");
        }
        success &= Check(unexpected_changes == 0, "route refresh would trigger an unsolicited audio switch");
        Rml::RemoveContext("ios-route-publication");
    }
    return success;
}

bool CheckControlLayout() {
    const std::filesystem::path directory = PARTIES_UI_SOURCE_DIR;
    for (const char* font : {"Inter-Regular.ttf", "Inter-Bold.ttf"})
        if (!Rml::LoadFontFace((directory / "fonts" / font).string())) return false;
    std::ifstream stream(directory / "lobby.rml");
    std::ostringstream contents;
    contents << stream.rdbuf();
    const auto source = contents.str();
    // Keep the production cascade, including platform rules and typography.
    const auto head = source.substr(0, source.find("</head>") + 7);
    bool success = true;
    struct Profile { bool ios; Rml::Vector2i viewport; float scale; };
    for (const Profile profile : {Profile{false, {1600, 1000}, 2.f}, Profile{false, {2560, 1440}, 2.f},
                                 Profile{true, {1170, 2532}, 3.f}, Profile{true, {1206, 2622}, 3.f}}) {
        const bool ios = profile.ios;
        auto* context = Rml::CreateContext("control-layout", profile.viewport);
        context->SetDensityIndependentPixelRatio(profile.scale);
        context->ActivateTheme(ios ? "ios" : "macos", true);
        const auto body = head + "<body class='" + (ios ? "platform-ios" : "platform-desktop") + R"RML('>
<div class="home-actions">
    <div class="home-add-button ui-button"><span class="ui-button-label ui-control">+ Add party</span></div>
    <span class="party-card-action ui-button ui-button-compact"><span class="ui-button-label ui-control">Join</span></span>
    <span class="party-card-action secondary ui-button ui-button-compact"><span class="ui-button-label ui-control">Reconnect</span></span>
</div>
<div class="modal-box ui-dialog">
    <div class="btn ui-button"><span class="ui-button-label ui-control">Save</span></div>
    <div class="btn btn-secondary ui-button ui-button-secondary"><span class="ui-button-label ui-control">Cancel</span></div>
    <div class="btn btn-danger ui-button ui-button-danger"><span class="ui-button-label ui-control">Replace identity</span></div>
    <div class="btn btn-small ui-button ui-button-compact"><span class="ui-button-label ui-control">Download</span></div>
</div>
<div class="share-codec-buttons ui-segment-group">
    <div class="share-codec-btn ui-segment ui-control"><span class="ui-control">AV1</span></div>
    <div class="share-codec-btn ui-segment ui-control"><span class="ui-control">H.265</span></div>
</div>
<div class="settings-secondary-action ui-button ui-button-secondary ui-button-compact"><span class="ui-control">Copy</span></div>
<div class="settings-danger-action ui-button ui-button-danger"><span class="ui-control">Replace Identity</span></div>
<div class="chat-search-bar"><input type="text" class="chat-search-input ui-input ui-body" value="Search typography" /></div>
<div class="party-connect-form"><input type="text" class="ui-input ui-body" value="voice.example.com" /></div>
<div class="chat-compose"><div class="compose-row"><div class="compose-input-wrap">
    <input id="compose-test" type="text" class="compose-input ui-input ui-body" placeholder="Write something..." />
    <div class="compose-attach-btn ui-icon-button"><svg src="icon-attach.svg" width="18" height="18" /></div>
</div><div class="send-btn ui-icon-button"><svg src="icon-send.svg" width="18" height="18" /></div></div></div>
</body></rml>)RML";
        auto* document = context->LoadDocumentFromMemory(body, (directory / "control-layout.rml").string());
        if (!document) { success = false; }
        else {
            document->Show();
            Rml::ElementList controls;
            document->QuerySelectorAll(controls, ".ui-button, .ui-segment");
            for (const char* state : {"normal", "hover", "active"}) {
                for (auto* control : controls) {
                    control->SetPseudoClass("hover", std::string(state) != "normal");
                    control->SetPseudoClass("active", std::string(state) == "active");
                }
                context->Update();
                context->Render();
                success &= parties::client::AuditUIControlLayout(document, state);
            }
            auto* composer = document->GetElementById("compose-test");
            for (const char* value : {"A draft message", "A longer draft that scrolls horizontally while the cursor stays in the input"}) {
                composer->SetAttribute("value", Rml::String(value));
                composer->Focus();
                context->Update();
                context->Render();
                success &= parties::client::AuditUIControlLayout(document, "composer-draft");
            }
            document->Close();
            context->Update();
            const auto launcher = head + "<body class='fullscreen " +
                (ios ? "platform-ios" : "platform-desktop") + R"RML('>
<div class="app-content"><div class="home-screen"><div class="home-inner"><div class="home-content">
<div class="home-toolbar">
    <div class="home-greeting"><span class="home-eyebrow">Welcome back</span><span class="home-title">Your Parties</span><span class="home-subtitle">3 parties</span></div>
    <div class="home-actions"><div class="home-add-button ui-button"><span class="ui-control">+ Add party</span></div></div>
</div>
<div class="home-grid">
    <div><div class="party-card" data-event-click="test"><span class="ui-body">First party</span></div></div>
    <div><div class="party-card" data-event-click="test"><span class="ui-body">Second party</span></div></div>
    <div><div class="party-card" data-event-click="test"><span class="ui-body">Third party</span></div></div>
    <div class="party-card-add" id="last-party" data-event-click="test"><span class="ui-body">Create or Join</span></div>
</div></div></div></div></div></body></rml>)RML";
            document = context->LoadDocumentFromMemory(launcher, (directory / "launcher-scroll.rml").string());
            if (!document) success = false;
            else {
                document->Show();
                context->Update();
                context->Render();
                success &= parties::client::AuditUIControlLayout(document, "launcher-scroll");
                auto* scroll = document->QuerySelector(".home-inner");
                auto* last = document->GetElementById("last-party");
                scroll->SetScrollTop(scroll->GetScrollHeight());
                context->Update();
                const auto origin = last->GetAbsoluteOffset(Rml::BoxArea::Border);
                const auto size = last->GetBox().GetSize(Rml::BoxArea::Border);
                success &= Check(origin.y >= 0 && origin.y + size.y <= profile.viewport.y + profile.scale,
                    "The final launcher card must be reachable by scrolling");
            }
        }
        Rml::RemoveContext("control-layout");
    }
    return success;
}

bool CheckIdentityGroup() {
    const std::filesystem::path directory = PARTIES_UI_SOURCE_DIR;
    std::ifstream stream(directory / "lobby.rml");
    std::ostringstream contents;
    contents << stream.rdbuf();
    const auto source = contents.str();
    const auto head = source.substr(0, source.find("</head>") + 7);
    const auto start = source.find("<div class=\"user-island-info\"");
    const auto end = source.find("</div>", start) + 6;
    bool success = true;
    for (bool ios : {false, true}) {
        auto* context = Rml::CreateContext("identity-group", ios ? Rml::Vector2i{1179, 2556} : Rml::Vector2i{1600, 1000});
        context->SetDensityIndependentPixelRatio(ios ? 3 : 2);
        context->ActivateTheme(ios ? "ios" : "macos", true);
        Rml::String name = "tuxick ios", fingerprint = "57:8b:dc:a9:50:75:b7:b0:7f:48:e8:f5:84:08:ca:8f:da:6d:33:d7:66:12:34:56";
        Rml::String copied;
        int copies = 0;
        auto lobby = context->CreateDataModel("lobby");
        lobby.Bind("username", &name);
        auto servers = context->CreateDataModel("serverlist");
        servers.Bind("fingerprint", &fingerprint);
        servers.BindEventCallback("copy_fingerprint", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            copied = fingerprint;
            ++copies;
        });
        auto* document = context->LoadDocumentFromMemory(head + "<body class='" +
            (ios ? "platform-ios" : "platform-desktop") + "'><div class='user-island' style='width: 260dp;'>" +
            "<div class='user-island-top'><gradient_circle class='user-avatar' />" + source.substr(start, end - start) +
            "</div></div></body></rml>", (directory / "identity-group.rml").string());
        if (!document) { Rml::RemoveContext("identity-group"); return false; }
        document->Show();
        for (const char* value : {"tuxick ios", "Alexander with a very long display name"}) {
            name = value;
            lobby.GetModelHandle().DirtyVariable("username");
            for (int frame = 0; frame < 3; ++frame) { context->Update(); context->Render(); }
            success &= parties::client::AuditUIControlLayout(document, "identity-group");
            auto* name_label = document->QuerySelector(".user-name");
            success &= Check(name_label->GetInnerRML() == name, "nickname retains its lobby model binding");
            name_label->DispatchEvent(Rml::EventId::Click, {});
            document->QuerySelector(".user-fingerprint")->DispatchEvent(Rml::EventId::Click, {});
            success &= Check(copied == fingerprint, "identity group copies the full fingerprint from the server model");
        }
        success &= Check(copies == 4, "both identity text rows activate the fingerprint copy action");
        Rml::RemoveContext("identity-group");
    }
    return success;
}

bool CheckIOSSafeAreas() {
    using namespace parties::client;
    bool success = true;
    success &= Check(IOSViewportHeight(874, 62, 0, 3) == 812 * 3, "portrait background extends behind home indicator");
    success &= Check(IOSViewportHeight(874, 62, 336, 3) == 476 * 3, "keyboard does not double-count bottom inset");
    success &= Check(IOSViewportHeight(402, 0, 0, 3) == 402 * 3, "landscape background extends behind home indicator");
    success &= Check(IOSViewportHeight(667, 20, 0, 2) == 647 * 2, "rectangular screen has no invented inset");
    const std::filesystem::path directory = PARTIES_UI_SOURCE_DIR;
    std::ifstream stream(directory / "lobby.rml");
    std::ostringstream contents;
    contents << stream.rdbuf();
    const auto source = contents.str();
    const auto head = source.substr(0, source.find("</head>") + 7);
    for (const auto viewport : {Rml::Vector2i{402, 812}, Rml::Vector2i{874, 402}, Rml::Vector2i{402, 476}}) {
        auto* context = Rml::CreateContext("safe-area", viewport * 3);
        context->SetDensityIndependentPixelRatio(3);
        context->ActivateTheme("ios", true);
        const auto body = head + R"RML(<body class="fullscreen platform-ios mobile-content">
<div class="app-content"><div class="main-content"><div style="flex: 1;"></div>
<div class="chat-compose"><div class="compose-input-wrap"><input id="composer" type="text" class="compose-input ui-input ui-body" value="Safe above system UI" /></div>
<div class="send-btn ui-icon-button" id="send"><svg src="icon-send.svg" width="20" height="20" /></div></div>
</div></div>
<div class="screen-share-area fullscreen" id="viewer"><div class="stream-call-dock" id="dock"></div></div>
</body></rml>)RML";
        auto* document = context->LoadDocumentFromMemory(body, (directory / "safe-area.rml").string());
        success &= Check(document != nullptr, "safe-area document loaded");
        if (document) {
            document->Show();
            const float edge = viewport.x > viewport.y ? 62.f : 0.f;
            const float bottom = viewport.x > viewport.y ? 21.f : 34.f;
            const bool keyboard = viewport.y == 476;
            ApplyIOSSafeArea(document, edge, edge, bottom, keyboard ? 336.f : 0.f);
            context->Update();
            for (const char* id : {"composer", "send", "dock", "viewer"}) {
                auto* target = document->GetElementById(id);
                const auto pos = target->GetAbsoluteOffset(Rml::BoxArea::Border) / 3.f;
                const auto size = target->GetBox().GetSize(Rml::BoxArea::Border) / 3.f;
                success &= Check(size.x > 0 && size.y > 0, "safe-area target is laid out");
                success &= Check(pos.x >= edge - 1 && pos.x + size.x <= viewport.x - edge + 1,
                    "landscape controls stay away from the notch and rounded corners");
                success &= Check(pos.y >= 0 && pos.y + size.y <= viewport.y - ((keyboard || std::string(id) == "viewer") ? 0 : bottom) + 1,
                    "bottom controls fit above the home indicator or keyboard");
            }
        }
        Rml::RemoveContext("safe-area");
    }
    return success;
}

bool CheckIOSPageBackTransitions() {
    using namespace parties::client;
    const std::filesystem::path directory = PARTIES_UI_SOURCE_DIR;
    std::ifstream stream(directory / "lobby.rml");
    std::ostringstream contents;
    contents << stream.rdbuf();
    const auto source = contents.str();
    const auto head = source.substr(0, source.find("</head>") + 7);
    bool success = true;
    for (const int height : {812, 476}) {
        auto* context = Rml::CreateContext("ios-page-transitions", {1206, height * 3});
        context->SetDensityIndependentPixelRatio(3);
        context->ActivateTheme("ios", true);
        auto* doc = context->LoadDocumentFromMemory(head + R"RML(<body class="fullscreen platform-ios">
<div class="app-content" id="app">
    <div class="sidebar" id="sidebar"><div style="flex: 1;"></div><div class="sidebar-bottom"><div class="user-island" id="island" style="height: 110dp;"></div></div></div>
    <div class="main-content" id="main">
        <div class="mobile-back-bar" id="back"><span class="ui-body">Channels</span></div>
        <div class="settings-overlay" id="settings"></div>
        <div class="screen-share-area" id="viewer"><div class="screen-share-header"><div class="stream-mobile-back" id="stream-back"><span class="ui-body">Room</span></div></div></div>
    </div>
</div>
<div class="modal-backdrop" id="dialog"><div class="modal-box ui-dialog"><span class="ui-body">Rename channel</span></div></div>
</body></rml>)RML", (directory / "ios-page-transitions.rml").string());
        if (!doc) { success = false; Rml::RemoveContext("ios-page-transitions"); continue; }
        doc->Show();
        ApplyIOSSafeArea(doc, 0, 0, 34, height == 476 ? 336 : 0);
        auto* app = doc->GetElementById("app");
        auto* dialog = doc->GetElementById("dialog");
        auto* viewer = doc->GetElementById("viewer");
        auto* back = doc->GetElementById("back");
        for (const char* page : {"channels", "room", "chat", "settings", "streams", "fullscreen", "channels", "chat"}) {
            const std::string route = page;
            const bool showing = route != "channels";
            const bool video = route == "streams" || route == "fullscreen";
            const bool fullscreen = route == "fullscreen";
            app->SetClass("mobile-content", showing);
            doc->GetElementById("settings")->SetProperty("display", route == "settings" ? "flex" : "none");
            viewer->SetProperty("display", video ? "flex" : "none");
            viewer->SetClass("fullscreen", fullscreen);
            back->SetProperty("display", video ? "none" : "flex");
            dialog->SetProperty("display", "none");
            context->Update();
            context->Render();
            auto* page_surface = doc->GetElementById(showing ? "main" : "sidebar");
            success &= Check(page_surface->GetAbsoluteOffset(Rml::BoxArea::Border).y +
                page_surface->GetBox().GetSize(Rml::BoxArea::Border).y >= height * 3 - 1,
                "page background extends to the screen edge");
            if (!showing) {
                auto* island = doc->GetElementById("island");
                const float island_bottom = island->GetAbsoluteOffset(Rml::BoxArea::Border).y +
                    island->GetBox().GetSize(Rml::BoxArea::Border).y;
                success &= Check(island_bottom <= (height - (height == 476 ? 0 : 34)) * 3,
                    "only the island moves above the system inset");
            }
            const auto target = FindIOSBackTarget(doc, true, showing, fullscreen, false);
            success &= Check(target.action == (!showing ? IOSBackAction::None : fullscreen ?
                IOSBackAction::ExitFullscreen : IOSBackAction::ActivateControl), ("back action for " + route).c_str());
            if (showing && !fullscreen)
                success &= Check(target.control == (video ? doc->GetElementById("stream-back") : back), "back uses the visible page control");
            success &= Check(FindIOSBackTarget(doc, false, showing, fullscreen, false).action == IOSBackAction::None,
                "disconnected pages cannot navigate a connected view");
            success &= Check(FindIOSBackTarget(doc, true, showing, fullscreen, true).action == IOSBackAction::None,
                "native presentations block navigation underneath");
            dialog->SetProperty("display", "flex");
            context->Update();
            context->Render();
            success &= Check(FindIOSBackTarget(doc, true, showing, fullscreen, false).action == IOSBackAction::None,
                "a dialog blocks the back gesture on every page");

        }
        Rml::RemoveContext("ios-page-transitions");
    }
    return success;
}

} // namespace

int main() {
	bool success = CheckTypographyContract();
	NullRenderInterface renderer;
	TestSystemInterface system;
	Rml::SetSystemInterface(&system);
	Rml::SetRenderInterface(&renderer);
	if (!Check(Rml::Initialise(), "RmlUi did not initialise"))
		return 1;
	success &= Check(Rml::Factory::GetElementInstancer("svg") != nullptr, "SVG plugin element was not registered");
	success &= Check(Rml::Factory::GetElementInstancer("lottie") != nullptr, "Lottie plugin element was not registered");

	Rml::Context* context = Rml::CreateContext("rcss-variable-test", {800, 600});
	if (!Check(context != nullptr, "context was not created")) {
		Rml::Shutdown();
		return 1;
	}

	const Rml::String source = R"RML(
<rml>
<head>
<style>
body {
    --surface: #10141c;
}
#target {
    --space-base: 14dp;
    --space-card: var(--space-base);
    background-color: var(--surface);
    color: var(--not-declared, #eef1f6);
    width: var(--space-card);
    height: var(--missing-height, var(--space-base));
    margin-left: var(--space-base);
}
</style>
</head>
<body><div id="target">Variables</div></body>
</rml>
)RML";

	Rml::ElementDocument* document = context->LoadDocumentFromMemory(source, "rcss-variable-test.rml");
	success &= Check(document != nullptr, "document containing custom properties did not load");
	if (document) {
		document->Show();
		context->Update();
		Rml::Element* target = document->GetElementById("target");
		success &= Check(target != nullptr, "target element is missing");
		if (target) {
			const auto read_colour = [&](const char* name) {
				const Rml::Property* property = target->GetProperty(name);
				success &= Check(property != nullptr, "expected color property is missing");
				return property ? property->Get<Rml::Colourb>() : Rml::Colourb{};
			};
			const auto read_number = [&](const char* name) {
				const Rml::Property* property = target->GetProperty(name);
				success &= Check(property != nullptr, "expected numeric property is missing");
				return property ? property->GetNumericValue().number : 0.0f;
			};

			const Rml::Colourb background = read_colour("background-color");
			const Rml::Colourb color = read_colour("color");
			const float width = read_number("width");
			const float height = read_number("height");
			const float margin_left = read_number("margin-left");
			success &= Check(background == Rml::Colourb(16, 20, 28, 255),
				"color variable was not resolved");
			success &= Check(color == Rml::Colourb(238, 241, 246, 255),
				"fallback color was not resolved");
			success &= Check(std::fabs(width - 14.0f) < 0.01f,
				"nested variable did not observe the later override");
			success &= Check(std::fabs(height - 14.0f) < 0.01f,
				"nested fallback was not resolved");
			success &= Check(std::fabs(margin_left - 14.0f) < 0.01f,
				"local custom property was not resolved");

			target->SetProperty("--space-base", "18dp");
			context->Update();
			const float updated_width = read_number("width");
			const float updated_height = read_number("height");
			success &= Check(std::fabs(updated_width - 18.0f) < 0.01f,
				"nested custom property did not react to a runtime override");
			success &= Check(std::fabs(updated_height - 18.0f) < 0.01f,
				"fallback variable did not react to a runtime override");
		}
	}

	Rml::RemoveContext("rcss-variable-test");
	success &= CheckControlLayout();
	success &= CheckIOSSafeAreas();
	success &= CheckIdentityGroup();
	success &= CheckIOSPageBackTransitions();
	success &= CheckIOSRoutePublication();
	Rml::Shutdown();
	return success ? 0 : 1;
}
