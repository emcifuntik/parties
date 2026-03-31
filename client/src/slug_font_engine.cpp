// Slug GPU font rendering — RmlUi FontEngineInterface implementation
// Uses stb_truetype for font loading/metrics, SlugGlyphCache for curve data.

#include <client/slug_font_engine.h>

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/CallbackTexture.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/TextShapingContext.h>

#include "font/stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// --- Private struct definitions (use stbtt_fontinfo) ---

struct SlugFontEngine::FontFace {
	std::vector<unsigned char> font_data; // Raw TTF data (stb_truetype needs it alive)
	stbtt_fontinfo font_info;
	std::string family;
	Rml::Style::FontStyle style;
	Rml::Style::FontWeight weight;
	int ascent, descent, line_gap;
	float em_scale; // 1.0 / (ascent - descent)
	bool valid;
};

struct SlugFontEngine::FontFaceSize {
	int face_index;      // Index into faces_
	int size;            // Pixel size
	float pixel_scale;   // stbtt_ScaleForMappingEmToPixels result
	Rml::FontMetrics metrics;
};

// Helper: reinterpret uint32 bits as float
static inline float AsFloat(uint32_t v) {
	float f;
	std::memcpy(&f, &v, 4);
	return f;
}

SlugFontEngine::SlugFontEngine()
	: marker_texture_source_([](const Rml::CallbackTextureInterface& iface) {
		Rml::byte data[] = {255, 0, 255, 255};
		return iface.GenerateTexture({data, 4}, {1, 1});
	}) {}

SlugFontEngine::~SlugFontEngine() {
	ReleaseFontResources();
}

// --- UTF-8 decoding ---

int SlugFontEngine::DecodeUtf8(const char*& p, const char* end) {
	if (p >= end) return 0;
	unsigned char c = static_cast<unsigned char>(*p);
	int codepoint;
	int extra_bytes;

	if (c < 0x80) {
		codepoint = c;
		extra_bytes = 0;
	} else if ((c & 0xE0) == 0xC0) {
		codepoint = c & 0x1F;
		extra_bytes = 1;
	} else if ((c & 0xF0) == 0xE0) {
		codepoint = c & 0x0F;
		extra_bytes = 2;
	} else if ((c & 0xF8) == 0xF0) {
		codepoint = c & 0x07;
		extra_bytes = 3;
	} else {
		p++;
		return 0xFFFD; // replacement character
	}
	p++;
	for (int i = 0; i < extra_bytes && p < end; i++) {
		unsigned char next = static_cast<unsigned char>(*p);
		if ((next & 0xC0) != 0x80) return 0xFFFD;
		codepoint = (codepoint << 6) | (next & 0x3F);
		p++;
	}
	return codepoint;
}

// --- Font loading ---

bool SlugFontEngine::LoadFontFace(const Rml::String& file_name, int face_index,
                                   bool fallback_face, Rml::Style::FontWeight weight) {

	// Load file via RmlUi's file interface
	Rml::FileInterface* file_interface = Rml::GetFileInterface();
	if (!file_interface) return false;

	Rml::FileHandle fh = file_interface->Open(file_name);
	if (!fh) {
		Rml::Log::Message(Rml::Log::LT_ERROR, "SlugFontEngine: Could not open font file '%s'", file_name.c_str());
		return false;
	}

	size_t length = file_interface->Length(fh);
	std::vector<unsigned char> data(length);
	file_interface->Read(data.data(), length, fh);
	file_interface->Close(fh);

	// Use the filename as the family name (strip path and extension)
	std::string family;
	size_t last_slash = file_name.find_last_of("/\\");
	size_t start = (last_slash != std::string::npos) ? last_slash + 1 : 0;
	size_t dot = file_name.find_last_of('.');
	if (dot != std::string::npos && dot > start)
		family = file_name.substr(start, dot - start);
	else
		family = file_name.substr(start);

	// Strip weight/style suffixes to get the base family name
	// e.g., "Inter-Bold" -> "Inter", "Inter-Medium" -> "Inter"
	size_t dash = family.find_last_of('-');
	std::string base_family = family;
	Rml::Style::FontStyle style = Rml::Style::FontStyle::Normal;

	if (dash != std::string::npos) {
		std::string suffix = family.substr(dash + 1);
		base_family = family.substr(0, dash);

		// Detect style/weight from suffix
		if (suffix == "Italic" || suffix == "It") {
			style = Rml::Style::FontStyle::Italic;
		} else if (suffix == "BoldItalic" || suffix == "BoldIt") {
			style = Rml::Style::FontStyle::Italic;
			if (weight == Rml::Style::FontWeight::Auto)
				weight = Rml::Style::FontWeight::Bold;
		} else if (suffix == "Bold") {
			if (weight == Rml::Style::FontWeight::Auto)
				weight = Rml::Style::FontWeight::Bold;
		} else if (suffix == "Medium") {
			if (weight == Rml::Style::FontWeight::Auto)
				weight = static_cast<Rml::Style::FontWeight>(500);
		} else if (suffix == "Light" || suffix == "Thin") {
			if (weight == Rml::Style::FontWeight::Auto)
				weight = static_cast<Rml::Style::FontWeight>(300);
		} else if (suffix == "Regular") {
			// Normal weight
		} else {
			// Unknown suffix, keep full name as family
			base_family = family;
		}
	}

	if (weight == Rml::Style::FontWeight::Auto)
		weight = Rml::Style::FontWeight::Normal;

	return LoadFontFace(
	    Rml::Span<const Rml::byte>(data.data(), data.size()),
	    face_index, base_family, style, weight, fallback_face);
}

