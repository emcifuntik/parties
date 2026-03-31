// Embedded Inter fonts for the designer's built-in UI.
// Uses C++23 #embed to include the TTF data directly in the binary.
// These are loaded during init so the manager window always has fonts,
// regardless of which asset folders are configured.

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Types.h>
#include <cstdio>

static const unsigned char font_inter_regular[] = {
	#embed "../../client/ui/fonts/Inter-Regular.ttf"
};

static const unsigned char font_inter_medium[] = {
	#embed "../../client/ui/fonts/Inter-Medium.ttf"
};

static const unsigned char font_inter_bold[] = {
	#embed "../../client/ui/fonts/Inter-Bold.ttf"
};

namespace designer {

void LoadEmbeddedFonts() {
	bool ok = true;

	ok &= Rml::LoadFontFace(
		{reinterpret_cast<const Rml::byte*>(font_inter_regular), sizeof(font_inter_regular)},
		"Inter", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal);

	ok &= Rml::LoadFontFace(
		{reinterpret_cast<const Rml::byte*>(font_inter_medium), sizeof(font_inter_medium)},
		"Inter", Rml::Style::FontStyle::Normal, static_cast<Rml::Style::FontWeight>(500));

	ok &= Rml::LoadFontFace(
		{reinterpret_cast<const Rml::byte*>(font_inter_bold), sizeof(font_inter_bold)},
		"Inter", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Bold, true);

	if (ok)
		std::printf("[Designer] Loaded 3 embedded Inter fonts\n");
	else
		std::printf("[Designer] Warning: some embedded fonts failed to load\n");
}

} // namespace designer
