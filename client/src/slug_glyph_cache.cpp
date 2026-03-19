// Slug GPU font rendering — glyph cache implementation
// Ports glyph processing logic from the Slug demo (font-rendering/demo/main.cpp).

#include <client/slug_glyph_cache.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#include "font/stb_truetype.h"

SlugGlyphCache::SlugGlyphCache() {
	curve_tex_data_.resize(kCurveTexWidth * kTexHeight * 4, 0.0f);
	band_tex_data_.resize(kBandTexWidth * kTexHeight * 4, 0);
}

SlugGlyphCache::~SlugGlyphCache() = default;

void SlugGlyphCache::Clear() {
	cache_.clear();
	std::fill(curve_tex_data_.begin(), curve_tex_data_.end(), 0.0f);
	std::fill(band_tex_data_.begin(), band_tex_data_.end(), 0u);
	curve_alloc_x_ = curve_alloc_y_ = 0;
	band_alloc_x_ = band_alloc_y_ = 0;
	dirty_ = true;
}

const SlugGlyphData& SlugGlyphCache::GetOrCreateGlyph(
    int font_id, stbtt_fontinfo* font, int glyph_index, float em_scale) {

	CacheKey key{font_id, glyph_index};
	auto it = cache_.find(key);
	if (it != cache_.end())
		return it->second;

	GlyphInfo gi = ProcessGlyph(font, glyph_index, em_scale);

	SlugGlyphData data{};
	data.num_curves = static_cast<int>(gi.curves.size());

	if (!gi.curves.empty()) {
		AllocCurveSpace(gi);
		AllocBandSpace(gi);

		data.em_xmin = gi.em_xmin;
		data.em_ymin = gi.em_ymin;
		data.em_xmax = gi.em_xmax;
		data.em_ymax = gi.em_ymax;
		data.curve_tex_x = gi.curve_tex_x;
		data.curve_tex_y = gi.curve_tex_y;
		data.glyph_loc_x = gi.glyph_loc_x;
		data.glyph_loc_y = gi.glyph_loc_y;

		data.packed_glyph_loc = static_cast<uint32_t>(gi.glyph_loc_x) |
		                        (static_cast<uint32_t>(gi.glyph_loc_y) << 16);
		uint32_t band_max_x = kNumBands - 1;
		uint32_t band_max_y = kNumBands - 1;
		data.packed_band_info = band_max_x | (band_max_y << 16);

		float em_w = gi.em_xmax - gi.em_xmin;
		float em_h = gi.em_ymax - gi.em_ymin;
		data.band_scale_x = static_cast<float>(kNumBands) / em_w;
		data.band_scale_y = static_cast<float>(kNumBands) / em_h;
		data.band_offset_x = -gi.em_xmin * data.band_scale_x;
		data.band_offset_y = -gi.em_ymin * data.band_scale_y;

		dirty_ = true;
	}

	auto [ins_it, _] = cache_.emplace(key, data);
	return ins_it->second;
}