bool SlugFontEngine::LoadFontFace(Rml::Span<const Rml::byte> data, int face_index,
                                   const Rml::String& family, Rml::Style::FontStyle style,
                                   Rml::Style::FontWeight weight, bool fallback_face) {

	auto face = std::make_unique<FontFace>();
	face->font_data.assign(data.begin(), data.end());
	face->family = family;
	face->style = style;
	face->weight = weight;
	face->valid = false;

	int offset = stbtt_GetFontOffsetForIndex(face->font_data.data(), face_index);
	if (offset < 0) {
		Rml::Log::Message(Rml::Log::LT_ERROR, "SlugFontEngine: Invalid font face index %d for '%s'",
		                  face_index, family.c_str());
		return false;
	}

	if (!stbtt_InitFont(&face->font_info, face->font_data.data(), offset)) {
		Rml::Log::Message(Rml::Log::LT_ERROR, "SlugFontEngine: Failed to init font '%s'", family.c_str());
		return false;
	}

	stbtt_GetFontVMetrics(&face->font_info, &face->ascent, &face->descent, &face->line_gap);
	face->em_scale = 1.0f / static_cast<float>(face->ascent - face->descent);
	face->valid = true;

	int idx = static_cast<int>(faces_.size());
	if (fallback_face)
		fallback_face_index_ = idx;

	Rml::Log::Message(Rml::Log::LT_INFO, "SlugFontEngine: Loaded '%s' (style=%d, weight=%d)%s",
	                  family.c_str(), static_cast<int>(style), static_cast<int>(weight),
	                  fallback_face ? " [fallback]" : "");

	faces_.push_back(std::move(face));
	version_++;
	return true;
}

// --- Face resolution ---

int SlugFontEngine::FindBestFace(const Rml::String& family, Rml::Style::FontStyle style,
                                  Rml::Style::FontWeight weight) const {
	int best = -1;
	int best_score = -1;

	for (int i = 0; i < static_cast<int>(faces_.size()); i++) {
		const auto& f = faces_[i];
		if (!f->valid) continue;

		// Case-insensitive family match
		if (!Rml::StringUtilities::StringCompareCaseInsensitive(f->family, family))
			continue;

		int score = 0;
		// Style match
		if (f->style == style) score += 100;
		// Weight match (closer is better)
		int weight_diff = std::abs(static_cast<int>(f->weight) - static_cast<int>(weight));
		score += 50 - (std::min)(weight_diff / 10, 50);

		if (score > best_score) {
			best_score = score;
			best = i;
		}
	}

	return best;
}

