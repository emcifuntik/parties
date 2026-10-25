#include <client/chat_model.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>

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
		std::fprintf(stderr, "Chat compose clear test failed: %s\n", message);
	return condition;
}

}

int main() {
	NullRenderInterface renderer;
	TestSystemInterface system;
	Rml::SetRenderInterface(&renderer);
	Rml::SetSystemInterface(&system);
	if (!Check(Rml::Initialise(), "RmlUi did not initialise"))
		return 1;

	Rml::Context* context = Rml::CreateContext("chat-compose-clear-test", {640, 480});
	if (!Check(context != nullptr, "context was not created")) {
		Rml::Shutdown();
		return 1;
	}

	parties::client::ChatModel chat;
	if (!Check(chat.init(context), "chat model did not initialise")) {
		Rml::RemoveContext("chat-compose-clear-test");
		Rml::Shutdown();
		return 1;
	}

	Rml::String sent;
	int sends = 0;
	chat.on_send_message = [&] {
		sent = chat.compose_text.get();
		++sends;
		chat.compose_text = "";
	};

	const Rml::String source = R"RML(
<rml>
<head><style>body { width: 640px; height: 480px; } input { display: block; width: 300px; height: 20px; }</style></head>
<body data-model="chat">
	<input id="compose" type="text" data-value="compose_text" data-event-keydown="compose_keydown"/>
</body>
</rml>
)RML";

	Rml::ElementDocument* document = context->LoadDocumentFromMemory(source, "chat-compose-clear-test.rml");
	bool success = Check(document != nullptr, "document did not load");
	if (document) {
		document->Show();
		context->Update();

		auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(document->GetElementById("compose"));
		success &= Check(input != nullptr, "compose input is missing");
		if (input) {
			input->Focus();
			context->ProcessTextInput("hello");
			context->Update();
			success &= Check(chat.compose_text.get() == "hello", "typed text did not reach the model");

			context->ProcessKeyDown(Rml::Input::KI_RETURN, 0);
			context->Update();
			context->Update();

			success &= Check(sends == 1 && sent == "hello", "Enter did not send the composed text");
			success &= Check(chat.compose_text.get().empty(), "model compose text survived Enter (the input's Enter change event writes its stale value back)");
			success &= Check(input->GetValue().empty(), "input still shows the sent text after Enter");

			context->ProcessTextInput("x");
			context->Update();
			success &= Check(chat.compose_text.get() == "x", "the cleared input does not accept new text");
		}
		document->Close();
	}

	Rml::RemoveContext("chat-compose-clear-test");
	Rml::Shutdown();
	return success ? 0 : 1;
}
