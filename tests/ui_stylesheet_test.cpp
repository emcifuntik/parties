#include <RmlUi/Core.h>

#include <cmath>
#include <cstdio>

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
		std::fprintf(stderr, "RCSS variable test failed: %s\n", message);
	return condition;
}

} // namespace

int main() {
	NullRenderInterface renderer;
	TestSystemInterface system;
	Rml::SetSystemInterface(&system);
	Rml::SetRenderInterface(&renderer);
	if (!Check(Rml::Initialise(), "RmlUi did not initialise"))
		return 1;
	bool success = true;
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
	Rml::Shutdown();
	return success ? 0 : 1;
}