Rml::FontFaceHandle SlugFontEngine::GetFontFaceHandle(
    const Rml::String& family, Rml::Style::FontStyle style,
    Rml::Style::FontWeight weight, int size) {

	int face_idx = FindBestFace(family, style, weight);
	if (face_idx < 0) {
		face_idx = fallback_face_index_;
		if (face_idx < 0 && !faces_.empty())
			face_idx = 0; // Last resort: first loaded face
	}
	if (face_idx < 0) return Rml::FontFaceHandle(0);

	const auto& face = faces_[face_idx];

	// Check if we already have this face+size combination
	for (int i = 0; i < static_cast<int>(face_sizes_.size()); i++) {
		auto& fs = face_sizes_[i];
		if (fs->face_index == face_idx && fs->size == size)
			return static_cast<Rml::FontFaceHandle>(i + 1); // 1-based handle
	}

	// Create new face size
	auto fs = std::make_unique<FontFaceSize>();
	fs->face_index = face_idx;
	fs->size = size;
	fs->pixel_scale = stbtt_ScaleForMappingEmToPixels(&face->font_info, static_cast<float>(size));

	// Compute metrics
	float ps = fs->pixel_scale;
	fs->metrics.size = size;
	fs->metrics.ascent = face->ascent * ps;
	fs->metrics.descent = -face->descent * ps; // RmlUi expects positive value below baseline
	fs->metrics.line_spacing = (face->ascent - face->descent + face->line_gap) * ps;

	// x-height: try to get from the 'x' glyph
	int x_glyph = stbtt_FindGlyphIndex(&face->font_info, 'x');
	if (x_glyph) {
		int x0, y0, x1, y1;
		stbtt_GetGlyphBox(&face->font_info, x_glyph, &x0, &y0, &x1, &y1);
		fs->metrics.x_height = y1 * ps;
	} else {
		fs->metrics.x_height = fs->metrics.ascent * 0.5f;
	}

	// Underline: try OS/2 table, fallback to heuristic
	fs->metrics.underline_position = fs->metrics.descent * 0.5f;
	fs->metrics.underline_thickness = (std::max)(1.0f, static_cast<float>(size) / 14.0f);

	// Check for ellipsis character
	fs->metrics.has_ellipsis = (stbtt_FindGlyphIndex(&face->font_info, 0x2026) != 0);

	int handle = static_cast<int>(face_sizes_.size()) + 1;
	face_sizes_.push_back(std::move(fs));
	return static_cast<Rml::FontFaceHandle>(handle);
}

// --- Font effects ---

Rml::FontEffectsHandle SlugFontEngine::PrepareFontEffects(
    Rml::FontFaceHandle /*handle*/, const Rml::FontEffectList& /*font_effects*/) {
	// Slug doesn't need bitmap-based font effects
	return Rml::FontEffectsHandle(0);
}

// --- Metrics ---

const Rml::FontMetrics& SlugFontEngine::GetFontMetrics(Rml::FontFaceHandle handle) {
	int idx = static_cast<int>(handle) - 1;
	if (idx < 0 || idx >= static_cast<int>(face_sizes_.size())) {
		static Rml::FontMetrics dummy{};
		return dummy;
	}
	return face_sizes_[idx]->metrics;
}

int SlugFontEngine::GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string,
                                    const Rml::TextShapingContext& ctx,
                                    Rml::Character prior_character) {
	int idx = static_cast<int>(handle) - 1;
	if (idx < 0 || idx >= static_cast<int>(face_sizes_.size()))
		return 0;

	const auto& fs = face_sizes_[idx];
	const auto& face = faces_[fs->face_index];
	float ps = fs->pixel_scale;

	float width = 0;
	int prev_glyph = 0;
	if (prior_character != Rml::Character::Null)
		prev_glyph = stbtt_FindGlyphIndex(&face->font_info, static_cast<int>(prior_character));

	const char* p = string.begin();
	const char* end = string.end();
	while (p < end) {
		int codepoint = DecodeUtf8(p, end);
		if (codepoint == 0) continue;

		int glyph = stbtt_FindGlyphIndex(&face->font_info, codepoint);

		// Kerning
		if (prev_glyph && glyph) {
			width += stbtt_GetGlyphKernAdvance(&face->font_info, prev_glyph, glyph) * ps;
		}

		int adv, lsb;
		stbtt_GetGlyphHMetrics(&face->font_info, glyph, &adv, &lsb);
		width += adv * ps;

		// Letter spacing
		width += ctx.letter_spacing;

		prev_glyph = glyph;
	}

	return static_cast<int>(std::ceil(width));
}