SlugGlyphCache::GlyphInfo SlugGlyphCache::ProcessGlyph(
    stbtt_fontinfo* font, int glyph_index, float em_scale) {

	GlyphInfo gi{};

	stbtt_vertex* verts = nullptr;
	int nv = stbtt_GetGlyphShape(font, glyph_index, &verts);

	float px = 0, py = 0;
	for (int i = 0; i < nv; i++) {
		float x = verts[i].x * em_scale;
		float y = verts[i].y * em_scale;
		switch (verts[i].type) {
		case STBTT_vmove:
			px = x;
			py = y;
			break;
		case STBTT_vline: {
			// Convert line to degenerate quadratic (midpoint as control point)
			Curve c = {px, py, (px + x) * 0.5f, (py + y) * 0.5f, x, y};
			gi.curves.push_back(c);
			px = x;
			py = y;
		} break;
		case STBTT_vcurve: {
			float cx = verts[i].cx * em_scale;
			float cy = verts[i].cy * em_scale;
			gi.curves.push_back({px, py, cx, cy, x, y});
			px = x;
			py = y;
		} break;
		case STBTT_vcubic: {
			// Approximate cubic with two quadratics via midpoint subdivision
			float cx1 = verts[i].cx * em_scale, cy1 = verts[i].cy * em_scale;
			float cx2 = verts[i].cx1 * em_scale, cy2 = verts[i].cy1 * em_scale;
			float mx = (px + 3 * cx1 + 3 * cx2 + x) * 0.125f;
			float my = (py + 3 * cy1 + 3 * cy2 + y) * 0.125f;
			float q1x = (px + cx1) * 0.5f, q1y = (py + cy1) * 0.5f;
			float q2x = (cx2 + x) * 0.5f, q2y = (cy2 + y) * 0.5f;
			gi.curves.push_back({px, py, q1x, q1y, mx, my});
			gi.curves.push_back({mx, my, q2x, q2y, x, y});
			px = x;
			py = y;
		} break;
		}
	}
	if (verts) stbtt_FreeShape(font, verts);

	if (gi.curves.empty()) {
		gi.em_xmin = gi.em_ymin = gi.em_xmax = gi.em_ymax = 0;
		gi.total_band_texels = 0;
		return gi;
	}

	// Compute bounding box from curve control points
	gi.em_xmin = gi.em_ymin = 1e10f;
	gi.em_xmax = gi.em_ymax = -1e10f;
	for (auto& c : gi.curves) {
		gi.em_xmin = (std::min)({gi.em_xmin, c.p1x, c.p2x, c.p3x});
		gi.em_ymin = (std::min)({gi.em_ymin, c.p1y, c.p2y, c.p3y});
		gi.em_xmax = (std::max)({gi.em_xmax, c.p1x, c.p2x, c.p3x});
		gi.em_ymax = (std::max)({gi.em_ymax, c.p1y, c.p2y, c.p3y});
	}
	float pad = (std::max)(gi.em_xmax - gi.em_xmin, gi.em_ymax - gi.em_ymin) * 0.02f;
	gi.em_xmin -= pad;
	gi.em_ymin -= pad;
	gi.em_xmax += pad;
	gi.em_ymax += pad;

	float em_w = gi.em_xmax - gi.em_xmin;
	float em_h = gi.em_ymax - gi.em_ymin;
	int nh = kNumBands, nv_b = kNumBands;
	gi.h_bands.resize(nh);
	gi.v_bands.resize(nv_b);

	// Assign curves to bands
	for (int ci = 0; ci < static_cast<int>(gi.curves.size()); ci++) {
		const Curve& c = gi.curves[ci];
		float c_min_y = (std::min)({c.p1y, c.p2y, c.p3y});
		float c_max_y = (std::max)({c.p1y, c.p2y, c.p3y});
		float c_min_x = (std::min)({c.p1x, c.p2x, c.p3x});
		float c_max_x = (std::max)({c.p1x, c.p2x, c.p3x});

		for (int b = 0; b < nh; b++) {
			float by0 = gi.em_ymin + b * em_h / nh;
			float by1 = gi.em_ymin + (b + 1) * em_h / nh;
			if (c_min_y <= by1 && c_max_y >= by0)
				gi.h_bands[b].push_back(ci);
		}
		for (int b = 0; b < nv_b; b++) {
			float bx0 = gi.em_xmin + b * em_w / nv_b;
			float bx1 = gi.em_xmin + (b + 1) * em_w / nv_b;
			if (c_min_x <= bx1 && c_max_x >= bx0)
				gi.v_bands[b].push_back(ci);
		}
	}

	// Sort: h-band curves by descending max X, v-band curves by descending max Y
	for (auto& band : gi.h_bands)
		std::sort(band.begin(), band.end(), [&](int a, int b) {
			return (std::max)({gi.curves[a].p1x, gi.curves[a].p2x, gi.curves[a].p3x}) >
			       (std::max)({gi.curves[b].p1x, gi.curves[b].p2x, gi.curves[b].p3x});
		});
	for (auto& band : gi.v_bands)
		std::sort(band.begin(), band.end(), [&](int a, int b) {
			return (std::max)({gi.curves[a].p1y, gi.curves[a].p2y, gi.curves[a].p3y}) >
			       (std::max)({gi.curves[b].p1y, gi.curves[b].p2y, gi.curves[b].p3y});
		});

	int total = nh + nv_b;
	for (auto& band : gi.h_bands) total += static_cast<int>(band.size());
	for (auto& band : gi.v_bands) total += static_cast<int>(band.size());
	gi.total_band_texels = total;

	return gi;
}

