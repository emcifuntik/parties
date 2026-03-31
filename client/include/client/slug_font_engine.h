#pragma once

// Slug GPU font rendering — RmlUi FontEngineInterface implementation
// Uses stb_truetype for font loading/metrics and SlugGlyphCache for GPU glyph data.

#include "slug_glyph_cache.h"

#include <RmlUi/Core/CallbackTexture.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/FontMetrics.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Per-vertex data for Slug GPU rendering (80 bytes, 5 x float4)
struct SlugVertex {
	float pos[4]; // xy = object-space position, zw = outward normal
	float tex[4]; // xy = em-space sample coords, z = packed glyph loc, w = packed band info
	float jac[4]; // inverse Jacobian (d_em/d_obj): 00, 01, 10, 11
	float bnd[4]; // xy = band scale, zw = band offset
	float col[4]; // rgba vertex color (premultiplied)
};
static_assert(sizeof(SlugVertex) == 80, "SlugVertex must be 80 bytes");

// Pending Slug batch data (registered by font engine, consumed by renderer)
struct SlugBatchData {
	std::vector<SlugVertex> vertices;
	std::vector<int> indices;
	uint32_t batch_id;
};

// Magic value embedded in Rml::Vertex::tex_coord.x to identify Slug geometry
static constexpr float SLUG_MAGIC_U = 7.7777e+37f;

class SlugFontEngine : public Rml::FontEngineInterface {
public:
	SlugFontEngine();
	~SlugFontEngine() override;

	// --- Font loading ---
	bool LoadFontFace(const Rml::String& file_name, int face_index,
	                  bool fallback_face, Rml::Style::FontWeight weight) override;
	bool LoadFontFace(Rml::Span<const Rml::byte> data, int face_index,
	                  const Rml::String& family, Rml::Style::FontStyle style,
	                  Rml::Style::FontWeight weight, bool fallback_face) override;

	// --- Face resolution ---
	Rml::FontFaceHandle GetFontFaceHandle(const Rml::String& family,
	    Rml::Style::FontStyle style, Rml::Style::FontWeight weight, int size) override;

	// --- Font effects ---
	Rml::FontEffectsHandle PrepareFontEffects(Rml::FontFaceHandle handle,
	    const Rml::FontEffectList& font_effects) override;

	// --- Metrics ---
	const Rml::FontMetrics& GetFontMetrics(Rml::FontFaceHandle handle) override;
	int GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string,
	    const Rml::TextShapingContext& ctx,
	    Rml::Character prior_character = Rml::Character::Null) override;

	// --- String rendering ---
	int GenerateString(Rml::RenderManager& render_manager,
	    Rml::FontFaceHandle face_handle, Rml::FontEffectsHandle effects_handle,
	    Rml::StringView string, Rml::Vector2f position,
	    Rml::ColourbPremultiplied colour, float opacity,
	    const Rml::TextShapingContext& ctx, Rml::TexturedMeshList& mesh_list) override;

	int GetVersion(Rml::FontFaceHandle handle) override;
	void Shutdown() override;
	void ReleaseFontResources() override;

	// --- Slug-specific access (for renderer) ---
	SlugGlyphCache& GetGlyphCache() { return glyph_cache_; }

	// Consume a pending batch by ID (called by renderer during CompileGeometry)
	SlugBatchData* ConsumePendingBatch(uint32_t batch_id);


private:
	struct FontFace;     // Defined in .cpp (uses stbtt_fontinfo)
	struct FontFaceSize; // Defined in .cpp

	int FindBestFace(const Rml::String& family, Rml::Style::FontStyle style,
	                 Rml::Style::FontWeight weight) const;

	static int DecodeUtf8(const char*& p, const char* end);

	std::vector<std::unique_ptr<FontFace>> faces_;
	std::vector<std::unique_ptr<FontFaceSize>> face_sizes_;
	int fallback_face_index_ = -1;

	SlugGlyphCache glyph_cache_;
	Rml::CallbackTextureSource marker_texture_source_;

	// Pending batch data (font engine produces, renderer consumes)
	std::unordered_map<uint32_t, std::unique_ptr<SlugBatchData>> pending_batches_;
	uint32_t next_batch_id_ = 1;

	int version_ = 0;
};