// --- String rendering ---

int SlugFontEngine::GenerateString(
    Rml::RenderManager& render_manager,
    Rml::FontFaceHandle face_handle, Rml::FontEffectsHandle /*effects_handle*/,
    Rml::StringView string, Rml::Vector2f position,
    Rml::ColourbPremultiplied colour, float opacity,
    const Rml::TextShapingContext& ctx, Rml::TexturedMeshList& mesh_list) {

	int idx = static_cast<int>(face_handle) - 1;
	if (idx < 0 || idx >= static_cast<int>(face_sizes_.size()))
		return 0;

	const auto& fs = face_sizes_[idx];
	const auto& face = faces_[fs->face_index];
	float ps = fs->pixel_scale;
	float jac_s = face->em_scale / ps; // d(em) / d(pixel)

	// Premultiplied color as float4
	float col_r = colour.red / 255.0f;
	float col_g = colour.green / 255.0f;
	float col_b = colour.blue / 255.0f;
	float col_a = colour.alpha / 255.0f * opacity;

	// Apply premultiplication
	col_r *= opacity;
	col_g *= opacity;
	col_b *= opacity;

	// Allocate a batch ID for the side-channel
	uint32_t batch_id = next_batch_id_++;
	auto batch = std::make_unique<SlugBatchData>();
	batch->batch_id = batch_id;

	// Standard RmlUi mesh (placeholder geometry for layout)
	Rml::Mesh rml_mesh;

	float pen_x = position.x;
	float base_y = position.y;

	int prev_glyph = 0;
	int glyph_count = 0;

	const char* p = string.begin();
	const char* end_ptr = string.end();
	while (p < end_ptr) {
		int codepoint = DecodeUtf8(p, end_ptr);
		if (codepoint == 0) continue;

		int glyph_idx = stbtt_FindGlyphIndex(&face->font_info, codepoint);

		// Kerning
		if (prev_glyph && glyph_idx) {
			pen_x += stbtt_GetGlyphKernAdvance(&face->font_info, prev_glyph, glyph_idx) * ps;
		}

		int adv, lsb;
		stbtt_GetGlyphHMetrics(&face->font_info, glyph_idx, &adv, &lsb);

		// Get or create glyph data in cache
		const SlugGlyphData& gd = glyph_cache_.GetOrCreateGlyph(
		    fs->face_index, &face->font_info, glyph_idx, face->em_scale);

		if (gd.num_curves > 0) {
			// Object-space quad corners (pixels, Y down for RmlUi)
			// RmlUi Y-axis: down. Slug demo Y-axis: up.
			// stb_truetype Y-axis: up (as in the font).
			// We need to flip Y when computing object-space positions.
			float obj_x0 = pen_x + gd.em_xmin / face->em_scale * ps;
			float obj_y0 = base_y - gd.em_ymax / face->em_scale * ps; // flip Y: max -> top
			float obj_x1 = pen_x + gd.em_xmax / face->em_scale * ps;
			float obj_y1 = base_y - gd.em_ymin / face->em_scale * ps; // flip Y: min -> bottom

			float tex_z = AsFloat(gd.packed_glyph_loc);
			float tex_w = AsFloat(gd.packed_band_info);

			// Build 4 SlugVertex entries (BL, BR, TR, TL in object space)
			// Note: RmlUi Y is down, but Slug expects Y up in em-space.
			// The em-space coordinates remain in font coordinate space (Y up).
			// The Jacobian handles the mapping between the two.
			struct Corner {
				float obj_x, obj_y;
				float norm_x, norm_y;
				float em_x, em_y;
			};

			Corner corners[4] = {
			    {obj_x0, obj_y1, -1, 1, gd.em_xmin, gd.em_ymin},  // BL (screen-space bottom-left = em-space bottom-left)
			    {obj_x1, obj_y1, 1, 1, gd.em_xmax, gd.em_ymin},   // BR
			    {obj_x1, obj_y0, 1, -1, gd.em_xmax, gd.em_ymax},  // TR (screen-space top-right = em-space top-right)
			    {obj_x0, obj_y0, -1, -1, gd.em_xmin, gd.em_ymax}, // TL
			};

			uint32_t base_v = static_cast<uint32_t>(batch->vertices.size());

			for (int v = 0; v < 4; v++) {
				SlugVertex sv{};
				sv.pos[0] = corners[v].obj_x;
				sv.pos[1] = corners[v].obj_y;
				sv.pos[2] = corners[v].norm_x;
				sv.pos[3] = corners[v].norm_y;
				sv.tex[0] = corners[v].em_x;
				sv.tex[1] = corners[v].em_y;
				sv.tex[2] = tex_z;
				sv.tex[3] = tex_w;
				sv.jac[0] = jac_s;
				sv.jac[1] = 0;
				sv.jac[2] = 0;
				sv.jac[3] = -jac_s; // Negative because screen Y is flipped relative to em Y
				sv.bnd[0] = gd.band_scale_x;
				sv.bnd[1] = gd.band_scale_y;
				sv.bnd[2] = gd.band_offset_x;
				sv.bnd[3] = gd.band_offset_y;
				sv.col[0] = col_r;
				sv.col[1] = col_g;
				sv.col[2] = col_b;
				sv.col[3] = col_a;
				batch->vertices.push_back(sv);
			}

			// Two triangles per quad
			batch->indices.push_back(static_cast<int>(base_v + 0));
			batch->indices.push_back(static_cast<int>(base_v + 1));
			batch->indices.push_back(static_cast<int>(base_v + 2));
			batch->indices.push_back(static_cast<int>(base_v + 0));
			batch->indices.push_back(static_cast<int>(base_v + 2));
			batch->indices.push_back(static_cast<int>(base_v + 3));

			// Also build standard Rml::Vertex quads for layout compatibility
			uint32_t rml_base = static_cast<uint32_t>(rml_mesh.vertices.size());
			for (int v = 0; v < 4; v++) {
				Rml::Vertex rv;
				rv.position = {corners[v].obj_x, corners[v].obj_y};
				rv.colour = colour;
				// Embed magic marker + batch ID in tex_coord
				std::memcpy(&rv.tex_coord.x, &SLUG_MAGIC_U, sizeof(float));
				float batch_id_f;
				std::memcpy(&batch_id_f, &batch_id, sizeof(float));
				rv.tex_coord.y = batch_id_f;
				rml_mesh.vertices.push_back(rv);
			}
			rml_mesh.indices.push_back(static_cast<int>(rml_base + 0));
			rml_mesh.indices.push_back(static_cast<int>(rml_base + 1));
			rml_mesh.indices.push_back(static_cast<int>(rml_base + 2));
			rml_mesh.indices.push_back(static_cast<int>(rml_base + 0));
			rml_mesh.indices.push_back(static_cast<int>(rml_base + 2));
			rml_mesh.indices.push_back(static_cast<int>(rml_base + 3));

			glyph_count++;
		}

		pen_x += adv * ps;
		pen_x += ctx.letter_spacing;
		prev_glyph = glyph_idx;
	}

	if (glyph_count > 0) {
		// Register the pending batch
		pending_batches_[batch_id] = std::move(batch);

		Rml::TexturedMesh tm;
		tm.mesh = std::move(rml_mesh);
		tm.texture = marker_texture_source_.GetTexture(render_manager);
		mesh_list.push_back(std::move(tm));
	}

	return static_cast<int>(std::ceil(pen_x - position.x));
}

int SlugFontEngine::GetVersion(Rml::FontFaceHandle /*handle*/) {
	return version_;
}

void SlugFontEngine::Shutdown() {
	// Release cached CallbackTexture entries before RenderManagers are destroyed.
	marker_texture_source_ = Rml::CallbackTextureSource{};
}

void SlugFontEngine::ReleaseFontResources() {
	faces_.clear();
	face_sizes_.clear();
	pending_batches_.clear();
	glyph_cache_.Clear();
	fallback_face_index_ = -1;
	version_++;
}

SlugBatchData* SlugFontEngine::ConsumePendingBatch(uint32_t batch_id) {
	auto it = pending_batches_.find(batch_id);
	if (it == pending_batches_.end())
		return nullptr;
	return it->second.get();
}
