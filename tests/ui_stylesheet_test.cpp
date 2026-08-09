#include <RmlUi/Core.h>

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
	Rml::Shutdown();
	return success ? 0 : 1;
}
