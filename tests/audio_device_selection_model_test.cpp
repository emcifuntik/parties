#include <client/lobby_model.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>

#include <cstdio>

namespace {

class NullRenderInterface final : public Rml::RenderInterface {
public:
	Rml::CompiledGeometryHandle CompileGeometry(
		Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
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
		return true;
	}
};

bool Check(bool condition, const char* message) {
	if (!condition)
		std::fprintf(stderr, "Audio device selection model test failed: %s\n", message);
	return condition;
}

bool CheckShareSettingsAndHotkeys(Rml::Context* context, parties::client::LobbyModel& model) {
	using namespace parties::client;
	const Rml::String source = R"RML(
<rml>
<body data-model="lobby">
	<div id="codec-h265" data-class-active="share_codec == 1" data-event-click="select_share_codec(1)">H.265</div>
	<div id="codec-out-of-table" data-event-click="select_share_codec(7)">Bogus</div>
	<div id="fps-120" data-class-active="share_fps == 3" data-event-click="select_share_fps(3)">120</div>
	<div id="scale-half" data-class-active="share_scale == 2" data-event-click="select_share_scale(2)">50%</div>
	<div id="audio-picker" data-if="share_picker_mode == 1">Application audio</div>
	<span id="ptt-name" data-if="ptt_binding == false && ptt_key_assigned">{{ ptt_key_name }}</span>
	<span id="ptt-unset" data-if="ptt_binding == false && ptt_key_assigned == false">Not set</span>
</body>
</rml>
)RML";
	Rml::ElementDocument* document = context->LoadDocumentFromMemory(source, "share-settings-test.rml");
	if (!Check(document != nullptr, "share settings document did not load"))
		return false;
	document->Show();
	context->Update();
	bool success = true;
	auto element = [document](const char* id) { return document->GetElementById(id); };

	success &= Check(!element("codec-h265")->IsClassSet("active"), "H.265 was active before selection");
	element("codec-h265")->DispatchEvent("click", {});
	context->Update();
	success &= Check(model.share_codec.get() == ShareCodec::H265, "codec index 1 did not select H.265");
	success &= Check(element("codec-h265")->IsClassSet("active"), "RML did not compare the codec enum numerically");

	element("codec-out-of-table")->DispatchEvent("click", {});
	context->Update();
	success &= Check(model.share_codec.get() == ShareCodec::H265, "out-of-table codec index changed the selection");

	element("fps-120")->DispatchEvent("click", {});
	element("scale-half")->DispatchEvent("click", {});
	context->Update();
	success &= Check(model.share_fps.get() == ShareFrameRate::Fps120 && element("fps-120")->IsClassSet("active"),
		"frame rate index 3 did not select 120 fps");
	success &= Check(model.share_scale.get() == ShareScale::Half && element("scale-half")->IsClassSet("active"),
		"scale index 2 did not select half scale");

	success &= Check(!element("audio-picker")->IsVisible(), "audio picker copy shown in screen mode");
	model.share_picker_mode = SharePickerMode::ApplicationAudio;
	context->Update();
	success &= Check(element("audio-picker")->IsVisible(), "audio picker copy hidden in application audio mode");

	success &= Check(element("ptt-unset")->IsVisible() && !element("ptt-name")->IsVisible(),
		"unassigned push to talk did not show Not set");
	model.ptt_key_assigned = true;
	model.ptt_key_name = "Ctrl+Space";
	context->Update();
	success &= Check(!element("ptt-unset")->IsVisible() && element("ptt-name")->IsVisible(),
		"assigned push to talk still showed Not set");

	document->Close();
	return success;
}

}

int main() {
	NullRenderInterface renderer;
	TestSystemInterface system;
	Rml::SetRenderInterface(&renderer);
	Rml::SetSystemInterface(&system);
	if (!Check(Rml::Initialise(), "RmlUi did not initialise"))
		return 1;

	Rml::Context* context = Rml::CreateContext("audio-device-selection-test", {640, 480});
	if (!Check(context != nullptr, "context was not created")) {
		Rml::Shutdown();
		return 1;
	}

	parties::client::LobbyModel model;
	if (!Check(model.init(context), "lobby model did not initialise")) {
		Rml::RemoveContext("audio-device-selection-test");
		Rml::Shutdown();
		return 1;
	}

	model.capture_devices.silent() = {{"Virtual Cable", 0}, {"Microphone", 1}};
	model.playback_devices.silent() = {{"Virtual Cable", 0}, {"Speakers", 1}};
	model.capture_devices.notify();
	model.playback_devices.notify();
	model.selected_capture = 0;
	model.selected_playback = 0;

	int selected_capture = -1;
	int selected_playback = -1;
	model.on_select_capture = [&](int index) { selected_capture = index; };
	model.on_select_playback = [&](int index) { selected_playback = index; };

	const Rml::String source = R"RML(
<rml>
<body data-model="lobby">
	<select id="capture" data-value="selected_capture" data-event-change="capture_selection_changed">
		<option data-for="dev : capture_devices" data-attr-value="dev.index">{{ dev.name }}</option>
	</select>
	<select id="playback" data-value="selected_playback" data-event-change="playback_selection_changed">
		<option data-for="dev : playback_devices" data-attr-value="dev.index">{{ dev.name }}</option>
	</select>
</body>
</rml>
)RML";

	Rml::ElementDocument* document = context->LoadDocumentFromMemory(source, "audio-device-selection-test.rml");
	bool success = Check(document != nullptr, "document did not load");
	if (document) {
		document->Show();
		context->Update();

		auto* capture = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(document->GetElementById("capture"));
		auto* playback = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(document->GetElementById("playback"));
		success &= Check(capture != nullptr, "capture select is missing");
		success &= Check(playback != nullptr, "playback select is missing");

		selected_capture = -1;
		selected_playback = -1;
		if (capture)
			capture->SetValue("1");
		if (playback)
			playback->SetValue("1");

		success &= Check(selected_capture == 1, "capture callback received a stale selection");
		success &= Check(model.selected_capture.get() == 1, "capture model value was not updated");
		success &= Check(selected_playback == 1, "playback callback received a stale selection");
		success &= Check(model.selected_playback.get() == 1, "playback model value was not updated");
		document->Close();
	}

	success &= CheckShareSettingsAndHotkeys(context, model);

	Rml::RemoveContext("audio-device-selection-test");
	Rml::Shutdown();
	return success ? 0 : 1;
}
