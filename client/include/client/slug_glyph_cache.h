#pragma once

// Slug GPU font rendering — glyph cache
// Manages curve and band textures for the Slug algorithm.
// Processes glyph outlines from stb_truetype into GPU texture data.

#include <cstdint>
#include <map>
#include <vector>

struct stbtt_fontinfo;

// Per-glyph data needed to build SlugVertex entries
struct SlugGlyphData {
	// Em-space bounding box (padded)
	float em_xmin, em_ymin, em_xmax, em_ymax;

	// Location in curve texture
	int curve_tex_x, curve_tex_y;

	// Location in band texture
	int glyph_loc_x, glyph_loc_y;

	// Packed values for SlugVertex tex.zw
	uint32_t packed_glyph_loc; // tex.z as uint bits: x | (y << 16)
	uint32_t packed_band_info; // tex.w as uint bits: bandMaxX | (bandMaxY << 16)

	// Band transform: maps em-space to band index
	float band_scale_x, band_scale_y;
	float band_offset_x, band_offset_y;

	// Number of curves (0 = glyph has no outline, e.g., space)
	int num_curves;
};

class SlugGlyphCache {
public:
	static constexpr int kCurveTexWidth = 4096;
	static constexpr int kBandTexWidth = 4096;
	static constexpr int kTexHeight = 512;
	static constexpr int kNumBands = 4;

	SlugGlyphCache();
	~SlugGlyphCache();

	// Process a glyph and add to cache. Returns cached data.
	// font_id: unique ID per font face (index into face array)
	// glyph_index: stb_truetype glyph index
	// em_scale: 1.0 / (ascent - descent) from stb_truetype
	const SlugGlyphData& GetOrCreateGlyph(int font_id, stbtt_fontinfo* font,
	                                       int glyph_index, float em_scale);

	// Get CPU texture data pointers (for GPU upload)
	const float* GetCurveTexData() const { return curve_tex_data_.data(); }
	const uint32_t* GetBandTexData() const { return band_tex_data_.data(); }
	int GetCurveTexWidth() const { return kCurveTexWidth; }
	int GetBandTexWidth() const { return kBandTexWidth; }
	int GetTexHeight() const { return kTexHeight; }

	// Returns true if new glyphs were added since last call to ClearDirty()
	bool IsDirty() const { return dirty_; }
	void ClearDirty() { dirty_ = false; }

	void Clear();

private:
	struct Curve {
		float p1x, p1y, p2x, p2y, p3x, p3y;
	};

	struct GlyphInfo {
		std::vector<Curve> curves;
		float em_xmin, em_ymin, em_xmax, em_ymax;
		int curve_tex_x, curve_tex_y;
		int glyph_loc_x, glyph_loc_y;
		int total_band_texels;
		std::vector<std::vector<int>> h_bands, v_bands;
	};

	struct CacheKey {
		int font_id;
		int glyph_index;
		bool operator<(const CacheKey& o) const {
			if (font_id != o.font_id) return font_id < o.font_id;
			return glyph_index < o.glyph_index;
		}
	};

	GlyphInfo ProcessGlyph(stbtt_fontinfo* font, int glyph_index, float em_scale);
	void AllocCurveSpace(GlyphInfo& gi);
	void AllocBandSpace(GlyphInfo& gi);

	std::map<CacheKey, SlugGlyphData> cache_;

	// CPU-side texture data
	std::vector<float> curve_tex_data_;    // RGBA32F, 4 floats per texel
	std::vector<uint32_t> band_tex_data_;  // RGBA32UI, 4 uint32s per texel

	// Allocation cursors
	int curve_alloc_x_ = 0, curve_alloc_y_ = 0;
	int band_alloc_x_ = 0, band_alloc_y_ = 0;

	bool dirty_ = false;
};