void SlugGlyphCache::AllocCurveSpace(GlyphInfo& gi) {
	int need = static_cast<int>(gi.curves.size()) * 2;
	if (need == 0) return;

	if (curve_alloc_x_ + need > kCurveTexWidth) {
		curve_alloc_x_ = 0;
		curve_alloc_y_++;
	}
	gi.curve_tex_x = curve_alloc_x_;
	gi.curve_tex_y = curve_alloc_y_;

	for (int i = 0; i < static_cast<int>(gi.curves.size()); i++) {
		const Curve& c = gi.curves[i];
		int tx = gi.curve_tex_x + i * 2;
		int ty = gi.curve_tex_y;
		int i0 = (ty * kCurveTexWidth + tx) * 4;
		int i1 = i0 + 4;
		curve_tex_data_[i0 + 0] = c.p1x;
		curve_tex_data_[i0 + 1] = c.p1y;
		curve_tex_data_[i0 + 2] = c.p2x;
		curve_tex_data_[i0 + 3] = c.p2y;
		curve_tex_data_[i1 + 0] = c.p3x;
		curve_tex_data_[i1 + 1] = c.p3y;
		curve_tex_data_[i1 + 2] = 0;
		curve_tex_data_[i1 + 3] = 0;
	}

	curve_alloc_x_ += need;
}

void SlugGlyphCache::AllocBandSpace(GlyphInfo& gi) {
	if (gi.total_band_texels == 0) return;

	if (band_alloc_x_ + gi.total_band_texels > kBandTexWidth) {
		band_alloc_x_ = 0;
		band_alloc_y_++;
	}
	gi.glyph_loc_x = band_alloc_x_;
	gi.glyph_loc_y = band_alloc_y_;

	int gx = gi.glyph_loc_x, gy = gi.glyph_loc_y;
	int nh = kNumBands, nvb = kNumBands;
	int list_off = nh + nvb; // offset from glyphLoc to first curve list

	auto setB = [&](int off, uint32_t r, uint32_t g_val, uint32_t b_val, uint32_t a) {
		int lx = gx + off;
		int ly = gy;
		ly += lx >> 12;
		lx &= 0xFFF;
		int idx = (ly * kBandTexWidth + lx) * 4;
		band_tex_data_[idx + 0] = r;
		band_tex_data_[idx + 1] = g_val;
		band_tex_data_[idx + 2] = b_val;
		band_tex_data_[idx + 3] = a;
	};

	// Horizontal band headers + curve location lists
	for (int b = 0; b < nh; b++) {
		setB(b, static_cast<uint32_t>(gi.h_bands[b].size()),
		     static_cast<uint32_t>(list_off), 0, 0);
		for (int ci = 0; ci < static_cast<int>(gi.h_bands[b].size()); ci++) {
			int curve_idx = gi.h_bands[b][ci];
			setB(list_off + ci,
			     static_cast<uint32_t>(gi.curve_tex_x + curve_idx * 2),
			     static_cast<uint32_t>(gi.curve_tex_y), 0, 0);
		}
		list_off += static_cast<int>(gi.h_bands[b].size());
	}

	// Vertical band headers + curve location lists
	for (int b = 0; b < nvb; b++) {
		setB(nh + b, static_cast<uint32_t>(gi.v_bands[b].size()),
		     static_cast<uint32_t>(list_off), 0, 0);
		for (int ci = 0; ci < static_cast<int>(gi.v_bands[b].size()); ci++) {
			int curve_idx = gi.v_bands[b][ci];
			setB(list_off + ci,
			     static_cast<uint32_t>(gi.curve_tex_x + curve_idx * 2),
			     static_cast<uint32_t>(gi.curve_tex_y), 0, 0);
		}
		list_off += static_cast<int>(gi.v_bands[b].size());
	}

	band_alloc_x_ += gi.total_band_texels;
}
